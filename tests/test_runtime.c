/* Unit tests for libwpo and the core classes. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wpo.h"
#include "wpo_loop.h"
#include "wps_classes.h"
#include "TBase.h"
#include "WPDataFile.h"
#include "WPFolder.h"
#include "WPProgram.h"
#include "WPShadow.h"

static const char *plugin_dir;

static char *plugin_path(const char *name)
{
    static char buf[4096];
    snprintf(buf, sizeof buf, "%s/%s", plugin_dir, name);
    return buf;
}

TEST(registration)
{
    CHECK(wpo_find_class("WPFolder") == WPFolderClass);
    CHECK(wpo_find_class("M_WPFolder") == M_WPFolderClass);
    CHECK(WPFolder_register() == WPO_EEXIST);

    /* Bad definitions are refused. */
    static const wpo_class_def orphan = { .name = "Orphan", .parent = "NoSuchClass", .major = 1 };
    CHECK(wpo_register_class(&orphan, NULL) == WPO_ENOCLASS);
    static const wpo_override_def bad_ov[] = { { "wpNoSuchMethod", (wpo_fn)abort } };
    static const wpo_class_def badov = { .name = "BadOverride", .parent = "WPObject", .major = 1,
                                         .parent_major = 1, .overrides = bad_ov, .noverrides = 1 };
    CHECK(wpo_register_class(&badov, NULL) == WPO_ENOMETHOD);
    static const wpo_method_def bad_sig[] = { { "wpX", "q(zz", 0, (wpo_apply_fn)abort, (wpo_fn)abort } };
    static const wpo_class_def badsig = { .name = "BadSig", .parent = "WPObject", .major = 1,
                                          .parent_major = 1, .methods = bad_sig, .nmethods = 1 };
    CHECK(wpo_register_class(&badsig, NULL) == WPO_EINVAL);
    static const wpo_method_def dup[] = { { "wpQueryTitle", "s()", 0, (wpo_apply_fn)abort, (wpo_fn)abort } };
    static const wpo_class_def redefine = { .name = "Redefine", .parent = "WPObject", .major = 1,
                                            .parent_major = 1, .methods = dup, .nmethods = 1 };
    CHECK(wpo_register_class(&redefine, NULL) == WPO_EEXIST);
    CHECK(wpo_find_class("BadSig") == NULL && wpo_find_class("M_BadSig") == NULL);
}

TEST(metaclasses)
{
    /* Classes are objects; their class is their metaclass. */
    CHECK(WPFolderClass->hdr.cls == M_WPFolderClass);
    CHECK(M_WPFolderClass->parent == M_WPFileSystemClass);
    CHECK(wpo_is_a(&WPFolderClass->hdr, WPOClassClass));
    CHECK(WPOClassClass->hdr.cls == WPOClassClass);
    CHECK(wpo_subclass_of(WPFolderClass, WPObjectClass));
    CHECK(!wpo_subclass_of(WPObjectClass, WPFolderClass));

    /* Class methods dispatch through the metaclass and are inherited. */
    char *t = WPObject_wpclsQueryTitle(WPFolderClass);
    CHECK_STR(t, "Folder");
    free(t);
    t = WPObject_wpclsQueryTitle(WPAbstractClass);
    CHECK_STR(t, "Abstract Object");
    free(t);
    wpo_value r;
    CHECK(wpo_send(&WPProgramClass->hdr, "wpclsQueryTitle", NULL, 0, &r) == WPO_OK);
    CHECK(r.type == 's');
    CHECK_STR(r.u.s, "Program");
    wpo_value_clear(&r);
    /* wpclsNew by name creates an instance. */
    CHECK(wpo_send(&WPFolderClass->hdr, "wpclsNew", NULL, 0, &r) == WPO_OK);
    CHECK(r.type == 'o' && r.u.o && r.u.o->cls == WPFolderClass);
    wpo_value_clear(&r);
}

TEST(layout)
{
    /* Each class's data block follows its parent's, aligned. */
    const wpo_class *chain[] = { WPObjectClass, WPFileSystemClass, WPFolderClass };
    for (int i = 0; i < 3; i++) {
        CHECK(chain[i]->data_offset % 16 == 0);
        CHECK(chain[i]->data_offset >= sizeof(wpo_object));
        if (i) CHECK(chain[i]->data_offset >= chain[i - 1]->data_offset + chain[i - 1]->data_size);
    }
    CHECK(WPFolderClass->instance_size == WPFolderClass->data_offset + sizeof(WPFolderData));
    /* Slots: inherited first, then the class's own. */
    CHECK(WPFolderClass->mbase == WPFileSystemClass->nslots);
    CHECK(wpo_method_slot(WPFolderClass, "wpQueryTitle") == wpo_method_slot(WPObjectClass, "wpQueryTitle"));
}

TEST(objects_and_static_calls)
{
    wpo_object *f = wpo_new(WPDataFileClass);
    CHECK(f && f->cls == WPDataFileClass && f->refs == 1);
    char *t = WPObject_wpQueryTitle(f);
    CHECK_STR(t, "Data File");   /* no path, no title: the class title */
    free(t);
    CHECK(WPFileSystem_wpSetPath(f, "/home/me/notes.txt"));
    t = WPObject_wpQueryTitle(f);
    CHECK_STR(t, "notes.txt");
    free(t);
    t = WPDataFile_wpQueryMimeType(f);
    CHECK_STR(t, "text/plain");
    free(t);

    /* Lazy identity: no ID until the service stores something. */
    char *h = WPObject_wpQueryHandle(f);
    CHECK_STR(h, "");
    free(h);
    uint64_t before = WPObject_data(f)->changes;
    CHECK(WPObject_wpSetTitle(f, "My notes"));
    t = WPObject_wpQueryTitle(f);
    CHECK_STR(t, "My notes");
    free(t);
    CHECK(WPObject_data(f)->dirty == 1);
    CHECK(WPObject_data(f)->changes == before + 1);
    wpo_unref(f);
}

TEST(parent_calls)
{
    wpo_object *d = wpo_new(WPFolderClass);
    char *m = WPObject_wpModifyPopupMenu(d, "");
    /* "Open" is a cascade of the folder's views, as "Open as" in Warp 4. */
    CHECK_STR(m, "1\tOpen\n2\tProperties\n41\tCreate another\n40\tDelete...\n11\tIcon view\t1\n12\tTree view\t1\n13\tDetails view\t1\n20\tArrange\n");
    free(m);
    wpo_unref(d);
    wpo_object *f = wpo_new(WPDataFileClass);
    m = WPObject_wpModifyPopupMenu(f, "");
    CHECK_STR(m, "1\tOpen\n2\tProperties\n41\tCreate another\n40\tDelete...\n");   /* (Open With: wpsd adds the cascade) */
    free(m);
    wpo_unref(f);
}

TEST(calls_by_name)
{
    wpo_object *p = wpo_new(WPProgramClass);
    wpo_value args[2], r;
    args[0] = wpo_v_string("firefox --new-window");
    CHECK(wpo_send(p, "wpSetCommand", args, 1, &r) == WPO_OK);
    CHECK(r.type == 'b' && r.u.b);
    CHECK(wpo_send(p, "wpQueryCommand", NULL, 0, &r) == WPO_OK);
    CHECK_STR(r.u.s, "firefox --new-window");
    wpo_value_clear(&r);
    /* Inherited method by name. */
    CHECK(wpo_send(p, "wpQueryTitle", NULL, 0, &r) == WPO_OK);
    CHECK_STR(r.u.s, "Program");
    wpo_value_clear(&r);
    /* Checked signatures. */
    CHECK(wpo_send(p, "wpSetCommand", NULL, 0, &r) == WPO_EARGS);
    args[1] = wpo_v_int(3);
    CHECK(wpo_send(p, "wpSetCommand", &args[1], 1, &r) == WPO_EARGS);
    CHECK(wpo_send(p, "wpNoSuchThing", NULL, 0, &r) == WPO_ENOMETHOD);
    wpo_value_clear(&args[0]);
    /* Method info for clients (the service API describes classes from this). */
    const wpo_method_def *mi = wpo_method_info(WPProgramClass, "wpSetCommand");
    CHECK(mi && strcmp(mi->sig, "b(s)") == 0);
    mi = wpo_method_info(WPFolderClass, "wpPopulate");
    CHECK(mi && (mi->flags & WPO_M_ASYNC));
    wpo_unref(p);
}

static int pending_seen;
static void on_done(wpo_pending *p, const wpo_value *v, void *user)
{
    (void)p;
    *(int *)user = (v->type == 'b' && v->u.b) || v->type == 'i' ? 1 : -1;
}

TEST(pending_results)
{
    wpo_object *o = wpo_new(WPFolderClass);
    wpo_pending *p = WPFolder_wpPopulate(o);
    CHECK(wpo_pending_done(p));
    wpo_pending_then(p, on_done, &pending_seen);
    CHECK(pending_seen == 1);
    wpo_pending_unref(p);

    /* Callbacks registered before completion run at completion, in order. */
    wpo_pending *q = wpo_pending_new();
    int seen = 0;
    wpo_pending_then(q, on_done, &seen);
    CHECK(seen == 0 && !wpo_pending_done(q));
    wpo_pending_complete(q, wpo_v_bool(true));
    CHECK(seen == 1);
    wpo_pending_complete(q, wpo_v_bool(false));   /* second completion ignored */
    CHECK(wpo_pending_value(q)->u.b);
    wpo_pending_unref(q);
    wpo_unref(o);
}

TEST(shadows_and_refcounts)
{
    wpo_object *target = wpo_new(WPDataFileClass);
    WPFileSystem_wpSetPath(target, "/srv/report.odt");
    wpo_object *s = wpo_new(WPShadowClass);
    CHECK(WPShadow_wpSetTarget(s, target));
    CHECK(target->refs == 2);
    char *t = WPObject_wpQueryTitle(s);
    CHECK_STR(t, "report.odt");   /* a shadow shows its target's title */
    free(t);
    CHECK(!WPShadow_wpSetTarget(s, s));   /* not itself */
    wpo_object *got = WPShadow_wpQueryTarget(s);
    CHECK(got == target && target->refs == 3);
    wpo_unref(got);
    wpo_unref(target);                    /* the shadow still holds it */
    CHECK(target->refs == 1);
    wpo_unref(s);                         /* releases the target too (ASan checks the frees) */
}

TEST(versions_and_plugins)
{
    char err[512];
    /* Built against TBase 1.0; TBase 1.1 is installed: loads and works. */
    CHECK(wpo_load_plugin(plugin_path("tchild_v10.so"), err, sizeof err) == WPO_OK);
    wpo_class *child = wpo_find_class("TChild");
    CHECK(child != NULL);
    if (child) {
        wpo_object *o = wpo_new(child);
        CHECK(TBase_tGetA(o) == 5);       /* set by the child's init, via TBase 1.0's slot */
        CHECK(TBase_tGetB(o) == 42);      /* method and data added in 1.1 */
        wpo_value r;
        CHECK(wpo_send(o, "tGetC", NULL, 0, &r) == WPO_OK && r.u.i == 77);
        CHECK(wpo_send(o, "tDescribe", NULL, 0, &r) == WPO_OK);
        CHECK_STR(r.u.s, "base a=5 b=42 child c=77");
        wpo_value_clear(&r);
        /* TBase 1.1's larger data block did not overlap the child's. */
        CHECK(child->data_offset >= TBaseClass->data_offset + TBaseClass->data_size);
        for (int i = 0; i < 40; i++) CHECK((unsigned char)TBase_data(o)->pad[i] == 0xAB);
        wpo_unref(o);
    }
    /* Built against TBase 1.2, newer than installed: refused. */
    CHECK(wpo_load_plugin(plugin_path("tchild_v12.so"), err, sizeof err) == WPO_EVERSION);
    /* Built against TBase 2.0, another major version: refused. */
    CHECK(wpo_load_plugin(plugin_path("tchild_v20.so"), err, sizeof err) == WPO_EVERSION);
    /* A missing plug-in is an error, not a crash. */
    CHECK(wpo_load_plugin(plugin_path("nonexistent.so"), err, sizeof err) == WPO_EPLUGIN);
}

TEST(class_replacement)
{
    char err[512];
    CHECK(wpo_load_plugin(plugin_path("myfolder.so"), err, sizeof err) == WPO_OK);
    CHECK(wpo_replace_class("WPFolder", "WPProgram") == WPO_ENOTSUBCLASS);
    CHECK(wpo_replace_class("WPFolder", "MyFolder") == WPO_OK);
    wpo_object *d = wpo_new(WPFolderClass);   /* asks for WPFolder ... */
    CHECK(d && strcmp(d->cls->name, "MyFolder") == 0);   /* ... gets the replacement */
    char *m = WPObject_wpModifyPopupMenu(d, "");
    CHECK_STR(m, "1\tOpen\n2\tProperties\n41\tCreate another\n40\tDelete...\n11\tIcon view\t1\n12\tTree view\t1\n13\tDetails view\t1\n20\tArrange\n"
                 "90\tMy Plug-in Item\n");
    free(m);
    CHECK(wpo_is_a(d, WPFolderClass));
    wpo_unref(d);
    /* Exact instantiation still possible, e.g. for the replacement's own use. */
    wpo_object *plain = wpo_instantiate(WPFolderClass);
    CHECK(plain && plain->cls == WPFolderClass);
    wpo_unref(plain);
}

/* An asynchronous method in the intended style: the method returns a
 * pending result at once, the I/O runs on a worker, and the result is
 * completed on the loop thread, where objects may be touched. */
struct count_job { wpo_pending *p; wpo_object *folder; int64_t n; };

static void count_work(void *a)      /* worker: no objects touched */
{
    struct count_job *j = a;
    j->n = 0;
    for (int i = 0; i < 100000; i++) j->n += i % 7 == 0;
}

static void count_done(void *a)      /* loop thread */
{
    struct count_job *j = a;
    wpo_pending_complete(j->p, wpo_v_int(j->n));
    wpo_pending_unref(j->p);
    wpo_unref(j->folder);
    free(j);
}

static wpo_pending *start_count(wpo_loop *l, wpo_object *folder)
{
    struct count_job *j = calloc(1, sizeof *j);
    wpo_pending *p = wpo_pending_new();
    j->p = wpo_pending_ref(p);
    j->folder = wpo_ref(folder);
    wpo_defer(l, "count", count_work, count_done, j);
    return p;
}

TEST(async_with_loop)
{
    wpo_loop *l = wpo_loop_new(2);
    wpo_object *d = wpo_new(WPFolderClass);
    wpo_pending *p = start_count(l, d);
    CHECK(!wpo_pending_done(p));         /* returned before the work ran */
    CHECK(wpo_loop_run_until_idle(l, 5000));
    CHECK(wpo_pending_done(p) && wpo_pending_value(p)->u.i == 14286);
    CHECK(d->refs == 1);                 /* the job released its reference */
    wpo_pending_unref(p);
    wpo_unref(d);
    wpo_loop_free(l);
}

TEST(state_bag)
{
    wpo_object *d = wpo_new(WPFolderClass);
    wpo_object *child = wpo_new(WPDataFileClass);
    /* Saving puts each class's fields into its part of the bag. */
    WPObject_wpSetTitle(d, "Projects");
    WPFolder_wpSetSortOrder(d, 3);
    CHECK(WPObject_wpSaveState(d));
    wps_cstate *o = wps_state_class(&WPObject_data(d)->state, "WPObject", false);
    wps_cstate *f = wps_state_class(&WPObject_data(d)->state, "WPFolder", false);
    CHECK(o && f && o->ntouched == 1 && f->ntouched == 1);
    /* Saving again with nothing changed touches nothing new. */
    wps_state_clear_touched(o);
    wps_state_clear_touched(f);
    CHECK(WPObject_wpSaveState(d));
    CHECK(o->ntouched == 0 && f->ntouched == 0);
    /* Icon positions need the child's ID (assigned by the service). */
    CHECK(!WPFolder_wpSetIconPos(d, child, 10, 20));
    snprintf(WPObject_data(child)->id, sizeof WPObject_data(child)->id, "%s",
             "11111111-2222-4333-8444-555555555555");
    CHECK(WPFolder_wpSetIconPos(d, child, 10, 20));
    char *pos = WPFolder_wpQueryIconPos(d, child);
    CHECK_STR(pos, "10,20");
    free(pos);
    /* Restoring reads the bag back into the fields. */
    wpo_object *d2 = wpo_new(WPFolderClass);
    wps_state_load(d2, "WPObject", &o->props);
    wps_state_load(d2, "WPFolder", &f->props);
    WPObject_wpRestoreProperties(d2);
    char *t = WPObject_wpQueryTitle(d2);
    CHECK_STR(t, "Projects");
    free(t);
    CHECK(WPFolder_wpQuerySortOrder(d2) == 3);
    pos = WPFolder_wpQueryIconPos(d2, child);
    CHECK_STR(pos, "10,20");
    free(pos);
    wpo_unref(d);
    wpo_unref(d2);
    wpo_unref(child);
}

int main(int argc, char **argv)
{
    plugin_dir = argc > 1 ? argv[1] : "build/tests/plugins";
    wps_mime_init(NULL);   /* shared-mime-info's glob table */
    if (wpo_init() != WPO_OK || wps_classes_register() != WPO_OK || TBase_register() != WPO_OK) {
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
    wpo_shutdown();
    return CHECK_SUMMARY();
}
