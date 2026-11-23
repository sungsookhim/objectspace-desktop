/* wpsd: Menus and settings pages as data; their setters. */
#define _GNU_SOURCE
#include "wpsd.h"
#include "wps_props.h"

/* ---- menus and settings pages as data */

/* Menu text: id TAB label [TAB parent [TAB flags]] per line, as a tree. */
void put_menu(cbuf *b, const char *text, const char *parent)
{
    size_t n = 0;
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char line[512], *f[4] = { 0 };
        const char *e = strchr(l, '\n');
        snprintf(line, sizeof line, "%.*s", (int)(e ? e - l : (long)strlen(l)), l);
        int k = 0;
        for (char *p = line, *t; k < 4 && (t = strsep(&p, "\t")); k++) f[k] = t;
        if (k >= 2 && !strcmp(f[2] ? f[2] : "", parent)) n++;
    }
    cb_array(b, n);
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char line[512], *f[4] = { 0 };
        const char *e = strchr(l, '\n');
        snprintf(line, sizeof line, "%.*s", (int)(e ? e - l : (long)strlen(l)), l);
        int k = 0;
        for (char *p = line, *t; k < 4 && (t = strsep(&p, "\t")); k++) f[k] = t;
        if (k < 2 || strcmp(f[2] ? f[2] : "", parent)) continue;
        const char *flags = f[3] ? f[3] : "";
        cb_map(b, 5);
        cb_text(b, "id"); cb_int(b, atoll(f[0]));
        cb_text(b, "label"); cb_text(b, f[1]);
        cb_text(b, "enabled"); cb_bool(b, !strstr(flags, "disabled"));
        cb_text(b, "checked"); cb_bool(b, strstr(flags, "checked") != NULL);
        cb_text(b, "items"); put_menu(b, text, f[0]);
    }
}

static size_t parse_settings(const char *text, char pages[][64], int *page_of, struct field *fields, size_t max, size_t *npages)
{
    size_t nf = 0;
    int cur = -1;
    *npages = 0;
    for (const char *l = text; l && *l && nf < max; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char line[2048], *f[7] = { 0 };
        const char *e = strchr(l, '\n');
        snprintf(line, sizeof line, "%.*s", (int)(e ? e - l : (long)strlen(l)), l);
        int k = 0;
        for (char *p = line, *t; k < 7 && (t = strsep(&p, "\t")); k++) f[k] = t;
        if (k >= 2 && !strcmp(f[0], "page")) {
            /* a page named again (a subclass's or a plug-in's fields on its
             * parent's page) is the same page */
            cur = -1;
            for (size_t p = 0; p < *npages && cur < 0; p++) if (!strcmp(pages[p], f[1])) cur = (int)p;
            if (cur < 0 && *npages < 16) { snprintf(pages[*npages], 64, "%s", f[1]); cur = (int)(*npages)++; }
        } else if (k >= 6 && !strcmp(f[0], "field") && cur >= 0) {
            struct field *x = &fields[nf];
            page_of[nf++] = cur;
            snprintf(x->key, sizeof x->key, "%s", f[1]);
            snprintf(x->type, sizeof x->type, "%s", f[2]);
            snprintf(x->label, sizeof x->label, "%s", f[3]);
            snprintf(x->value, sizeof x->value, "%s", f[4]);
            snprintf(x->setter, sizeof x->setter, "%s", f[5]);
            snprintf(x->choices, sizeof x->choices, "%s", f[6] ? f[6] : "");
        }
    }
    return nf;
}

/* A declared property's field (setter "@prop"): whether the object has a
 * value of its own that Default can remove, and where an inherited one
 * comes from, as its notebook says it ("Folder default"). */
void field_origin(void *ctx, const char *key, bool *own, char *from, size_t fn)
{
    wpo_object *o = ctx;
    *own = false;
    from[0] = 0;
    const wpo_property_def *p = o ? wpo_find_property(o->cls, key, NULL) : NULL;
    if (!p || p->get || (p->flags & WPO_P_READONLY)) return;
    if (p->backing) {   /* kept where Linux keeps it */
        snprintf(from, fn, "%s", !strcmp(p->backing, "mimeapps") ? "shared with GNOME" : "its desktop entry");
        return;
    }
    char origin[160];
    char *v = wps_prop_get_origin(o, key, origin, sizeof origin);
    free(v);
    if (!strcmp(origin, "object")) { *own = true; return; }
    if (!strncmp(origin, "class:", 6)) {
        wpo_class *c = wpo_find_class(origin + 6);
        char *t = c ? WPObject_wpclsQueryTitle(c) : NULL;
        snprintf(from, fn, "%s default", t && *t ? t : origin + 6);
        free(t);
    } else if (!strncmp(origin, "type:", 5)) {
        snprintf(from, fn, "%s default", origin + 5);
    }
}

void put_settings_with(cbuf *b, const char *text, const char *icon_shown, origin_fn of, void *ctx)
{
    char pages[16][64];
    int page_of[64];
    struct field fields[64];
    size_t np, nf = parse_settings(text, pages, page_of, fields, 64, &np);
    cb_array(b, np);
    for (size_t p = 0; p < np; p++) {
        size_t cnt = 0;
        for (size_t i = 0; i < nf; i++) cnt += page_of[i] == (int)p;
        cb_map(b, 2);
        cb_text(b, "title"); cb_text(b, pages[p]);
        cb_text(b, "fields");
        cb_array(b, cnt);
        for (size_t i = 0; i < nf; i++) {
            if (page_of[i] != (int)p) continue;
            struct field *x = &fields[i];
            if (!strcmp(x->type, "icon") && !x->choices[0] && icon_shown && *icon_shown)   /* (no commas in it: one choice) */
                snprintf(x->choices, sizeof x->choices, "%s", icon_shown);
            bool own = false;
            char from[200] = "";
            if (!strcmp(x->setter, "@prop") && of) of(ctx, x->key, &own, from, sizeof from);
            cb_map(b, (x->choices[0] ? 5 : 4) + own + (from[0] != 0));
            if (own) { cb_text(b, "own"); cb_bool(b, true); }
            if (from[0]) { cb_text(b, "from"); cb_text(b, from); }
            cb_text(b, "key"); cb_text(b, x->key);
            cb_text(b, "type"); cb_text(b, x->type);
            cb_text(b, "label"); cb_text(b, x->label);
            cb_text(b, "value");
            if (!strcmp(x->type, "number") || !strcmp(x->type, "choice")) cb_int(b, atoll(x->value));
            else if (!strcmp(x->type, "checkbox")) cb_bool(b, !strcmp(x->value, "1") || !strcmp(x->value, "true"));
            else cb_text(b, x->value);
            if (x->choices[0]) {
                size_t nc = 1;
                for (const char *c = x->choices; *c; c++) nc += *c == ',';
                cb_text(b, "choices");
                cb_array(b, nc);
                char tmp[256];
                snprintf(tmp, sizeof tmp, "%s", x->choices);
                for (char *q = tmp, *t; (t = strsep(&q, ","));) cb_text(b, t);
            }
        }
    }
}

void put_settings(cbuf *b, wpo_object *o, const char *text, const char *icon_shown)
{
    put_settings_with(b, text, icon_shown, o ? field_origin : NULL, o);
}

char *call_text(wpo_object *o, const char *method, const char *arg)
{
    wpo_value a = wpo_v_string(arg), r;
    char *out = NULL;
    if (wpo_send(o, method, &a, 1, &r) == WPO_OK && r.type == 's') { out = r.u.s; r.u.s = NULL; }
    wpo_value_clear(&a);
    wpo_value_clear(&r);
    return out ? out : strdup("");
}

/* The folder an object's icon sits in (loaded: a window lists it), and
 * its path; NULL if not known. */
static wpo_object *home_folder_of(wpo_object *o, char *fpath, size_t n)
{
    sobj *s = wps_sobj_of(o), *fs = NULL;
    if (!s) return NULL;
    if (s->central) fs = s->folder_id[0] ? wps_sobj_by_id(s->folder_id) : NULL;
    else {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s", s->path);
        char *sl = strrchr(p, '/');
        if (!sl || !sl[1]) return NULL;
        if (sl == p) sl[1] = 0;
        else *sl = 0;
        fs = wps_sobj_by_path(p);
    }
    if (!fs || !fs->o || !wpo_is_a(fs->o, WPFolderClass)) return NULL;
    snprintf(fpath, n, "%s", fs->path);
    return fs->o;
}

/* Its pages, and on the General page Lock position (v0.3-design 5: kept
 * by the folder it sits in, with its position). */
char *settings_text(wpo_object *o)
{
    char *t = call_text(o, "wpAddSettingsPages", ""), fpath[PATH_MAX];
    wpo_object *f = home_folder_of(o, fpath, sizeof fpath);
    char *g = strstr(t, "page\tGeneral\n");
    if (!f || !g || (g != t && g[-1] != '\n')) return t;
    char *next = strstr(g + 1, "\npage\t");
    size_t at = next ? (size_t)(next - t) + 1 : strlen(t);
    if (!next && at && t[at - 1] != '\n') return t;
    char line[160];
    snprintf(line, sizeof line, "field\tlock_pos\tcheckbox\tLock position\t%d\t@lock\n", WPFolder_wpQueryIconLock(f, o) ? 1 : 0);
    size_t tl = strlen(t), ll = strlen(line);
    char *r = malloc(tl + ll + 1);
    memcpy(r, t, at);
    memcpy(r + at, line, ll);
    memcpy(r + at + ll, t + at, tl - at + 1);
    free(t);
    return r;
}

/* settings_set: apply {key: value} through each field's setter. */
const char *apply_settings(wpo_object *o, const cv *values)
{
    char *text = settings_text(o);
    char pages[16][64];
    int page_of[64];
    struct field fields[64];
    size_t np, nf = parse_settings(text, pages, page_of, fields, 64, &np);
    free(text);
    const char *err = NULL;
    for (size_t k = 0; values && values->k == CV_MAP && k < values->n && !err; k++) {
        const cv *key = &values->items[2 * k], *val = &values->items[2 * k + 1];
        struct field *f = NULL;
        for (size_t i = 0; i < nf && !f; i++) if (key->k == CV_TEXT && !strcmp(fields[i].key, key->s)) f = &fields[i];
        if (!f) { err = "no such setting"; break; }
        wpo_value a, r;
        if (!strcmp(f->setter, "@lock")) {
            char fpath[PATH_MAX];
            wpo_object *fo = home_folder_of(o, fpath, sizeof fpath);
            bool on = val->k == CV_BOOL ? val->b : val->k == CV_INT ? val->i != 0 : false;
            if (!fo || !WPFolder_wpSetIconLock(fo, o, on)) { err = "the setter refused the value"; break; }
            on_layout_changed(fpath);   /* its windows read the positions again */
            WPObject_wpObjectChanged(o);  /* (its notebook shows it) */
            continue;
        }
        if (!strncmp(f->setter, "@col:", 5)) {
            /* a details view's column on or off: wpSetColumn("name=1") */
            char arg[64];
            bool on = val->k == CV_BOOL ? val->b : val->k == CV_INT ? val->i != 0 : false;
            snprintf(arg, sizeof arg, "%s=%d", f->setter + 5, on);
            wpo_value av = wpo_v_string(arg);
            if (wpo_send(o, "wpSetColumn", &av, 1, &r) != WPO_OK || (r.type == 'b' && !r.u.b)) err = "the setter refused the value";
            wpo_value_clear(&av);
            wpo_value_clear(&r);
            continue;
        }
        if (!strcmp(f->setter, "@prop")) {
            /* a declared property (object-properties-design.md 2.1), as text */
            char num[32];
            const char *txt = val->k == CV_TEXT ? val->s : NULL;
            if (val->k == CV_INT) { snprintf(num, sizeof num, "%lld", (long long)val->i); txt = num; }
            else if (val->k == CV_BOOL) txt = val->b ? "1" : "0";
            if (!txt) { err = "unsupported value"; break; }
            if (!WPObject_wpSetProperty(o, f->key, txt)) { err = "the setter refused the value"; break; }
            if ((!strcmp(f->key, "sort_key") || !strcmp(f->key, "sort_desc")) && wpo_is_a(o, WPFileSystemClass))
                wps_listing_invalidate(WPFileSystem_data(o)->path);
            continue;
        }
        if (!strcmp(f->setter, "@")) {
            /* A settings object's field: wpSetSetting(key, value as text). */
            char num[32];
            const char *txt = val->k == CV_TEXT ? val->s : NULL;
            if (val->k == CV_INT) { snprintf(num, sizeof num, "%lld", (long long)val->i); txt = num; }
            else if (val->k == CV_BOOL) txt = val->b ? "1" : "0";
            if (!txt) { err = "unsupported value"; break; }
            wpo_value args[2] = { wpo_v_string(f->key), wpo_v_string(txt) };
            if (wpo_send(o, "wpSetSetting", args, 2, &r) != WPO_OK || (r.type == 'b' && !r.u.b)) err = "the setter refused the value";
            wpo_value_clear(&args[0]);
            wpo_value_clear(&args[1]);
            wpo_value_clear(&r);
            continue;
        }
        if (val->k == CV_TEXT) a = wpo_v_string(val->s);
        else if (val->k == CV_INT) a = wpo_v_int(val->i);
        else if (val->k == CV_BOOL) a = wpo_v_bool(val->b);
        else { err = "unsupported value"; break; }
        if (wpo_send(o, f->setter, &a, 1, &r) != WPO_OK) err = "the setter refused the value";
        else if (r.type == 'b' && !r.u.b) err = "the setter refused the value";
        wpo_value_clear(&a);
        wpo_value_clear(&r);
        /* a folder's sort: its listing is read again in the new order */
        if (!err && (!strcmp(f->key, "sort_key") || !strcmp(f->key, "sort_desc")) && wpo_is_a(o, WPFileSystemClass))
            wps_listing_invalidate(WPFileSystem_data(o)->path);
    }
    return err;
}
