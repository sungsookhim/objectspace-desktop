/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * libwpo core: classes, metaclasses, method tables, dispatch, replacement,
 * pending results. Uses only the C library.
 */
#include "osdo.h"

#include <stdlib.h>
#include <string.h>

struct osdo_name_ent {
    const char *name;
    uint32_t slot;
};

osdo_class *OsdoRootClass;
osdo_class *OsdoClassClass;

static osdo_class **registry;
static size_t nclasses, cap_classes;

/* ------------------------------------------------------------- helpers */

const char *osdo_strerror(osdo_status st)
{
    switch (st) {
    case OSDO_OK: return "ok";
    case OSDO_ENOMEM: return "out of memory";
    case OSDO_ENOCLASS: return "no such class";
    case OSDO_EEXIST: return "already exists";
    case OSDO_ENOMETHOD: return "no such method";
    case OSDO_EARGS: return "wrong arguments";
    case OSDO_EVERSION: return "incompatible class version";
    case OSDO_ENOTSUBCLASS: return "not a subclass";
    case OSDO_EPLUGIN: return "plug-in failed";
    case OSDO_EINVAL: return "invalid argument";
    }
    return "unknown error";
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static size_t align16(size_t n) { return (n + 15u) & ~(size_t)15u; }

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

/* A signature is "R(A...)": R one of vbidsop, each A one of bidso. */
static int sig_nargs(const char *sig)
{
    if (!sig || !strchr("vbidsop", sig[0]) || sig[0] == 0 || sig[1] != '(') return -1;
    int n = 0;
    const char *p = sig + 2;
    for (; *p && *p != ')'; p++, n++)
        if (!strchr("bidso", *p)) return -1;
    if (*p != ')' || p[1] != 0) return -1;
    return n;
}

static int names_find(const osdo_class *c, const char *name)
{
    if (!c->names_cap) return -1;
    uint32_t mask = c->names_cap - 1;
    for (uint32_t i = hash(name) & mask;; i = (i + 1) & mask) {
        if (!c->names[i].name) return -1;
        if (strcmp(c->names[i].name, name) == 0) return (int)c->names[i].slot;
    }
}

static void names_put(osdo_class *c, const char *name, uint32_t slot)
{
    uint32_t mask = c->names_cap - 1;
    uint32_t i = hash(name) & mask;
    while (c->names[i].name) i = (i + 1) & mask;
    c->names[i].name = name;
    c->names[i].slot = slot;
}

static osdo_status registry_add(osdo_class *c)
{
    if (nclasses == cap_classes) {
        size_t ncap = cap_classes ? cap_classes * 2 : 32;
        osdo_class **n = realloc(registry, ncap * sizeof *n);
        if (!n) return OSDO_ENOMEM;
        registry = n;
        cap_classes = ncap;
    }
    registry[nclasses++] = c;
    return OSDO_OK;
}

static void class_free(osdo_class *c)
{
    free(c->mtab);
    free(c->minfo);
    free(c->names);
    if (c->owns_name) free((char *)c->name);
    free(c);
}

/*
 * Build one class object. `meta` is its metaclass (NULL during bootstrap).
 * Inherits the parent's slots and names, appends `methods`, then applies
 * `overrides`.
 */
static osdo_status build(const char *name, osdo_class *parent, osdo_class *meta, bool is_meta,
                        size_t data_size,
                        const osdo_method_def *methods, size_t nm,
                        const osdo_override_def *ov, size_t nov, osdo_class **out)
{
    size_t objsize = meta ? meta->instance_size : sizeof(osdo_class);
    if (objsize < sizeof(osdo_class)) objsize = sizeof(osdo_class);
    osdo_class *c = calloc(1, objsize);
    if (!c) return OSDO_ENOMEM;
    c->hdr.cls = meta;
    c->hdr.refs = 1;
    c->name = name;
    c->parent = parent;
    c->is_meta = is_meta;
    c->depth = parent ? parent->depth + 1 : 0;

    /* Instance layout: header, then each class's data block in order. The
     * first metaclass (OsdoClass) starts after the whole class struct,
     * because its instances are classes. */
    size_t base = parent ? parent->instance_size : sizeof(osdo_object);
    if (is_meta && (!parent || !parent->is_meta) && base < sizeof(osdo_class))
        base = sizeof(osdo_class);
    c->data_offset = align16(base);
    c->data_size = data_size;
    c->instance_size = c->data_offset + data_size;

    uint32_t inherited = parent ? parent->nslots : 0;
    c->mbase = inherited;
    c->nmethods = (uint32_t)nm;
    c->nslots = inherited + (uint32_t)nm;
    c->mtab = calloc(c->nslots ? c->nslots : 1, sizeof *c->mtab);
    c->minfo = calloc(c->nslots ? c->nslots : 1, sizeof *c->minfo);
    c->names_cap = 8;
    while (c->names_cap < 2 * c->nslots + 1) c->names_cap *= 2;
    c->names = calloc(c->names_cap, sizeof *c->names);
    if (!c->mtab || !c->minfo || !c->names) { class_free(c); return OSDO_ENOMEM; }

    if (parent) {
        memcpy(c->mtab, parent->mtab, inherited * sizeof *c->mtab);
        memcpy(c->minfo, parent->minfo, inherited * sizeof *c->minfo);
        for (uint32_t s = 0; s < inherited; s++) names_put(c, parent->minfo[s]->name, s);
    }
    for (size_t i = 0; i < nm; i++) {
        if (sig_nargs(methods[i].sig) < 0 || !methods[i].apply || !methods[i].impl) {
            class_free(c);
            return OSDO_EINVAL;
        }
        if (names_find(c, methods[i].name) >= 0) {   /* must override, not redefine */
            class_free(c);
            return OSDO_EEXIST;
        }
        uint32_t s = inherited + (uint32_t)i;
        c->mtab[s] = methods[i].impl;
        c->minfo[s] = &methods[i];
        names_put(c, methods[i].name, s);
    }
    for (size_t i = 0; i < nov; i++) {
        int s = names_find(c, ov[i].name);
        if (s < 0 || (uint32_t)s >= inherited || !ov[i].impl) {
            class_free(c);
            return OSDO_ENOMETHOD;
        }
        c->mtab[s] = ov[i].impl;
    }
    *out = c;
    return OSDO_OK;
}

/* ------------------------------------------------- built-in root methods */

static void root_init(osdo_object *self) { (void)self; }
static void root_uninit(osdo_object *self) { (void)self; }

static osdo_status apply_void0(osdo_fn fn, osdo_object *self, const osdo_value *a, osdo_value *ret)
{
    (void)a;
    ((void (*)(osdo_object *))fn)(self);
    *ret = osdo_v_void();
    return OSDO_OK;
}

static osdo_object *class_new(osdo_object *self)
{
    return osdo_instantiate(osdo_resolve_class((osdo_class *)self));
}

static osdo_status apply_obj0(osdo_fn fn, osdo_object *self, const osdo_value *a, osdo_value *ret)
{
    (void)a;
    *ret = osdo_v_object_take(((osdo_object *(*)(osdo_object *))fn)(self));
    return OSDO_OK;
}

static const osdo_method_def root_methods[] = {
    { "osdInitData", "v()", 0, apply_void0, (osdo_fn)root_init },
    { "osdUnInitData", "v()", 0, apply_void0, (osdo_fn)root_uninit },
};
static const osdo_method_def class_methods[] = {
    { "osdclsNew", "o()", 0, apply_obj0, (osdo_fn)class_new },
};

osdo_status osdo_init(void)
{
    if (OsdoRootClass) return OSDO_OK;
    osdo_class *root, *metaroot;
    osdo_status st = build("OsdoRoot", NULL, NULL, false, 0, root_methods, 2, NULL, 0, &root);
    if (st) return st;
    st = build("OsdoClass", root, NULL, true, 0, class_methods, 1, NULL, 0, &metaroot);
    if (st) { class_free(root); return st; }
    root->hdr.cls = metaroot;
    metaroot->hdr.cls = metaroot;
    root->major = metaroot->major = 1;
    if ((st = registry_add(root)) || (st = registry_add(metaroot))) return st;
    OsdoRootClass = root;
    OsdoClassClass = metaroot;
    return OSDO_OK;
}

void osdo_shutdown(void)
{
    for (size_t i = 0; i < nclasses; i++) class_free(registry[i]);
    free(registry);
    registry = NULL;
    nclasses = cap_classes = 0;
    OsdoRootClass = OsdoClassClass = NULL;
}

/* ------------------------------------------------------------ registry */

osdo_class *osdo_find_class(const char *name)
{
    for (size_t i = 0; i < nclasses; i++)
        if (strcmp(registry[i]->name, name) == 0) return registry[i];
    return NULL;
}

size_t osdo_class_count(void) { return nclasses; }
osdo_class *osdo_class_at(size_t i) { return i < nclasses ? registry[i] : NULL; }

osdo_status osdo_register_class(const osdo_class_def *def, osdo_class **out)
{
    if (!OsdoRootClass) return OSDO_EINVAL;
    if (!def || !def->name || !def->parent) return OSDO_EINVAL;
    osdo_class *parent = osdo_find_class(def->parent);
    if (!parent || parent->is_meta) return OSDO_ENOCLASS;
    /* Same major version; the installed minor must be at least the one the
     * class was built against. Checked first, so an incompatible plug-in
     * is reported as such even if a class of that name exists. */
    if (def->parent_major != parent->major || def->parent_minor > parent->minor)
        return OSDO_EVERSION;
    if (osdo_find_class(def->name)) return OSDO_EEXIST;

    size_t n = strlen(def->name);
    char *mname = malloc(n + 3);
    if (!mname) return OSDO_ENOMEM;
    mname[0] = 'M';
    mname[1] = '_';
    memcpy(mname + 2, def->name, n + 1);

    osdo_class *meta, *cls;
    osdo_status st = build(mname, parent->hdr.cls, OsdoClassClass, true, def->cdata_size,
                          def->cmethods, def->ncmethods, def->coverrides, def->ncoverrides, &meta);
    if (st) { free(mname); return st; }
    meta->owns_name = true;
    meta->major = def->major;
    meta->minor = def->minor;
    st = build(def->name, parent, meta, false, def->data_size,
               def->methods, def->nmethods, def->overrides, def->noverrides, &cls);
    if (st) { class_free(meta); return st; }
    cls->major = def->major;
    cls->minor = def->minor;
    cls->props = def->props;
    cls->nprops = def->nprops;
    cls->events = def->events;
    cls->nevents = def->nevents;
    cls->types = def->types;
    cls->ntypes = def->ntypes;
    if ((st = registry_add(meta)) || (st = registry_add(cls))) return st;
    if (out) *out = cls;
    return OSDO_OK;
}

bool osdo_subclass_of(const osdo_class *cls, const osdo_class *of)
{
    for (; cls; cls = cls->parent)
        if (cls == of) return true;
    return false;
}

#ifdef OSDO_CHECKED
#include <stdio.h>
#endif

void osdo_bad_call(const osdo_object *obj, const osdo_class *intro, uint32_t k)
{
#ifdef OSDO_CHECKED
    fprintf(stderr, "osdo: static call of %s method %u on %s %s\n", intro->name, (unsigned)k,
            obj ? "an object of class" : "a NULL object", obj ? obj->cls->name : "");
#endif
    (void)obj; (void)intro; (void)k;
    abort();
}

const osdo_property_def *osdo_find_property(const osdo_class *cls, const char *name, const osdo_class **declaring)
{
    for (; cls && name; cls = cls->parent)
        for (size_t i = 0; i < cls->nprops; i++)
            if (!strcmp(cls->props[i].name, name)) {
                if (declaring) *declaring = cls;
                return &cls->props[i];
            }
    return NULL;
}

bool osdo_has_event(const osdo_class *cls, const char *name)
{
    for (; cls && name; cls = cls->parent)
        for (size_t i = 0; i < cls->nevents; i++)
            if (!strcmp(cls->events[i], name)) return true;
    return false;
}

bool osdo_is_a(const osdo_object *obj, const osdo_class *cls)
{
    return obj && osdo_subclass_of(obj->cls, cls);
}

osdo_class *osdo_resolve_class(osdo_class *cls)
{
    while (cls && cls->replaced_by) cls = cls->replaced_by;
    return cls;
}

osdo_status osdo_replace_class(const char *original, const char *replacement)
{
    osdo_class *o = osdo_find_class(original), *r = osdo_find_class(replacement);
    if (!o || !r || o->is_meta || r->is_meta) return OSDO_ENOCLASS;
    if (o == r || !osdo_subclass_of(r, o)) return OSDO_ENOTSUBCLASS;
    o->replaced_by = r;
    return OSDO_OK;
}

/* ------------------------------------------------------------- objects */

osdo_object *osdo_instantiate(osdo_class *cls)
{
    if (!cls || cls->is_meta) return NULL;
    osdo_object *o = calloc(1, cls->instance_size);
    if (!o) return NULL;
    o->cls = cls;
    o->refs = 1;
    ((void (*)(osdo_object *))osdo_slot(o, OsdoRootClass, OsdoRoot_osdInitData))(o);
    return o;
}

osdo_object *osdo_new(osdo_class *cls)
{
    if (!cls || cls->is_meta) return NULL;
    osdo_fn fn = osdo_slot(&cls->hdr, OsdoClassClass, OsdoClass_osdclsNew);
    return ((osdo_object *(*)(osdo_object *))fn)(&cls->hdr);
}

osdo_object *osdo_ref(osdo_object *obj)
{
    if (obj) obj->refs++;
    return obj;
}

void osdo_unref(osdo_object *obj)
{
    if (!obj || --obj->refs) return;
    if (obj->cls->is_meta) return;   /* classes live until shutdown */
    ((void (*)(osdo_object *))osdo_slot(obj, OsdoRootClass, OsdoRoot_osdUnInitData))(obj);
    free(obj);
}

/* ------------------------------------------------------------ dispatch */

int osdo_method_slot(const osdo_class *cls, const char *method)
{
    return cls ? names_find(cls, method) : -1;
}

const osdo_method_def *osdo_method_info(const osdo_class *cls, const char *method)
{
    int s = osdo_method_slot(cls, method);
    return s < 0 ? NULL : cls->minfo[s];
}

osdo_status osdo_send(osdo_object *obj, const char *method,
                    const osdo_value *args, size_t nargs, osdo_value *ret)
{
    osdo_value dummy;
    if (!ret) ret = &dummy;
    *ret = osdo_v_void();
    if (!obj || !method) return OSDO_EINVAL;
    int s = names_find(obj->cls, method);
    if (s < 0) return OSDO_ENOMETHOD;
    const osdo_method_def *m = obj->cls->minfo[s];
    int n = sig_nargs(m->sig);
    if (n < 0 || (size_t)n != nargs) return OSDO_EARGS;
    for (int i = 0; i < n; i++) {
        char want = m->sig[2 + i];
        if (args[i].type != want) return OSDO_EARGS;
        if (want == 's' && !args[i].u.s) return OSDO_EARGS;
    }
    osdo_status st = m->apply(obj->cls->mtab[s], obj, args, ret);
    if (ret == &dummy) osdo_value_clear(&dummy);
    return st;
}

/* -------------------------------------------------------------- values */

osdo_value osdo_v_void(void) { osdo_value v = { .type = 'v' }; return v; }
osdo_value osdo_v_bool(bool b) { osdo_value v = { .type = 'b', .u.b = b }; return v; }
osdo_value osdo_v_int(int64_t i) { osdo_value v = { .type = 'i', .u.i = i }; return v; }
osdo_value osdo_v_double(double d) { osdo_value v = { .type = 'd', .u.d = d }; return v; }
osdo_value osdo_v_string(const char *s) { return osdo_v_string_take(s ? xstrdup(s) : NULL); }
osdo_value osdo_v_string_take(char *s) { osdo_value v = { .type = 's', .u.s = s }; return v; }
osdo_value osdo_v_object(osdo_object *o) { return osdo_v_object_take(osdo_ref(o)); }
osdo_value osdo_v_object_take(osdo_object *o) { osdo_value v = { .type = 'o', .u.o = o }; return v; }
osdo_value osdo_v_pending_take(osdo_pending *p) { osdo_value v = { .type = 'p', .u.p = p }; return v; }

void osdo_value_clear(osdo_value *v)
{
    if (!v) return;
    switch (v->type) {
    case 's': free(v->u.s); break;
    case 'o': osdo_unref(v->u.o); break;
    case 'p': osdo_pending_unref(v->u.p); break;
    default: break;
    }
    *v = osdo_v_void();
}

/* ------------------------------------------------------------- pending */

struct pending_cb { osdo_pending_cb cb; void *user; };

struct osdo_pending {
    uint32_t refs;
    bool done;
    osdo_value value;
    struct pending_cb *cbs;
    size_t ncbs, capcbs;
};

osdo_pending *osdo_pending_new(void)
{
    osdo_pending *p = calloc(1, sizeof *p);
    if (p) { p->refs = 1; p->value = osdo_v_void(); }
    return p;
}

osdo_pending *osdo_pending_ref(osdo_pending *p)
{
    if (p) p->refs++;
    return p;
}

void osdo_pending_unref(osdo_pending *p)
{
    if (!p || --p->refs) return;
    osdo_value_clear(&p->value);
    free(p->cbs);
    free(p);
}

void osdo_pending_complete(osdo_pending *p, osdo_value v)
{
    if (!p || p->done) { osdo_value_clear(&v); return; }
    p->value = v;
    p->done = true;
    osdo_pending_ref(p);   /* callbacks may drop the last outside reference */
    for (size_t i = 0; i < p->ncbs; i++) p->cbs[i].cb(p, &p->value, p->cbs[i].user);
    free(p->cbs);
    p->cbs = NULL;
    p->ncbs = p->capcbs = 0;
    osdo_pending_unref(p);
}

void osdo_pending_then(osdo_pending *p, osdo_pending_cb cb, void *user)
{
    if (!p || !cb) return;
    if (p->done) { cb(p, &p->value, user); return; }
    if (p->ncbs == p->capcbs) {
        size_t ncap = p->capcbs ? p->capcbs * 2 : 4;
        struct pending_cb *n = realloc(p->cbs, ncap * sizeof *n);
        if (!n) return;
        p->cbs = n;
        p->capcbs = ncap;
    }
    p->cbs[p->ncbs++] = (struct pending_cb){ cb, user };
}

bool osdo_pending_done(const osdo_pending *p) { return p && p->done; }
const osdo_value *osdo_pending_value(const osdo_pending *p) { return p && p->done ? &p->value : NULL; }
