/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Unit tests for libwpo and the core classes. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "osdo.h"
#include "osdo_loop.h"
#include "osd_classes.h"
#include "TBase.h"
#include "OsdDataFile.h"
#include "OsdFolder.h"
#include "OsdProgram.h"
#include "OsdShadow.h"

static const char *plugin_dir;

static char *plugin_path(const char *name)
{
    static char buf[4096];
    snprintf(buf, sizeof buf, "%s/%s", plugin_dir, name);
    return buf;
}

TEST(registration)
{
    CHECK(osdo_find_class("OsdFolder") == OsdFolderClass);
    CHECK(osdo_find_class("M_OsdFolder") == M_OsdFolderClass);
    CHECK(OsdFolder_register() == OSDO_EEXIST);

    /* Bad definitions are refused. */
    static const osdo_class_def orphan = { .name = "Orphan", .parent = "NoSuchClass", .major = 1 };
    CHECK(osdo_register_class(&orphan, NULL) == OSDO_ENOCLASS);
    static const osdo_override_def bad_ov[] = { { "osdNoSuchMethod", (osdo_fn)abort } };
    static const osdo_class_def badov = { .name = "BadOverride", .parent = "OsdObject", .major = 1,
                                         .parent_major = 1, .overrides = bad_ov, .noverrides = 1 };
    CHECK(osdo_register_class(&badov, NULL) == OSDO_ENOMETHOD);
    static const osdo_method_def bad_sig[] = { { "osdX", "q(zz", 0, (osdo_apply_fn)abort, (osdo_fn)abort } };
    static const osdo_class_def badsig = { .name = "BadSig", .parent = "OsdObject", .major = 1,
                                          .parent_major = 1, .methods = bad_sig, .nmethods = 1 };
    CHECK(osdo_register_class(&badsig, NULL) == OSDO_EINVAL);
    static const osdo_method_def dup[] = { { "osdQueryTitle", "s()", 0, (osdo_apply_fn)abort, (osdo_fn)abort } };
    static const osdo_class_def redefine = { .name = "Redefine", .parent = "OsdObject", .major = 1,
                                            .parent_major = 1, .methods = dup, .nmethods = 1 };
    CHECK(osdo_register_class(&redefine, NULL) == OSDO_EEXIST);
    CHECK(osdo_find_class("BadSig") == NULL && osdo_find_class("M_BadSig") == NULL);
}

TEST(metaclasses)
{
    /* Classes are objects; their class is their metaclass. */
    CHECK(OsdFolderClass->hdr.cls == M_OsdFolderClass);
    CHECK(M_OsdFolderClass->parent == M_OsdFileSystemClass);
    CHECK(osdo_is_a(&OsdFolderClass->hdr, OsdoClassClass));
    CHECK(OsdoClassClass->hdr.cls == OsdoClassClass);
    CHECK(osdo_subclass_of(OsdFolderClass, OsdObjectClass));
    CHECK(!osdo_subclass_of(OsdObjectClass, OsdFolderClass));

    /* Class methods dispatch through the metaclass and are inherited. */
    char *t = OsdObject_osdclsQueryTitle(OsdFolderClass);
    CHECK_STR(t, "Folder");
    free(t);
    t = OsdObject_osdclsQueryTitle(OsdAbstractClass);
    CHECK_STR(t, "Abstract Object");
    free(t);
    osdo_value r;
    CHECK(osdo_send(&OsdProgramClass->hdr, "osdclsQueryTitle", NULL, 0, &r) == OSDO_OK);
    CHECK(r.type == 's');
    CHECK_STR(r.u.s, "Program");
    osdo_value_clear(&r);
    /* osdclsNew by name creates an instance. */
    CHECK(osdo_send(&OsdFolderClass->hdr, "osdclsNew", NULL, 0, &r) == OSDO_OK);
    CHECK(r.type == 'o' && r.u.o && r.u.o->cls == OsdFolderClass);
    osdo_value_clear(&r);
}

TEST(layout)
{
    /* Each class's data block follows its parent's, aligned. */
    const osdo_class *chain[] = { OsdObjectClass, OsdFileSystemClass, OsdFolderClass };
    for (int i = 0; i < 3; i++) {
        CHECK(chain[i]->data_offset % 16 == 0);
        CHECK(chain[i]->data_offset >= sizeof(osdo_object));
        if (i) CHECK(chain[i]->data_offset >= chain[i - 1]->data_offset + chain[i - 1]->data_size);
    }
    CHECK(OsdFolderClass->instance_size == OsdFolderClass->data_offset + sizeof(OsdFolderData));
    /* Slots: inherited first, then the class's own. */
    CHECK(OsdFolderClass->mbase == OsdFileSystemClass->nslots);
    CHECK(osdo_method_slot(OsdFolderClass, "osdQueryTitle") == osdo_method_slot(OsdObjectClass, "osdQueryTitle"));
}

TEST(objects_and_static_calls)
{
    osdo_object *f = osdo_new(OsdDataFileClass);
    CHECK(f && f->cls == OsdDataFileClass && f->refs == 1);
    char *t = OsdObject_osdQueryTitle(f);
    CHECK_STR(t, "Data File");   /* no path, no title: the class title */
    free(t);
    CHECK(OsdFileSystem_osdSetPath(f, "/home/me/notes.txt"));
    t = OsdObject_osdQueryTitle(f);
    CHECK_STR(t, "notes.txt");
    free(t);
    t = OsdDataFile_osdQueryMimeType(f);
    CHECK_STR(t, "text/plain");
    free(t);

    /* Lazy identity: no ID until the service stores something. */
    char *h = OsdObject_osdQueryHandle(f);
    CHECK_STR(h, "");
    free(h);
    uint64_t before = OsdObject_data(f)->changes;
    CHECK(OsdObject_osdSetTitle(f, "My notes"));
    t = OsdObject_osdQueryTitle(f);
    CHECK_STR(t, "My notes");
    free(t);
    CHECK(OsdObject_data(f)->dirty == 1);
    CHECK(OsdObject_data(f)->changes == before + 1);
    osdo_unref(f);
}

TEST(parent_calls)
{
    osdo_object *d = osdo_new(OsdFolderClass);
    char *m = OsdObject_osdModifyPopupMenu(d, "");
    /* "Open" is a cascade of the folder's views, as "Open as" in Warp 4. */
    CHECK_STR(m, "1\tOpen\n2\tProperties\n41\tCreate another\n40\tDelete...\n11\tIcon view\t1\n12\tTree view\t1\n13\tDetails view\t1\n14\tColumn view\t1\n20\tArrange\n73\tCommand prompt here\n");
    free(m);
    osdo_unref(d);
    osdo_object *f = osdo_new(OsdDataFileClass);
    m = OsdObject_osdModifyPopupMenu(f, "");
    CHECK_STR(m, "1\tOpen\n2\tProperties\n41\tCreate another\n40\tDelete...\n");   /* (Open With: osdd adds the cascade) */
    free(m);
    osdo_unref(f);
}

TEST(calls_by_name)
{
    osdo_object *p = osdo_new(OsdProgramClass);
    osdo_value args[2], r;
    args[0] = osdo_v_string("firefox --new-window");
    CHECK(osdo_send(p, "osdSetCommand", args, 1, &r) == OSDO_OK);
    CHECK(r.type == 'b' && r.u.b);
    CHECK(osdo_send(p, "osdQueryCommand", NULL, 0, &r) == OSDO_OK);
    CHECK_STR(r.u.s, "firefox --new-window");
    osdo_value_clear(&r);
    /* Inherited method by name. */
    CHECK(osdo_send(p, "osdQueryTitle", NULL, 0, &r) == OSDO_OK);
    CHECK_STR(r.u.s, "Program");
    osdo_value_clear(&r);
    /* Checked signatures. */
    CHECK(osdo_send(p, "osdSetCommand", NULL, 0, &r) == OSDO_EARGS);
    args[1] = osdo_v_int(3);
    CHECK(osdo_send(p, "osdSetCommand", &args[1], 1, &r) == OSDO_EARGS);
    CHECK(osdo_send(p, "osdNoSuchThing", NULL, 0, &r) == OSDO_ENOMETHOD);
    osdo_value_clear(&args[0]);
    /* Method info for clients (the service API describes classes from this). */
    const osdo_method_def *mi = osdo_method_info(OsdProgramClass, "osdSetCommand");
    CHECK(mi && strcmp(mi->sig, "b(s)") == 0);
    mi = osdo_method_info(OsdFolderClass, "osdPopulate");
    CHECK(mi && (mi->flags & OSDO_M_ASYNC));
    osdo_unref(p);
}

static int pending_seen;
static void on_done(osdo_pending *p, const osdo_value *v, void *user)
{
    (void)p;
    *(int *)user = (v->type == 'b' && v->u.b) || v->type == 'i' ? 1 : -1;
}

TEST(pending_results)
{
    osdo_object *o = osdo_new(OsdFolderClass);
    osdo_pending *p = OsdFolder_osdPopulate(o);
    CHECK(osdo_pending_done(p));
    osdo_pending_then(p, on_done, &pending_seen);
    CHECK(pending_seen == 1);
    osdo_pending_unref(p);

    /* Callbacks registered before completion run at completion, in order. */
    osdo_pending *q = osdo_pending_new();
    int seen = 0;
    osdo_pending_then(q, on_done, &seen);
    CHECK(seen == 0 && !osdo_pending_done(q));
    osdo_pending_complete(q, osdo_v_bool(true));
    CHECK(seen == 1);
    osdo_pending_complete(q, osdo_v_bool(false));   /* second completion ignored */
    CHECK(osdo_pending_value(q)->u.b);
    osdo_pending_unref(q);
    osdo_unref(o);
}

TEST(shadows_and_refcounts)
{
    osdo_object *target = osdo_new(OsdDataFileClass);
    OsdFileSystem_osdSetPath(target, "/srv/report.odt");
    osdo_object *s = osdo_new(OsdShadowClass);
    CHECK(OsdShadow_osdSetTarget(s, target));
    CHECK(target->refs == 2);
    char *t = OsdObject_osdQueryTitle(s);
    CHECK_STR(t, "report.odt");   /* a shadow shows its target's title */
    free(t);
    CHECK(!OsdShadow_osdSetTarget(s, s));   /* not itself */
    osdo_object *got = OsdShadow_osdQueryTarget(s);
    CHECK(got == target && target->refs == 3);
    osdo_unref(got);
    osdo_unref(target);                    /* the shadow still holds it */
    CHECK(target->refs == 1);
    osdo_unref(s);                         /* releases the target too (ASan checks the frees) */
}

TEST(versions_and_plugins)
{
    char err[512];
    /* Built against TBase 1.0; TBase 1.1 is installed: loads and works. */
    CHECK(osdo_load_plugin(plugin_path("tchild_v10.so"), err, sizeof err) == OSDO_OK);
    osdo_class *child = osdo_find_class("TChild");
    CHECK(child != NULL);
    if (child) {
        osdo_object *o = osdo_new(child);
        CHECK(TBase_tGetA(o) == 5);       /* set by the child's init, via TBase 1.0's slot */
        CHECK(TBase_tGetB(o) == 42);      /* method and data added in 1.1 */
        osdo_value r;
        CHECK(osdo_send(o, "tGetC", NULL, 0, &r) == OSDO_OK && r.u.i == 77);
        CHECK(osdo_send(o, "tDescribe", NULL, 0, &r) == OSDO_OK);
        CHECK_STR(r.u.s, "base a=5 b=42 child c=77");
        osdo_value_clear(&r);
        /* TBase 1.1's larger data block did not overlap the child's. */
        CHECK(child->data_offset >= TBaseClass->data_offset + TBaseClass->data_size);
        for (int i = 0; i < 40; i++) CHECK((unsigned char)TBase_data(o)->pad[i] == 0xAB);
        osdo_unref(o);
    }
    /* Built against TBase 1.2, newer than installed: refused. */
    CHECK(osdo_load_plugin(plugin_path("tchild_v12.so"), err, sizeof err) == OSDO_EVERSION);
    /* Built against TBase 2.0, another major version: refused. */
    CHECK(osdo_load_plugin(plugin_path("tchild_v20.so"), err, sizeof err) == OSDO_EVERSION);
    /* A missing plug-in is an error, not a crash. */
    CHECK(osdo_load_plugin(plugin_path("nonexistent.so"), err, sizeof err) == OSDO_EPLUGIN);
}

TEST(class_replacement)
{
    char err[512];
    CHECK(osdo_load_plugin(plugin_path("myfolder.so"), err, sizeof err) == OSDO_OK);
    CHECK(osdo_replace_class("OsdFolder", "OsdProgram") == OSDO_ENOTSUBCLASS);
    CHECK(osdo_replace_class("OsdFolder", "MyFolder") == OSDO_OK);
    osdo_object *d = osdo_new(OsdFolderClass);   /* asks for OsdFolder ... */
    CHECK(d && strcmp(d->cls->name, "MyFolder") == 0);   /* ... gets the replacement */
    char *m = OsdObject_osdModifyPopupMenu(d, "");
    CHECK_STR(m, "1\tOpen\n2\tProperties\n41\tCreate another\n40\tDelete...\n11\tIcon view\t1\n12\tTree view\t1\n13\tDetails view\t1\n14\tColumn view\t1\n20\tArrange\n73\tCommand prompt here\n"
                 "90\tMy Plug-in Item\n");
    free(m);
    CHECK(osdo_is_a(d, OsdFolderClass));
    osdo_unref(d);
    /* Exact instantiation still possible, e.g. for the replacement's own use. */
    osdo_object *plain = osdo_instantiate(OsdFolderClass);
    CHECK(plain && plain->cls == OsdFolderClass);
    osdo_unref(plain);
}

/* An asynchronous method in the intended style: the method returns a
 * pending result at once, the I/O runs on a worker, and the result is
 * completed on the loop thread, where objects may be touched. */
struct count_job { osdo_pending *p; osdo_object *folder; int64_t n; };

static void count_work(void *a)      /* worker: no objects touched */
{
    struct count_job *j = a;
    j->n = 0;
    for (int i = 0; i < 100000; i++) j->n += i % 7 == 0;
}

static void count_done(void *a)      /* loop thread */
{
    struct count_job *j = a;
    osdo_pending_complete(j->p, osdo_v_int(j->n));
    osdo_pending_unref(j->p);
    osdo_unref(j->folder);
    free(j);
}

static osdo_pending *start_count(osdo_loop *l, osdo_object *folder)
{
    struct count_job *j = calloc(1, sizeof *j);
    osdo_pending *p = osdo_pending_new();
    j->p = osdo_pending_ref(p);
    j->folder = osdo_ref(folder);
    osdo_defer(l, "count", count_work, count_done, j);
    return p;
}

TEST(async_with_loop)
{
    osdo_loop *l = osdo_loop_new(2);
    osdo_object *d = osdo_new(OsdFolderClass);
    osdo_pending *p = start_count(l, d);
    CHECK(!osdo_pending_done(p));         /* returned before the work ran */
    CHECK(osdo_loop_run_until_idle(l, 5000));
    CHECK(osdo_pending_done(p) && osdo_pending_value(p)->u.i == 14286);
    CHECK(d->refs == 1);                 /* the job released its reference */
    osdo_pending_unref(p);
    osdo_unref(d);
    osdo_loop_free(l);
}

TEST(state_bag)
{
    osdo_object *d = osdo_new(OsdFolderClass);
    osdo_object *child = osdo_new(OsdDataFileClass);
    /* Saving puts each class's fields into its part of the bag. */
    OsdObject_osdSetTitle(d, "Projects");
    OsdFolder_osdSetSortOrder(d, 3);
    CHECK(OsdObject_osdSaveState(d));
    osd_cstate *o = osd_state_class(&OsdObject_data(d)->state, "OsdObject", false);
    osd_cstate *f = osd_state_class(&OsdObject_data(d)->state, "OsdFolder", false);
    CHECK(o && f && o->ntouched == 1 && f->ntouched == 1);
    /* Saving again with nothing changed touches nothing new. */
    osd_state_clear_touched(o);
    osd_state_clear_touched(f);
    CHECK(OsdObject_osdSaveState(d));
    CHECK(o->ntouched == 0 && f->ntouched == 0);
    /* Icon positions need the child's ID (assigned by the service). */
    CHECK(!OsdFolder_osdSetIconPos(d, child, 10, 20));
    snprintf(OsdObject_data(child)->id, sizeof OsdObject_data(child)->id, "%s",
             "11111111-2222-4333-8444-555555555555");
    CHECK(OsdFolder_osdSetIconPos(d, child, 10, 20));
    char *pos = OsdFolder_osdQueryIconPos(d, child);
    CHECK_STR(pos, "10,20");
    free(pos);
    /* Restoring reads the bag back into the fields. */
    osdo_object *d2 = osdo_new(OsdFolderClass);
    osd_state_load(d2, "OsdObject", &o->props);
    osd_state_load(d2, "OsdFolder", &f->props);
    OsdObject_osdRestoreProperties(d2);
    char *t = OsdObject_osdQueryTitle(d2);
    CHECK_STR(t, "Projects");
    free(t);
    CHECK(OsdFolder_osdQuerySortOrder(d2) == 3);
    pos = OsdFolder_osdQueryIconPos(d2, child);
    CHECK_STR(pos, "10,20");
    free(pos);
    osdo_unref(d);
    osdo_unref(d2);
    osdo_unref(child);
}

int main(int argc, char **argv)
{
    plugin_dir = argc > 1 ? argv[1] : "build/tests/plugins";
    osd_mime_init(NULL);   /* shared-mime-info's glob table */
    if (osdo_init() != OSDO_OK || osd_classes_register() != OSDO_OK || TBase_register() != OSDO_OK) {
        fprintf(stderr, "setup failed\n");
        return 1;
    }
    RUN(registration);
    RUN(metaclasses);
    RUN(layout);
    RUN(objects_and_static_calls);
    RUN(parent_calls);
    RUN(calls_by_name);
    RUN(pending_results);
    RUN(shadows_and_refcounts);
    RUN(versions_and_plugins);
    RUN(class_replacement);
    RUN(async_with_loop);
    RUN(state_bag);
    osdo_shutdown();
    return CHECK_SUMMARY();
}
