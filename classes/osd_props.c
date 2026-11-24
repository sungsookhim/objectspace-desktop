/* Declared properties (object-properties-design.md 2.1): their values, kept
 * in the object's state or reached through the class's methods, and the
 * settings pages made from them. OsdObject's osdQueryProperty, osdSetProperty,
 * osdListProperties and osdAddSettingsPages call these. */
#include "osd_props.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "OsdObject.h"
#include "osd_classes.h"
#include "osd_state.h"

/* ---------------------------------------------------------------- choices */

int osd_option_index(const char *options, const char *v)
{
    int i = 0;
    for (const char *p = options; p && *p && v; i++) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (strlen(v) == l && !strncasecmp(p, v, l)) return i;   /* ("classic" is "Classic") */
        p = e ? e + 1 : NULL;
    }
    return -1;
}

/* A choice's options now: its class's method's, else the declared ones. */
char *osd_prop_choices(osdo_object *self, const osdo_property_def *p)
{
    if (p->backing && !strcmp(p->type, "choice") && self && osd_backing_options) {
        char *o = osd_backing_options(self, p);
        if (o) return o;
    }
    if (p->options && self) {
        osdo_value r = osdo_v_void();
        if (osdo_send(self, p->options, NULL, 0, &r) == OSDO_OK && r.type == 's' && r.u.s) {
            char *o = osd_strdup(r.u.s);
            osdo_value_clear(&r);
            return o;
        }
        osdo_value_clear(&r);
    }
    return p->choices ? osd_strdup(p->choices) : NULL;
}

char *osd_option_at(const char *options, long k)
{
    long i = 0;
    for (const char *p = options; p && *p; i++) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (i == k) {
            char *r = malloc(l + 1);
            if (r) { memcpy(r, p, l); r[l] = 0; }
            return r;
        }
        p = e ? e + 1 : NULL;
    }
    return NULL;
}

static bool is_int_type(const osdo_property_def *p)
{
    return !strcmp(p->type, "number") || !strcmp(p->type, "checkbox") ||
           (!strcmp(p->type, "choice") && (p->flags & OSDO_P_INDEX));
}

static const char *store_of(const osdo_property_def *p, const osdo_class *decl)
{
    return p->store ? p->store : decl->name;
}

static char *itoa_dup(int64_t v)
{
    char b[32];
    snprintf(b, sizeof b, "%lld", (long long)v);
    return osd_strdup(b);
}

/* A value given by a method, as text (a choice's number as the option). */
static char *call_get(osdo_object *self, const osdo_property_def *p)
{
    osdo_value r = osdo_v_void();
    if (osdo_send(self, p->get, NULL, 0, &r) != OSDO_OK) return NULL;
    char *out = NULL;
    switch (r.type) {
    case 's': out = r.u.s ? osd_strdup(r.u.s) : osd_strdup(""); break;
    case 'b': out = osd_strdup(r.u.b ? "1" : "0"); break;
    case 'i':
        if (!strcmp(p->type, "choice")) {
            char *ch = osd_prop_choices(self, p);
            out = osd_option_at(ch, (long)r.u.i);
            free(ch);
        } else out = itoa_dup(r.u.i);
        if (!out) out = osd_strdup("");
        break;
    case 'd': { char b[64]; snprintf(b, sizeof b, "%g", r.u.d); out = osd_strdup(b); break; }
    default: break;
    }
    osdo_value_clear(&r);
    return out;
}

/* ------------------------------------------------------------------ values */

char *(*osd_prop_inherit)(osdo_object *self, const osdo_property_def *p, const char *store, char *origin, size_t on);
char *(*osd_scope_get)(const char *scope, const char *store, const char *prop);
bool (*osd_scope_set)(const char *scope, const char *store, const char *prop, const char *value);
char *(*osd_scheme_titles)(void);
char *(*osd_backing_get)(osdo_object *self, const osdo_property_def *p);
bool (*osd_backing_set)(osdo_object *self, const osdo_property_def *p, const char *value);
char *(*osd_backing_options)(osdo_object *self, const osdo_property_def *p);

/* Its own value, if the object has one (kept in its state). */
static char *own_value(osdo_object *self, const osdo_property_def *p, const char *st)
{
    if (is_int_type(p) && !(p->flags & OSDO_P_TEXT)) {
        int64_t v;
        if (osd_state_get_int(self, st, p->name, &v)) {
            if (!strcmp(p->type, "choice")) {
                char *ch = osd_prop_choices(self, p);
                char *o = osd_option_at(ch, (long)v);
                free(ch);
                return o ? o : osd_strdup("");
            }
            if (!strcmp(p->type, "checkbox")) return osd_strdup(v ? "1" : "0");
            return itoa_dup(v);
        }
    } else {
        char *v = NULL;
        if (osd_state_get_str(self, st, p->name, &v) && v) return v;
        free(v);
    }
    return NULL;
}

char *osd_prop_get_origin(osdo_object *self, const char *name, char *origin, size_t on)
{
    const osdo_class *decl;
    const osdo_property_def *p = osdo_find_property(self->cls, name, &decl);
    if (!p) return NULL;
    if (origin && on) snprintf(origin, on, "object");
    if (p->backing) {   /* where Linux keeps it (NULL: not for this object) */
        if (origin && on) snprintf(origin, on, "backing");
        return osd_backing_get ? osd_backing_get(self, p) : NULL;
    }
    if (p->get) {   /* the object's own, always (its title, its command) */
        char *v = call_get(self, p);
        if (v) return v;
    }
    const char *st = store_of(p, decl);
    char *v = own_value(self, p, st);
    if (v) return v;
    /* the chain (object-properties-design.md 2.2): its type, its class and
     * the classes above it, ... (the service's) */
    if (osd_prop_inherit && (v = osd_prop_inherit(self, p, st, origin, on))) return v;
    if (origin && on) snprintf(origin, on, "default");
    return osd_strdup(p->def ? p->def : "");
}

char *osd_prop_get(osdo_object *self, const char *name)
{
    return osd_prop_get_origin(self, name, NULL, 0);
}

bool osd_prop_reset(osdo_object *self, const char *name)
{
    const osdo_class *decl;
    const osdo_property_def *p = osdo_find_property(self->cls, name, &decl);
    if (!p || p->get || p->backing || (p->flags & OSDO_P_READONLY)) return false;
    osd_state_remove(self, store_of(p, decl), p->name);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

const char *osd_prop_store(const osdo_class *cls, const char *name)
{
    const osdo_class *decl;
    const osdo_property_def *p = osdo_find_property(cls, name, &decl);
    return p ? store_of(p, decl) : NULL;
}

/* The text a value is given as, made canonical for its type; NULL if it is
 * not one (a choice: the option's text or number; a checkbox: 1/0,
 * true/false, yes/no, on/off). */
static char *canonical_in(const osdo_property_def *p, const char *choices, const char *v);

char *osd_prop_canonical(const osdo_property_def *p, const char *v) { return canonical_in(p, p->choices, v); }

/* An event's actions (object-properties-design.md 2.4): "" (none of its
 * own), or kinds separated by "|": none, sound:FILE, run:COMMAND,
 * call:METHOD ARGS, set:SETUP. */
static bool actions_ok(const char *v)
{
    for (const char *p = v; p && *p;) {
        while (*p == ' ') p++;
        const char *e = strchr(p, '|');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        while (l && p[l - 1] == ' ') l--;
        bool ok = (l == 4 && !strncmp(p, "none", 4)) || (l > 6 && !strncmp(p, "sound:", 6)) ||
                  (l > 4 && !strncmp(p, "run:", 4)) || (l > 5 && !strncmp(p, "call:", 5)) || (l > 4 && !strncmp(p, "set:", 4));
        if (!ok) return false;
        if (!strncmp(p, "set:", 4)) {   /* key=value, each of them (";" between) */
            for (const char *q = p + 4; q < p + l;) {
                const char *e2 = memchr(q, ';', (size_t)(p + l - q));
                const char *end = e2 ? e2 : p + l, *eq = memchr(q, '=', (size_t)(end - q));
                while (q < end && *q == ' ') q++;
                if (q < end && (!eq || eq == q)) return false;
                q = e2 ? e2 + 1 : end;
            }
        }
        p = e ? e + 1 : NULL;
    }
    return true;
}

static char *canonical_in(const osdo_property_def *p, const char *choices, const char *v)
{
    if (!v || strchr(v, '\n') || strchr(v, '\t')) return NULL;
    if (!strncmp(p->name, "on:", 3) && !actions_ok(v)) return NULL;
    if (!strcmp(p->type, "checkbox")) {
        if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on")) return osd_strdup("1");
        if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "off")) return osd_strdup("0");
        return NULL;
    }
    if (!strcmp(p->type, "choice")) {
        if (osd_option_index(choices, v) >= 0) return osd_strdup(v);   /* (as given: "dark" is "Dark") */
        char *end;
        long k = strtol(v, &end, 10);
        return *v && !*end ? osd_option_at(choices, k) : NULL;
    }
    if (!strcmp(p->type, "number")) {
        char *end;
        strtoll(v, &end, 10);
        return *v && !*end ? osd_strdup(v) : NULL;
    }
    if (!strcmp(p->type, "color")) {
        if (!*v) return osd_strdup("");
        if (v[0] != '#' || strlen(v) != 7) return NULL;
        for (int i = 1; i < 7; i++)
            if (!strchr("0123456789abcdefABCDEF", v[i])) return NULL;
        return osd_strdup(v);
    }
    return osd_strdup(v);
}

bool osd_prop_set(osdo_object *self, const char *name, const char *value)
{
    const osdo_class *decl;
    const osdo_property_def *p = osdo_find_property(self->cls, name, &decl);
    if (!p || (p->flags & OSDO_P_READONLY) || !strcmp(p->type, "info")) return false;
    char *ch = osd_prop_choices(self, p);
    char *v = canonical_in(p, ch, value);
    if (!v) { free(ch); return false; }
    bool ok;
    if (p->backing) {
        ok = osd_backing_set && osd_backing_set(self, p, v);
        if (ok) OsdObject_osdObjectChanged(self);
    } else if (p->set) {
        /* the class's setter, given the value as its argument's type */
        const osdo_method_def *m = osdo_method_info(self->cls, p->set);
        if (!m || strlen(m->sig) < 4) { free(v); free(ch); return false; }
        osdo_value a, r = osdo_v_void();
        char t = m->sig[2];
        if (t == 'b') a = osdo_v_bool(!strcmp(v, "1"));
        else if (t == 'i') a = osdo_v_int(!strcmp(p->type, "choice") ? osd_option_index(ch, v) : strtoll(v, NULL, 10));
        else a = osdo_v_string(v);
        ok = osdo_send(self, p->set, &a, 1, &r) == OSDO_OK && (r.type != 'b' || r.u.b);
        osdo_value_clear(&a);
        osdo_value_clear(&r);
    } else {
        const char *st = store_of(p, decl);
        if (is_int_type(p) && !(p->flags & OSDO_P_TEXT))
            osd_state_put_int(self, st, p->name,
                              !strcmp(p->type, "choice") ? osd_option_index(ch, v) : strtoll(v, NULL, 10));
        else
            osd_state_put_str(self, st, p->name, v);
        OsdObject_osdSaveDeferred(self);
        OsdObject_osdObjectChanged(self);
        ok = true;
    }
    free(v);
    free(ch);
    return ok;
}

/* ------------------------------------------------------------------ lists */

static void chain(const osdo_class *cls, const osdo_class **out, size_t *n)
{
    const osdo_class *c[64];
    size_t k = 0;
    for (; cls && k < 64; cls = cls->parent) c[k++] = cls;
    *n = 0;
    while (k) out[(*n)++] = c[--k];   /* the root first */
}

static char *append(char *s, const char *t)
{
    char *r = osd_concat(s, t);
    free(s);
    return r;
}

char *osd_prop_list(osdo_object *self)
{
    const osdo_class *c[64];
    size_t n;
    chain(self->cls, c, &n);
    char *out = osd_strdup("");
    for (size_t k = 0; k < n; k++)
        for (size_t i = 0; i < c[k]->nprops; i++) {
            const osdo_property_def *p = &c[k]->props[i];
            if (osdo_find_property(self->cls, p->name, NULL) != p) continue;   /* declared again below */
            char *v = osd_prop_get(self, p->name);
            if (!v && p->backing) continue;   /* (not for this object) */
            size_t l = strlen(p->name) + strlen(v ? v : "") + 3;
            char *line = malloc(l);
            snprintf(line, l, "%s\t%s\n", p->name, v ? v : "");
            out = append(out, line);
            free(line);
            free(v);
        }
    return out;
}

static bool shown(osdo_object *self, const osdo_property_def *p)
{
    if (p->flags & OSDO_P_NOFIELD) return false;
    if (!p->show) return true;
    osdo_value r = osdo_v_void();
    bool ok = osdo_send(self, p->show, NULL, 0, &r) == OSDO_OK && r.type == 'b' && r.u.b;
    osdo_value_clear(&r);
    return ok;
}

/* Whether an ancestor of `c` declares p's name (p redeclares it). */
static bool redeclared(const osdo_class *c, const osdo_property_def *p)
{
    return c->parent && osdo_find_property(c->parent, p->name, NULL) != NULL;
}

static char *field_lines(osdo_object *self, const osdo_property_def *p, const char **page)
{
    if (!shown(self, p)) return osd_strdup("");
    char *v = osd_prop_get(self, p->name);
    if (!v && p->backing) return osd_strdup("");   /* not kept for this object (a program with no desktop entry) */
    char *ch = osd_prop_choices(self, p);
    char shownv[32];
    const char *fv = v ? v : "";
    if (!strcmp(p->type, "choice")) { snprintf(shownv, sizeof shownv, "%d", osd_option_index(ch, fv)); fv = shownv; }
    else if (!strcmp(p->type, "checkbox")) fv = !strcmp(fv, "1") ? "1" : "0";
    const char *pg = p->page ? p->page : "General";
    size_t l = strlen(pg) + strlen(p->name) + strlen(p->label ? p->label : "") + strlen(fv) +
               strlen(ch ? ch : "") + 64;
    char *line = malloc(l);
    size_t o = 0;
    if (!*page || strcmp(*page, pg)) o += (size_t)snprintf(line, l, "page\t%s\n", pg);
    *page = pg;
    snprintf(line + o, l - o, "field\t%s\t%s\t%s\t%s\t@prop%s%s\n", p->name, p->type, p->label ? p->label : p->name, fv,
             ch ? "\t" : "", ch ? ch : "");
    free(v);
    free(ch);
    return line;
}

char *osd_prop_pages(osdo_object *self, const osdo_class *only)
{
    const osdo_class *c[64];
    size_t n;
    chain(self->cls, c, &n);
    char *out = osd_strdup("");
    const char *page = NULL;
    /* pages marked first, then the others, then those marked last; each
     * property where it was first declared, as its nearest declaration (a
     * subclass's label) */
    for (int pass = 0; pass < 3; pass++)
        for (size_t k = 0; k < n; k++) {
            if (only && c[k] != only) continue;
            for (size_t i = 0; i < c[k]->nprops; i++) {
                const osdo_property_def *d = &c[k]->props[i];
                if (redeclared(c[k], d)) continue;   /* shown where its ancestor put it */
                const osdo_property_def *p = osdo_find_property(self->cls, d->name, NULL);
                int want = (p && (p->flags & OSDO_P_FIRST)) ? 0 : (p && (p->flags & OSDO_P_LAST)) ? 2 : 1;
                if (!p || want != pass) continue;
                char *line = field_lines(self, p, &page);
                out = append(out, line);
                free(line);
            }
        }
    return out;
}
