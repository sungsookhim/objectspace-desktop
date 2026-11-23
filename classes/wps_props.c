/* Declared properties (object-properties-design.md 2.1): their values, kept
 * in the object's state or reached through the class's methods, and the
 * settings pages made from them. WPObject's wpQueryProperty, wpSetProperty,
 * wpListProperties and wpAddSettingsPages call these. */
#include "wps_props.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "WPObject.h"
#include "wps_classes.h"
#include "wps_state.h"

/* ---------------------------------------------------------------- choices */

int wps_option_index(const char *options, const char *v)
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
char *wps_prop_choices(wpo_object *self, const wpo_property_def *p)
{
    if (p->options && self) {
        wpo_value r = wpo_v_void();
        if (wpo_send(self, p->options, NULL, 0, &r) == WPO_OK && r.type == 's' && r.u.s) {
            char *o = wps_strdup(r.u.s);
            wpo_value_clear(&r);
            return o;
        }
        wpo_value_clear(&r);
    }
    return p->choices ? wps_strdup(p->choices) : NULL;
}

char *wps_option_at(const char *options, long k)
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

static bool is_int_type(const wpo_property_def *p)
{
    return !strcmp(p->type, "number") || !strcmp(p->type, "checkbox") ||
           (!strcmp(p->type, "choice") && (p->flags & WPO_P_INDEX));
}

static const char *store_of(const wpo_property_def *p, const wpo_class *decl)
{
    return p->store ? p->store : decl->name;
}

static char *itoa_dup(int64_t v)
{
    char b[32];
    snprintf(b, sizeof b, "%lld", (long long)v);
    return wps_strdup(b);
}

/* A value given by a method, as text (a choice's number as the option). */
static char *call_get(wpo_object *self, const wpo_property_def *p)
{
    wpo_value r = wpo_v_void();
    if (wpo_send(self, p->get, NULL, 0, &r) != WPO_OK) return NULL;
    char *out = NULL;
    switch (r.type) {
    case 's': out = r.u.s ? wps_strdup(r.u.s) : wps_strdup(""); break;
    case 'b': out = wps_strdup(r.u.b ? "1" : "0"); break;
    case 'i':
        if (!strcmp(p->type, "choice")) {
            char *ch = wps_prop_choices(self, p);
            out = wps_option_at(ch, (long)r.u.i);
            free(ch);
        } else out = itoa_dup(r.u.i);
        if (!out) out = wps_strdup("");
        break;
    case 'd': { char b[64]; snprintf(b, sizeof b, "%g", r.u.d); out = wps_strdup(b); break; }
    default: break;
    }
    wpo_value_clear(&r);
    return out;
}

/* ------------------------------------------------------------------ values */

char *(*wps_prop_inherit)(wpo_object *self, const wpo_property_def *p, const char *store, char *origin, size_t on);
char *(*wps_scope_get)(const char *scope, const char *store, const char *prop);
bool (*wps_scope_set)(const char *scope, const char *store, const char *prop, const char *value);
char *(*wps_scheme_titles)(void);

/* Its own value, if the object has one (kept in its state). */
static char *own_value(wpo_object *self, const wpo_property_def *p, const char *st)
{
    if (is_int_type(p) && !(p->flags & WPO_P_TEXT)) {
        int64_t v;
        if (wps_state_get_int(self, st, p->name, &v)) {
            if (!strcmp(p->type, "choice")) {
                char *ch = wps_prop_choices(self, p);
                char *o = wps_option_at(ch, (long)v);
                free(ch);
                return o ? o : wps_strdup("");
            }
            if (!strcmp(p->type, "checkbox")) return wps_strdup(v ? "1" : "0");
            return itoa_dup(v);
        }
    } else {
        char *v = NULL;
        if (wps_state_get_str(self, st, p->name, &v) && v) return v;
        free(v);
    }
    return NULL;
}

char *wps_prop_get_origin(wpo_object *self, const char *name, char *origin, size_t on)
{
    const wpo_class *decl;
    const wpo_property_def *p = wpo_find_property(self->cls, name, &decl);
    if (!p) return NULL;
    if (origin && on) snprintf(origin, on, "object");
    if (p->get) {   /* the object's own, always (its title, its command) */
        char *v = call_get(self, p);
        if (v) return v;
    }
    const char *st = store_of(p, decl);
    char *v = own_value(self, p, st);
    if (v) return v;
    /* the chain (object-properties-design.md 2.2): its type, its class and
     * the classes above it, ... (the service's) */
    if (wps_prop_inherit && (v = wps_prop_inherit(self, p, st, origin, on))) return v;
    if (origin && on) snprintf(origin, on, "default");
    return wps_strdup(p->def ? p->def : "");
}

char *wps_prop_get(wpo_object *self, const char *name)
{
    return wps_prop_get_origin(self, name, NULL, 0);
}

bool wps_prop_reset(wpo_object *self, const char *name)
{
    const wpo_class *decl;
    const wpo_property_def *p = wpo_find_property(self->cls, name, &decl);
    if (!p || p->get || (p->flags & WPO_P_READONLY)) return false;
    wps_state_remove(self, store_of(p, decl), p->name);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

const char *wps_prop_store(const wpo_class *cls, const char *name)
{
    const wpo_class *decl;
    const wpo_property_def *p = wpo_find_property(cls, name, &decl);
    return p ? store_of(p, decl) : NULL;
}

/* The text a value is given as, made canonical for its type; NULL if it is
 * not one (a choice: the option's text or number; a checkbox: 1/0,
 * true/false, yes/no, on/off). */
static char *canonical_in(const wpo_property_def *p, const char *choices, const char *v);

char *wps_prop_canonical(const wpo_property_def *p, const char *v) { return canonical_in(p, p->choices, v); }

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
        p = e ? e + 1 : NULL;
    }
    return true;
}

static char *canonical_in(const wpo_property_def *p, const char *choices, const char *v)
{
    if (!v || strchr(v, '\n') || strchr(v, '\t')) return NULL;
    if (!strncmp(p->name, "on:", 3) && !actions_ok(v)) return NULL;
    if (!strcmp(p->type, "checkbox")) {
        if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on")) return wps_strdup("1");
        if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "off")) return wps_strdup("0");
        return NULL;
    }
    if (!strcmp(p->type, "choice")) {
        if (wps_option_index(choices, v) >= 0) return wps_strdup(v);   /* (as given: "dark" is "Dark") */
        char *end;
        long k = strtol(v, &end, 10);
        return *v && !*end ? wps_option_at(choices, k) : NULL;
    }
    if (!strcmp(p->type, "number")) {
        char *end;
        strtoll(v, &end, 10);
        return *v && !*end ? wps_strdup(v) : NULL;
    }
    if (!strcmp(p->type, "color")) {
        if (!*v) return wps_strdup("");
        if (v[0] != '#' || strlen(v) != 7) return NULL;
        for (int i = 1; i < 7; i++)
            if (!strchr("0123456789abcdefABCDEF", v[i])) return NULL;
        return wps_strdup(v);
    }
    return wps_strdup(v);
}

bool wps_prop_set(wpo_object *self, const char *name, const char *value)
{
    const wpo_class *decl;
    const wpo_property_def *p = wpo_find_property(self->cls, name, &decl);
    if (!p || (p->flags & WPO_P_READONLY) || !strcmp(p->type, "info")) return false;
    char *ch = wps_prop_choices(self, p);
    char *v = canonical_in(p, ch, value);
    if (!v) { free(ch); return false; }
    bool ok;
    if (p->set) {
        /* the class's setter, given the value as its argument's type */
        const wpo_method_def *m = wpo_method_info(self->cls, p->set);
        if (!m || strlen(m->sig) < 4) { free(v); free(ch); return false; }
        wpo_value a, r = wpo_v_void();
        char t = m->sig[2];
        if (t == 'b') a = wpo_v_bool(!strcmp(v, "1"));
        else if (t == 'i') a = wpo_v_int(!strcmp(p->type, "choice") ? wps_option_index(ch, v) : strtoll(v, NULL, 10));
        else a = wpo_v_string(v);
        ok = wpo_send(self, p->set, &a, 1, &r) == WPO_OK && (r.type != 'b' || r.u.b);
        wpo_value_clear(&a);
        wpo_value_clear(&r);
    } else {
        const char *st = store_of(p, decl);
        if (is_int_type(p) && !(p->flags & WPO_P_TEXT))
            wps_state_put_int(self, st, p->name,
                              !strcmp(p->type, "choice") ? wps_option_index(ch, v) : strtoll(v, NULL, 10));
        else
            wps_state_put_str(self, st, p->name, v);
        WPObject_wpSaveDeferred(self);
        WPObject_wpObjectChanged(self);
        ok = true;
    }
    free(v);
    free(ch);
    return ok;
}

/* ------------------------------------------------------------------ lists */

static void chain(const wpo_class *cls, const wpo_class **out, size_t *n)
{
    const wpo_class *c[64];
    size_t k = 0;
    for (; cls && k < 64; cls = cls->parent) c[k++] = cls;
    *n = 0;
    while (k) out[(*n)++] = c[--k];   /* the root first */
}

static char *append(char *s, const char *t)
{
    char *r = wps_concat(s, t);
    free(s);
    return r;
}

char *wps_prop_list(wpo_object *self)
{
    const wpo_class *c[64];
    size_t n;
    chain(self->cls, c, &n);
    char *out = wps_strdup("");
    for (size_t k = 0; k < n; k++)
        for (size_t i = 0; i < c[k]->nprops; i++) {
            const wpo_property_def *p = &c[k]->props[i];
            if (wpo_find_property(self->cls, p->name, NULL) != p) continue;   /* declared again below */
            char *v = wps_prop_get(self, p->name);
            size_t l = strlen(p->name) + strlen(v ? v : "") + 3;
            char *line = malloc(l);
            snprintf(line, l, "%s\t%s\n", p->name, v ? v : "");
            out = append(out, line);
            free(line);
            free(v);
        }
    return out;
}

static bool shown(wpo_object *self, const wpo_property_def *p)
{
    if (p->flags & WPO_P_NOFIELD) return false;
    if (!p->show) return true;
    wpo_value r = wpo_v_void();
    bool ok = wpo_send(self, p->show, NULL, 0, &r) == WPO_OK && r.type == 'b' && r.u.b;
    wpo_value_clear(&r);
    return ok;
}

/* Whether an ancestor of `c` declares p's name (p redeclares it). */
static bool redeclared(const wpo_class *c, const wpo_property_def *p)
{
    return c->parent && wpo_find_property(c->parent, p->name, NULL) != NULL;
}

static char *field_lines(wpo_object *self, const wpo_property_def *p, const char **page)
{
    if (!shown(self, p)) return wps_strdup("");
    char *v = wps_prop_get(self, p->name);
    char *ch = wps_prop_choices(self, p);
    char shownv[32];
    const char *fv = v ? v : "";
    if (!strcmp(p->type, "choice")) { snprintf(shownv, sizeof shownv, "%d", wps_option_index(ch, fv)); fv = shownv; }
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

char *wps_prop_pages(wpo_object *self, const wpo_class *only)
{
    const wpo_class *c[64];
    size_t n;
    chain(self->cls, c, &n);
    char *out = wps_strdup("");
    const char *page = NULL;
    /* pages marked first, then the others, then those marked last; each
     * property where it was first declared, as its nearest declaration (a
     * subclass's label) */
    for (int pass = 0; pass < 3; pass++)
        for (size_t k = 0; k < n; k++) {
            if (only && c[k] != only) continue;
            for (size_t i = 0; i < c[k]->nprops; i++) {
                const wpo_property_def *d = &c[k]->props[i];
                if (redeclared(c[k], d)) continue;   /* shown where its ancestor put it */
                const wpo_property_def *p = wpo_find_property(self->cls, d->name, NULL);
                int want = (p && (p->flags & WPO_P_FIRST)) ? 0 : (p && (p->flags & WPO_P_LAST)) ? 2 : 1;
                if (!p || want != pass) continue;
                char *line = field_lines(self, p, &page);
                out = append(out, line);
                free(line);
            }
        }
    return out;
}
