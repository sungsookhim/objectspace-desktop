/*
 * libwpo core: classes, metaclasses, method tables, dispatch, replacement,
 * pending results. Uses only the C library.
 */
#include "wpo.h"

#include <stdlib.h>
#include <string.h>

struct wpo_name_ent {
    const char *name;
    uint32_t slot;
};

wpo_class *WPORootClass;
wpo_class *WPOClassClass;

static wpo_class **registry;
static size_t nclasses, cap_classes;

/* ------------------------------------------------------------- helpers */

const char *wpo_strerror(wpo_status st)
{
    switch (st) {
    case WPO_OK: return "ok";
    case WPO_ENOMEM: return "out of memory";
    case WPO_ENOCLASS: return "no such class";
    case WPO_EEXIST: return "already exists";
    case WPO_ENOMETHOD: return "no such method";
    case WPO_EARGS: return "wrong arguments";
    case WPO_EVERSION: return "incompatible class version";
    case WPO_ENOTSUBCLASS: return "not a subclass";
    case WPO_EPLUGIN: return "plug-in failed";
    case WPO_EINVAL: return "invalid argument";
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

static int names_find(const wpo_class *c, const char *name)
{
    if (!c->names_cap) return -1;
    uint32_t mask = c->names_cap - 1;
    for (uint32_t i = hash(name) & mask;; i = (i + 1) & mask) {
        if (!c->names[i].name) return -1;
        if (strcmp(c->names[i].name, name) == 0) return (int)c->names[i].slot;
    }
}

static void names_put(wpo_class *c, const char *name, uint32_t slot)
{
    uint32_t mask = c->names_cap - 1;
    uint32_t i = hash(name) & mask;
    while (c->names[i].name) i = (i + 1) & mask;
    c->names[i].name = name;
    c->names[i].slot = slot;
}

static wpo_status registry_add(wpo_class *c)
{
    if (nclasses == cap_classes) {
        size_t ncap = cap_classes ? cap_classes * 2 : 32;
        wpo_class **n = realloc(registry, ncap * sizeof *n);
        if (!n) return WPO_ENOMEM;
        registry = n;
        cap_classes = ncap;
    }
    registry[nclasses++] = c;
    return WPO_OK;
}

static void class_free(wpo_class *c)
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
static wpo_status build(const char *name, wpo_class *parent, wpo_class *meta, bool is_meta,
                        size_t data_size,
                        const wpo_method_def *methods, size_t nm,
                        const wpo_override_def *ov, size_t nov, wpo_class **out)
{
    size_t objsize = meta ? meta->instance_size : sizeof(wpo_class);
    if (objsize < sizeof(wpo_class)) objsize = sizeof(wpo_class);
    wpo_class *c = calloc(1, objsize);
    if (!c) return WPO_ENOMEM;
    c->hdr.cls = meta;
    c->hdr.refs = 1;
    c->name = name;
    c->parent = parent;
    c->is_meta = is_meta;
    c->depth = parent ? parent->depth + 1 : 0;

    /* Instance layout: header, then each class's data block in order. The
     * first metaclass (WPOClass) starts after the whole class struct,
     * because its instances are classes. */
    size_t base = parent ? parent->instance_size : sizeof(wpo_object);
    if (is_meta && (!parent || !parent->is_meta) && base < sizeof(wpo_class))
        base = sizeof(wpo_class);
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
    if (!c->mtab || !c->minfo || !c->names) { class_free(c); return WPO_ENOMEM; }

    if (parent) {
        memcpy(c->mtab, parent->mtab, inherited * sizeof *c->mtab);
        memcpy(c->minfo, parent->minfo, inherited * sizeof *c->minfo);
        for (uint32_t s = 0; s < inherited; s++) names_put(c, parent->minfo[s]->name, s);
    }
    for (size_t i = 0; i < nm; i++) {
        if (sig_nargs(methods[i].sig) < 0 || !methods[i].apply || !methods[i].impl) {
            class_free(c);
            return WPO_EINVAL;
        }
        if (names_find(c, methods[i].name) >= 0) {   /* must override, not redefine */
            class_free(c);
            return WPO_EEXIST;
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
            return WPO_ENOMETHOD;
        }
        c->mtab[s] = ov[i].impl;
    }
    *out = c;
    return WPO_OK;
}

/* ------------------------------------------------- built-in root methods */

static void root_init(wpo_object *self) { (void)self; }
static void root_uninit(wpo_object *self) { (void)self; }

static wpo_status apply_void0(wpo_fn fn, wpo_object *self, const wpo_value *a, wpo_value *ret)
{
    (void)a;
    ((void (*)(wpo_object *))fn)(self);
    *ret = wpo_v_void();
    return WPO_OK;
}

static wpo_object *class_new(wpo_object *self)
{
    return wpo_instantiate(wpo_resolve_class((wpo_class *)self));
}

static wpo_status apply_obj0(wpo_fn fn, wpo_object *self, const wpo_value *a, wpo_value *ret)
{
    (void)a;
    *ret = wpo_v_object_take(((wpo_object *(*)(wpo_object *))fn)(self));
    return WPO_OK;
}

static const wpo_method_def root_methods[] = {
    { "wpInitData", "v()", 0, apply_void0, (wpo_fn)root_init },
    { "wpUnInitData", "v()", 0, apply_void0, (wpo_fn)root_uninit },
};
static const wpo_method_def class_methods[] = {
    { "wpclsNew", "o()", 0, apply_obj0, (wpo_fn)class_new },
};

wpo_status wpo_init(void)
{
    if (WPORootClass) return WPO_OK;
    wpo_class *root, *metaroot;
    wpo_status st = build("WPORoot", NULL, NULL, false, 0, root_methods, 2, NULL, 0, &root);
    if (st) return st;
    st = build("WPOClass", root, NULL, true, 0, class_methods, 1, NULL, 0, &metaroot);
    if (st) { class_free(root); return st; }
    root->hdr.cls = metaroot;
    metaroot->hdr.cls = metaroot;
    root->major = metaroot->major = 1;
    if ((st = registry_add(root)) || (st = registry_add(metaroot))) return st;
    WPORootClass = root;
    WPOClassClass = metaroot;
    return WPO_OK;
}

void wpo_shutdown(void)
{
    for (size_t i = 0; i < nclasses; i++) class_free(registry[i]);
    free(registry);
    registry = NULL;
    nclasses = cap_classes = 0;
    WPORootClass = WPOClassClass = NULL;
}

/* ------------------------------------------------------------ registry */

wpo_class *wpo_find_class(const char *name)
{
    for (size_t i = 0; i < nclasses; i++)
        if (strcmp(registry[i]->name, name) == 0) return registry[i];
    return NULL;
}

size_t wpo_class_count(void) { return nclasses; }
wpo_class *wpo_class_at(size_t i) { return i < nclasses ? registry[i] : NULL; }

wpo_status wpo_register_class(const wpo_class_def *def, wpo_class **out)
{
    if (!WPORootClass) return WPO_EINVAL;
    if (!def || !def->name || !def->parent) return WPO_EINVAL;
    wpo_class *parent = wpo_find_class(def->parent);
    if (!parent || parent->is_meta) return WPO_ENOCLASS;
    /* Same major version; the installed minor must be at least the one the
     * class was built against. Checked first, so an incompatible plug-in
     * is reported as such even if a class of that name exists. */
    if (def->parent_major != parent->major || def->parent_minor > parent->minor)
        return WPO_EVERSION;
    if (wpo_find_class(def->name)) return WPO_EEXIST;

    size_t n = strlen(def->name);
    char *mname = malloc(n + 3);
    if (!mname) return WPO_ENOMEM;
    mname[0] = 'M';
    mname[1] = '_';
    memcpy(mname + 2, def->name, n + 1);

    wpo_class *meta, *cls;
    wpo_status st = build(mname, parent->hdr.cls, WPOClassClass, true, def->cdata_size,
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
    if ((st = registry_add(meta)) || (st = registry_add(cls))) return st;
    if (out) *out = cls;
    return WPO_OK;
}

bool wpo_subclass_of(const wpo_class *cls, const wpo_class *of)
{
    for (; cls; cls = cls->parent)
        if (cls == of) return true;
    return false;
}

#ifdef WPO_CHECKED
#include <stdio.h>
#endif

void wpo_bad_call(const wpo_object *obj, const wpo_class *intro, uint32_t k)
{
#ifdef WPO_CHECKED
    fprintf(stderr, "wpo: static call of %s method %u on %s %s\n", intro->name, (unsigned)k,
            obj ? "an object of class" : "a NULL object", obj ? obj->cls->name : "");
#endif
    (void)obj; (void)intro; (void)k;
    abort();
}

const wpo_property_def *wpo_find_property(const wpo_class *cls, const char *name, const wpo_class **declaring)
{
    for (; cls && name; cls = cls->parent)
        for (size_t i = 0; i < cls->nprops; i++)
            if (!strcmp(cls->props[i].name, name)) {
                if (declaring) *declaring = cls;
                return &cls->props[i];
            }
    return NULL;
}

bool wpo_has_event(const wpo_class *cls, const char *name)
{
    for (; cls && name; cls = cls->parent)
        for (size_t i = 0; i < cls->nevents; i++)
            if (!strcmp(cls->events[i], name)) return true;
    return false;
}

bool wpo_is_a(const wpo_object *obj, const wpo_class *cls)
{
    return obj && wpo_subclass_of(obj->cls, cls);
}

wpo_class *wpo_resolve_class(wpo_class *cls)
{
    while (cls && cls->replaced_by) cls = cls->replaced_by;
    return cls;
}

wpo_status wpo_replace_class(const char *original, const char *replacement)
{
    wpo_class *o = wpo_find_class(original), *r = wpo_find_class(replacement);
    if (!o || !r || o->is_meta || r->is_meta) return WPO_ENOCLASS;
    if (o == r || !wpo_subclass_of(r, o)) return WPO_ENOTSUBCLASS;
    o->replaced_by = r;
    return WPO_OK;
}

/* ------------------------------------------------------------- objects */

wpo_object *wpo_instantiate(wpo_class *cls)
{
    if (!cls || cls->is_meta) return NULL;
    wpo_object *o = calloc(1, cls->instance_size);
    if (!o) return NULL;
    o->cls = cls;
    o->refs = 1;
    ((void (*)(wpo_object *))wpo_slot(o, WPORootClass, WPORoot_wpInitData))(o);
    return o;
}

wpo_object *wpo_new(wpo_class *cls)
{
    if (!cls || cls->is_meta) return NULL;
    wpo_fn fn = wpo_slot(&cls->hdr, WPOClassClass, WPOClass_wpclsNew);
    return ((wpo_object *(*)(wpo_object *))fn)(&cls->hdr);
}

wpo_object *wpo_ref(wpo_object *obj)
{
    if (obj) obj->refs++;
    return obj;
}

void wpo_unref(wpo_object *obj)
{
    if (!obj || --obj->refs) return;
    if (obj->cls->is_meta) return;   /* classes live until shutdown */
    ((void (*)(wpo_object *))wpo_slot(obj, WPORootClass, WPORoot_wpUnInitData))(obj);
    free(obj);
}

/* ------------------------------------------------------------ dispatch */

int wpo_method_slot(const wpo_class *cls, const char *method)
{
    return cls ? names_find(cls, method) : -1;
}

const wpo_method_def *wpo_method_info(const wpo_class *cls, const char *method)
{
    int s = wpo_method_slot(cls, method);
    return s < 0 ? NULL : cls->minfo[s];
}

wpo_status wpo_send(wpo_object *obj, const char *method,
                    const wpo_value *args, size_t nargs, wpo_value *ret)
{
    wpo_value dummy;
    if (!ret) ret = &dummy;
    *ret = wpo_v_void();
    if (!obj || !method) return WPO_EINVAL;
    int s = names_find(obj->cls, method);
    if (s < 0) return WPO_ENOMETHOD;
    const wpo_method_def *m = obj->cls->minfo[s];
    int n = sig_nargs(m->sig);
    if (n < 0 || (size_t)n != nargs) return WPO_EARGS;
    for (int i = 0; i < n; i++) {
        char want = m->sig[2 + i];
        if (args[i].type != want) return WPO_EARGS;
        if (want == 's' && !args[i].u.s) return WPO_EARGS;
    }
    wpo_status st = m->apply(obj->cls->mtab[s], obj, args, ret);
    if (ret == &dummy) wpo_value_clear(&dummy);
    return st;
}

/* -------------------------------------------------------------- values */

wpo_value wpo_v_void(void) { wpo_value v = { .type = 'v' }; return v; }
wpo_value wpo_v_bool(bool b) { wpo_value v = { .type = 'b', .u.b = b }; return v; }
wpo_value wpo_v_int(int64_t i) { wpo_value v = { .type = 'i', .u.i = i }; return v; }
wpo_value wpo_v_double(double d) { wpo_value v = { .type = 'd', .u.d = d }; return v; }
wpo_value wpo_v_string(const char *s) { return wpo_v_string_take(s ? xstrdup(s) : NULL); }
wpo_value wpo_v_string_take(char *s) { wpo_value v = { .type = 's', .u.s = s }; return v; }
wpo_value wpo_v_object(wpo_object *o) { return wpo_v_object_take(wpo_ref(o)); }
wpo_value wpo_v_object_take(wpo_object *o) { wpo_value v = { .type = 'o', .u.o = o }; return v; }
wpo_value wpo_v_pending_take(wpo_pending *p) { wpo_value v = { .type = 'p', .u.p = p }; return v; }

void wpo_value_clear(wpo_value *v)
{
    if (!v) return;
    switch (v->type) {
    case 's': free(v->u.s); break;
    case 'o': wpo_unref(v->u.o); break;
    case 'p': wpo_pending_unref(v->u.p); break;
    default: break;
    }
    *v = wpo_v_void();
}

/* ------------------------------------------------------------- pending */

struct pending_cb { wpo_pending_cb cb; void *user; };

struct wpo_pending {
    uint32_t refs;
    bool done;
    wpo_value value;
    struct pending_cb *cbs;
    size_t ncbs, capcbs;
};

wpo_pending *wpo_pending_new(void)
{
    wpo_pending *p = calloc(1, sizeof *p);
    if (p) { p->refs = 1; p->value = wpo_v_void(); }
    return p;
}

wpo_pending *wpo_pending_ref(wpo_pending *p)
{
    if (p) p->refs++;
    return p;
}

void wpo_pending_unref(wpo_pending *p)
{
    if (!p || --p->refs) return;
    wpo_value_clear(&p->value);
    free(p->cbs);
    free(p);
}

void wpo_pending_complete(wpo_pending *p, wpo_value v)
{
    if (!p || p->done) { wpo_value_clear(&v); return; }
    p->value = v;
    p->done = true;
    wpo_pending_ref(p);   /* callbacks may drop the last outside reference */
    for (size_t i = 0; i < p->ncbs; i++) p->cbs[i].cb(p, &p->value, p->cbs[i].user);
    free(p->cbs);
    p->cbs = NULL;
    p->ncbs = p->capcbs = 0;
    wpo_pending_unref(p);
}

void wpo_pending_then(wpo_pending *p, wpo_pending_cb cb, void *user)
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

bool wpo_pending_done(const wpo_pending *p) { return p && p->done; }
const wpo_value *wpo_pending_value(const wpo_pending *p) { return p && p->done ? &p->value : NULL; }
