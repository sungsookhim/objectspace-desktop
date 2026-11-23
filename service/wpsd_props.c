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
#include "wps_props.h"

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

static void put_prop(cbuf *b, wpo_object *o, const wpo_property_def *p, const wpo_class *decl)
{
    char *v = wps_prop_get(o, p->name);
    /* its own value: kept in its state (or given by its class's method) */
    bool own = p->get != NULL;
    if (!own) {
        char *s = NULL;
        int64_t i;
        const char *st = p->store ? p->store : decl->name;
        own = wps_state_get_str(o, st, p->name, &s) || wps_state_get_int(o, st, p->name, &i);
        free(s);
    }
    cb_map(b, 7);
    cb_text(b, "name"); cb_text(b, p->name);
    cb_text(b, "value"); cb_text(b, v ? v : "");
    cb_text(b, "type"); cb_text(b, p->type);
    cb_text(b, "page"); cb_text(b, p->page ? p->page : "General");
    cb_text(b, "label"); cb_text(b, p->label ? p->label : p->name);
    cb_text(b, "choices"); cb_text(b, p->choices ? p->choices : "");
    cb_text(b, "origin"); cb_text(b, own ? "object" : "default");
    free(v);
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
        char k[128], v[4096], msg[256];
        bool ok;
        for (const char *s = setup; (s = next_pair(s, k, sizeof k, v, sizeof v, &ok));) {
            if (!ok) { reply_error(r->c, r->id, "bad request", "setup strings are key=value;key=value"); return; }
            if (!wpo_find_property(o->cls, k, NULL)) {
                snprintf(msg, sizeof msg, "%s: no such property", k);
                reply_error(r->c, r->id, "bad request", msg);
                return;
            }
            if (!WPObject_wpSetProperty(o, k, v)) {
                snprintf(msg, sizeof msg, "%s: the value was refused", k);
                reply_error(r->c, r->id, "bad request", msg);
                return;
            }
            if ((!strcmp(k, "sort_key") || !strcmp(k, "sort_desc")) && wpo_is_a(o, WPFileSystemClass))
                wps_listing_invalidate(WPFileSystem_data(o)->path);
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
            if (wpo_find_property(o->cls, p->name, NULL) == p) put_prop(&b, o, p, chain[k]);
        }
    send_frame(r->c, &b);
    cb_free(&b);
}
