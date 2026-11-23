/* wpsd: declared properties over the protocol (object-properties-design.md
 * 2.5): get one, set some with a setup string, list them all.
 *   {op:"get", target, name}              its value, as text
 *   {op:"set", target, setup, expect?}    "key=value;key=value" (\; and \\
 *                                         inside a value); applied in order,
 *                                         stopping at the first refused
 *   {op:"props", target}                  [{name, value, type, page, label,
 *                                         choices, origin}] (origin: "object"
 *                                         if this object has its own value,
 *                                         else "default")
 */
#define _GNU_SOURCE
#include "wpsd.h"
#include "defaults.h"
#include "wps_props.h"

/* ---- the chain (object-properties-design.md 2.2): an object's own value,
 * then its type's (type:image/png, type:image/\*), then its class's and
 * each class's above it up to the one declaring the property
 * (class:WPFolder, ...), then the declared default. */

#define MAX_SCOPES 24

/* The scopes above an object (or a scope) for property p, nearest first. */
static size_t scopes_for(const wpo_class *cls, const char *mime, const wpo_property_def *p, char out[][160])
{
    size_t n = 0;
    if (mime && *mime) {
        snprintf(out[n++], 160, "type:%s", mime);
        const char *sl = strchr(mime, '/');
        if (sl && strcmp(sl, "/*")) snprintf(out[n++], 160, "type:%.*s/*", (int)(sl - mime), mime);
    }
    for (const wpo_class *c = cls; c && n < MAX_SCOPES; c = c->parent) {
        snprintf(out[n++], 160, "class:%s", c->name);
        bool declares = false;
        for (size_t i = 0; i < c->nprops && !declares; i++) declares = &c->props[i] == p || !strcmp(c->props[i].name, p->name);
        if (declares && !(c->parent && wpo_find_property(c->parent, p->name, NULL))) break;
    }
    return n;
}

static char *first_in(char sc[][160], size_t n, size_t from, const char *store, const char *name, char *origin, size_t on)
{
    for (size_t i = from; i < n; i++) {
        char *v = wps_defaults_get(sc[i], store, name);
        if (v) {
            if (origin && on) snprintf(origin, on, "%s", sc[i]);
            return v;
        }
    }
    return NULL;
}

static char *mime_of(wpo_object *self)
{
    wpo_value r = wpo_v_void();
    if (wpo_send(self, "wpQueryMimeType", NULL, 0, &r) != WPO_OK || r.type != 's') { wpo_value_clear(&r); return NULL; }
    char *m = r.u.s ? strdup(r.u.s) : NULL;
    wpo_value_clear(&r);
    return m;
}

static char *chain_value(wpo_object *self, const wpo_property_def *p, const char *store, char *origin, size_t on)
{
    char sc[MAX_SCOPES][160];
    char *mime = mime_of(self);
    size_t n = scopes_for(self->cls, mime, p, sc);
    free(mime);
    return first_in(sc, n, 0, store, p->name, origin, on);
}

static void defaults_broadcast(const char *scope);
void props_broadcast(const char *scope) { defaults_broadcast(scope); }

static bool scope_set_hook(const char *scope, const char *store, const char *prop, const char *value)
{
    if (wps_defaults_set(scope, store, prop, value)) return false;
    defaults_broadcast(scope);
    return true;
}

void props_init(void)
{
    wps_loaded l;
    wps_load(env.config_dir, false, &l);
    wps_defaults_init(db, l.err ? "" : l.vol);
    wps_loaded_free(&l);
    wps_prop_inherit = chain_value;
    wps_scope_get = wps_defaults_get;
    wps_scope_set = scope_set_hook;
    wps_scheme_titles = scheme_titles;
}

/* A class's or a type's settings changed: every client (once) reads what it
 * shows again. */
static void defaults_broadcast(const char *scope)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead) continue;
        bool sent = false;
        for (struct sub *y = subs; y != x && !sent; y = y->next) sent = y->c == x->c;
        if (sent) continue;
        cbuf b;
        event_head(&b, "defaults_changed", x->id, 1);
        cb_text(&b, "scope"); cb_text(&b, scope);
        send_frame(x->c, &b);
        cb_free(&b);
    }
}

/* The next "key=value" of a setup string, unescaped into k and v; the
 * string after it, or NULL at its end. */
static const char *next_pair(const char *s, char *k, size_t nk, char *v, size_t nv, bool *ok)
{
    while (*s == ';' || *s == ' ') s++;
    if (!*s) return NULL;
    size_t ik = 0, iv = 0;
    bool in_v = false;
    *ok = true;
    for (; *s && *s != ';'; s++) {
        char ch = *s;
        if (ch == '\\' && (s[1] == ';' || s[1] == '\\')) ch = *++s;
        else if (ch == '=' && !in_v) { in_v = true; continue; }
        if (!in_v) { if (ik + 1 < nk) k[ik++] = ch; }
        else if (iv + 1 < nv) v[iv++] = ch;
        else *ok = false;
    }
    while (ik && k[ik - 1] == ' ') ik--;
    k[ik] = 0;
    v[iv] = 0;
    if (!in_v || !ik) *ok = false;
    return s;
}

static void put_prop_value(cbuf *b, const wpo_property_def *p, const char *v, const char *origin)
{
    cb_map(b, 7);
    cb_text(b, "name"); cb_text(b, p->name);
    cb_text(b, "value"); cb_text(b, v ? v : "");
    cb_text(b, "type"); cb_text(b, p->type);
    cb_text(b, "page"); cb_text(b, p->page ? p->page : "General");
    cb_text(b, "label"); cb_text(b, p->label ? p->label : p->name);
    cb_text(b, "choices"); cb_text(b, p->choices ? p->choices : "");
    cb_text(b, "origin"); cb_text(b, origin);
}

static void put_prop(cbuf *b, wpo_object *o, const wpo_property_def *p)
{
    char origin[160];
    char *v = wps_prop_get_origin(o, p->name, origin, sizeof origin);
    char *ch = wps_prop_choices(o, p);
    wpo_property_def q = *p;   /* (its options now) */
    q.choices = ch;
    put_prop_value(b, &q, v, origin);
    free(v);
    free(ch);
}

bool apply_setup(wpo_object *o, const char *setup, char *err, size_t en)
{
    char k[128], v[4096];
    bool ok;
    for (const char *s = setup; (s = next_pair(s, k, sizeof k, v, sizeof v, &ok));) {
        if (!ok) { snprintf(err, en, "setup strings are key=value;key=value"); return false; }
        if (!wpo_find_property(o->cls, k, NULL)) { snprintf(err, en, "%s: no such property", k); return false; }
        if (!WPObject_wpSetProperty(o, k, v)) { snprintf(err, en, "%s: the value was refused", k); return false; }
        if ((!strcmp(k, "sort_key") || !strcmp(k, "sort_desc")) && wpo_is_a(o, WPFileSystemClass))
            wps_listing_invalidate(WPFileSystem_data(o)->path);
    }
    return true;
}

void do_props(struct req *r, wpo_object *o)
{
    int64_t ex;
    if (!strcmp(r->op, "get")) {
        const char *name = cv_text(&r->msg, "name");
        char *v = name ? WPObject_wpQueryProperty(o, name) : NULL;
        if (!v) { reply_error(r->c, r->id, "not found", "no such property"); return; }
        cbuf b;
        cb_init(&b);
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, r->id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "result"); cb_text(&b, v);
        send_frame(r->c, &b);
        cb_free(&b);
        free(v);
        return;
    }
    if (!strcmp(r->op, "set")) {
        const char *setup = cv_text(&r->msg, "setup");
        if (!setup) { reply_error(r->c, r->id, "bad request", "set needs a setup string"); return; }
        if (cv_int(&r->msg, "expect", &ex) && (uint64_t)ex != WPObject_data(o)->changes) {
            reply_error(r->c, r->id, "conflict", "conflict");
            return;
        }
        char msg[256];
        if (!apply_setup(o, setup, msg, sizeof msg)) { reply_error(r->c, r->id, "bad request", msg); return; }
        reply_ok_object(r, o, NULL);
        return;
    }
    if (!strcmp(r->op, "reset")) {
        /* {op:"reset", target, names:"a;b"}: their own values removed */
        const char *names = cv_text(&r->msg, "names");
        char nm[128];
        for (const char *q = names; q && *q;) {
            const char *e = strchr(q, ';');
            snprintf(nm, sizeof nm, "%.*s", (int)(e ? e - q : (long)strlen(q)), q);
            if (*nm && !wps_prop_reset(o, nm)) {
                char msg[200];
                snprintf(msg, sizeof msg, "%s: no value of its own to remove", nm);
                reply_error(r->c, r->id, "bad request", msg);
                return;
            }
            q = e ? e + 1 : NULL;
        }
        reply_ok_object(r, o, NULL);
        return;
    }
    /* props: every declared property, the root class's first */
    const wpo_class *chain[64];
    size_t nc = 0;
    for (const wpo_class *c = o->cls; c && nc < 64; c = c->parent) chain[nc++] = c;
    size_t count = 0;
    for (size_t k = nc; k-- > 0;)
        for (size_t i = 0; i < chain[k]->nprops; i++)
            count += wpo_find_property(o->cls, chain[k]->props[i].name, NULL) == &chain[k]->props[i];
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    cb_array(&b, count);
    for (size_t k = nc; k-- > 0;)
        for (size_t i = 0; i < chain[k]->nprops; i++) {
            const wpo_property_def *p = &chain[k]->props[i];
            if (wpo_find_property(o->cls, p->name, NULL) == p) put_prop(&b, o, p);
        }
    send_frame(r->c, &b);
    cb_free(&b);
}

/* ---- class:NAME and type:MIME targets: get, set, props, reset of the
 * settings of all objects of a class or a type (no object is loaded). */

static void reply_text(struct client *c, int64_t id, const char *v)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result"); cb_text(&b, v);
    send_frame(c, &b);
    cb_free(&b);
}

/* its value: the scope's own, else the scopes above it, else the default */
static char *scope_value(const wpo_class *cls, const char *target, const wpo_property_def *p, const char *store, char *origin, size_t on)
{
    char sc[MAX_SCOPES][160];
    const char *mime = !strncmp(target, "type:", 5) ? target + 5 : NULL;
    size_t n = scopes_for(cls, mime, p, sc);
    char *v = first_in(sc, n, 0, store, p->name, origin, on);
    if (v) return v;
    snprintf(origin, on, "default");
    return strdup(p->def ? p->def : "");
}

/* A scope's notebook: the pages of what all of its objects can share (not
 * an object's own, like its title, nor information). */
struct scope_ctx { const wpo_class *cls; const char *target; };

static bool shared_prop(const wpo_property_def *p)
{
    return !p->get && !(p->flags & (WPO_P_READONLY | WPO_P_NOFIELD)) && strcmp(p->type, "info");
}

static void scope_origin(void *ctx, const char *key, bool *own, char *from, size_t fn)
{
    struct scope_ctx *x = ctx;
    *own = false;
    from[0] = 0;
    const wpo_property_def *p = wpo_find_property(x->cls, key, NULL);
    if (!p) return;
    char origin[160];
    free(scope_value(x->cls, x->target, p, wps_prop_store(x->cls, key), origin, sizeof origin));
    if (!strcmp(origin, x->target)) { *own = true; return; }
    if (!strncmp(origin, "class:", 6)) {
        wpo_class *c = wpo_find_class(origin + 6);
        char *t = c ? WPObject_wpclsQueryTitle(c) : NULL;
        snprintf(from, fn, "%s default", t && *t ? t : origin + 6);
        free(t);
    } else if (!strncmp(origin, "type:", 5)) {
        snprintf(from, fn, "%s default", origin + 5);
    }
}

static char *scope_pages(const wpo_class *cls, const char *target)
{
    const wpo_class *chain[64];
    size_t nc = 0;
    for (const wpo_class *k = cls; k && nc < 64; k = k->parent) chain[nc++] = k;
    char *out = strdup("");
    const char *page = NULL;
    char origin[160];
    for (int pass = 0; pass < 3; pass++)   /* pages marked first, the others, those marked last (Actions) */
    for (size_t j = nc; j-- > 0;)
        for (size_t i = 0; i < chain[j]->nprops; i++) {
            const wpo_property_def *d = &chain[j]->props[i];
            if (chain[j]->parent && wpo_find_property(chain[j]->parent, d->name, NULL)) continue;   /* where first declared */
            const wpo_property_def *p = wpo_find_property(cls, d->name, NULL);
            if (!shared_prop(p)) continue;
            if (((p->flags & WPO_P_FIRST) ? 0 : (p->flags & WPO_P_LAST) ? 2 : 1) != pass) continue;
            char *v = scope_value(cls, target, p, wps_prop_store(cls, p->name), origin, sizeof origin);
            char shown[32];
            const char *fv = v ? v : "";
            if (!strcmp(p->type, "choice")) { snprintf(shown, sizeof shown, "%d", wps_option_index(p->choices, fv)); fv = shown; }
            const char *pg = p->page ? p->page : "General";
            size_t l = strlen(pg) + strlen(p->name) + strlen(p->label ? p->label : "") + strlen(fv) + strlen(p->choices ? p->choices : "") + 64;
            char *line = malloc(l);
            size_t o = 0;
            if (!page || strcmp(page, pg)) o += (size_t)snprintf(line, l, "page\t%s\n", pg);
            page = pg;
            snprintf(line + o, l - o, "field\t%s\t%s\t%s\t%s\t@prop%s%s\n", p->name, p->type, p->label ? p->label : p->name, fv,
                     p->choices ? "\t" : "", p->choices ? p->choices : "");
            char *r = malloc(strlen(out) + strlen(line) + 1);
            strcpy(r, out);
            strcat(r, line);
            free(out);
            free(line);
            free(v);
            out = r;
        }
    return out;
}

static void reply_scope_settings(struct client *c, int64_t id, const wpo_class *cls, const char *target)
{
    char *t = scope_pages(cls, target);
    struct scope_ctx x = { cls, target };
    char title[200];
    if (!strncmp(target, "class:", 6)) {
        char *ct = WPObject_wpclsQueryTitle((wpo_class *)cls);
        snprintf(title, sizeof title, "All %s objects", ct && *ct ? ct : cls->name);
        free(ct);
    } else {
        snprintf(title, sizeof title, "All %s files", target + 5);
    }
    cbuf b;
    cb_init(&b);
    cb_map(&b, 4);
    cb_text(&b, "id"); cb_int(&b, id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    put_settings_with(&b, t, NULL, scope_origin, &x);
    cb_text(&b, "object");
    cb_map(&b, 3);
    cb_text(&b, "address"); cb_text(&b, target);
    cb_text(&b, "title"); cb_text(&b, title);
    cb_text(&b, "changes"); cb_uint(&b, 0);
    send_frame(c, &b);
    cb_free(&b);
    free(t);
}

bool do_scope(struct client *c, int64_t id, const char *op, cv *msg, const char *target)
{
    const wpo_class *cls = NULL;
    if (!strncmp(target, "class:", 6)) {
        wpo_class *k = wpo_find_class(target + 6);
        if (k && !k->is_meta && wpo_subclass_of(k, WPObjectClass)) cls = k;
    } else if (strchr(target + 5, '/')) {
        cls = WPDataFileClass;   /* files of a type */
    }
    if (!cls) { reply_error(c, id, "not found", "no such class or type"); return false; }
    char origin[160], m[256];
    if (!strcmp(op, "settings")) { reply_scope_settings(c, id, cls, target); return false; }
    if (!strcmp(op, "settings_set")) {
        /* its notebook's changes: {key: value} (number, checkbox or text) */
        const cv *values = cv_get(msg, "values");
        for (size_t k = 0; values && values->k == CV_MAP && k < values->n; k++) {
            const cv *key = &values->items[2 * k], *val = &values->items[2 * k + 1];
            const wpo_property_def *p = key->k == CV_TEXT ? wpo_find_property(cls, key->s, NULL) : NULL;
            if (!p || !shared_prop(p)) { reply_error(c, id, "bad request", "no such setting"); return false; }
            char num[32];
            const char *txt = val->k == CV_TEXT ? val->s : NULL;
            if (val->k == CV_INT) { snprintf(num, sizeof num, "%lld", (long long)val->i); txt = num; }
            else if (val->k == CV_BOOL) txt = val->b ? "1" : "0";
            char *cvv = txt ? wps_prop_canonical(p, txt) : NULL;
            if (!cvv) { reply_error(c, id, "bad request", "the setter refused the value"); return false; }
            int e = wps_defaults_set(target, wps_prop_store(cls, p->name), p->name, cvv);
            free(cvv);
            if (e) { reply_error(c, id, "failed", "the setting was not stored"); return false; }
        }
        defaults_broadcast(target);
        reply_scope_settings(c, id, cls, target);
        return false;
    }
    if (!strcmp(op, "get")) {
        const char *name = cv_text(msg, "name");
        const wpo_property_def *p = name ? wpo_find_property(cls, name, NULL) : NULL;
        if (!p || p->get) { reply_error(c, id, "not found", "no such property for all objects of it"); return false; }
        char *v = scope_value(cls, target, p, wps_prop_store(cls, name), origin, sizeof origin);
        reply_text(c, id, v);
        free(v);
        return false;
    }
    if (!strcmp(op, "set") || !strcmp(op, "reset")) {
        const char *list = cv_text(msg, !strcmp(op, "set") ? "setup" : "names");
        if (!list) { reply_error(c, id, "bad request", "set needs a setup string, reset names"); return false; }
        char k[128], v[4096];
        bool ok = true;
        for (const char *q = list; q && *q;) {
            if (!strcmp(op, "set")) {
                q = next_pair(q, k, sizeof k, v, sizeof v, &ok);
                if (!q) break;
                if (!ok) { reply_error(c, id, "bad request", "setup strings are key=value;key=value"); return false; }
            } else {
                const char *e = strchr(q, ';');
                snprintf(k, sizeof k, "%.*s", (int)(e ? e - q : (long)strlen(q)), q);
                q = e ? e + 1 : NULL;
                if (!*k) continue;
            }
            const wpo_property_def *p = wpo_find_property(cls, k, NULL);
            if (!p || p->get || (p->flags & WPO_P_READONLY) || !strcmp(p->type, "info")) {
                snprintf(m, sizeof m, "%s: not a setting for all objects of it", k);
                reply_error(c, id, "bad request", m);
                return false;
            }
            char *cv = !strcmp(op, "set") ? wps_prop_canonical(p, v) : NULL;
            if (!strcmp(op, "set") && !cv) {
                snprintf(m, sizeof m, "%s: the value was refused", k);
                reply_error(c, id, "bad request", m);
                return false;
            }
            int e = wps_defaults_set(target, wps_prop_store(cls, k), k, cv);
            free(cv);
            if (e) { reply_error(c, id, "failed", "the setting was not stored"); return false; }
        }
        defaults_broadcast(target);
        reply_text(c, id, target);
        return false;
    }
    if (!strcmp(op, "props")) {
        const wpo_class *chain[64];
        size_t nc = 0;
        for (const wpo_class *k = cls; k && nc < 64; k = k->parent) chain[nc++] = k;
        size_t count = 0;
        for (size_t j = nc; j-- > 0;)
            for (size_t i = 0; i < chain[j]->nprops; i++) {
                const wpo_property_def *p = &chain[j]->props[i];
                count += wpo_find_property(cls, p->name, NULL) == p && !p->get && strcmp(p->type, "info");
            }
        cbuf b;
        cb_init(&b);
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "result");
        cb_array(&b, count);
        for (size_t j = nc; j-- > 0;)
            for (size_t i = 0; i < chain[j]->nprops; i++) {
                const wpo_property_def *p = &chain[j]->props[i];
                if (wpo_find_property(cls, p->name, NULL) != p || p->get || !strcmp(p->type, "info")) continue;
                char *v = scope_value(cls, target, p, wps_prop_store(cls, p->name), origin, sizeof origin);
                put_prop_value(&b, p, v, !strcmp(origin, target) ? "object" : origin);
                free(v);
            }
        send_frame(c, &b);
        cb_free(&b);
        return false;
    }
    reply_error(c, id, "bad request", "a class or a type has get, set, props and reset");
    return false;
}
