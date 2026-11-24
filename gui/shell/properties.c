/* wps-shell: Properties notebooks. */
#define _GNU_SOURCE
#include "shell.h"

struct props *props_open;

static long next_props_id = 1;

static struct props *props_by_id(long id)
{
    for (struct props *p = props_open; p; p = p->next)
        if (p->id == id) return p;
    return NULL;
}

/* The service's pages, as notebook pages (strings point into `res`). */
static ui_page_desc *pages_from(const cv *res, size_t *n)
{
    size_t total = res && res->k == CV_ARRAY ? res->n : 0;
    ui_page_desc *pages = calloc(total ? total : 1, sizeof *pages);
    *n = 0;
    for (size_t i0 = 0; i0 < total; i0++) {
        size_t i = *n;
        const cv *pg = &res->items[i0], *fs = cv_get(pg, "fields");
        pages[i].title = cv_text(pg, "title");
        size_t nf = fs && fs->k == CV_ARRAY ? fs->n : 0;
        ui_field_desc *fields = calloc(nf ? nf : 1, sizeof *fields);
        size_t kept = 0;
        for (size_t k = 0; k < nf; k++) {
            const cv *fd = &fs->items[k], *v = cv_get(fd, "value"), *ch = cv_get(fd, "choices");
            const char *type = cv_text(fd, "type");
            if (type && !strcmp(type, "hidden")) continue;   /* (kept by the GUI itself: column widths) */
            ui_field_desc *d = &fields[kept++];
            const cv *own = cv_get(fd, "own");
            d->own = own && own->k == CV_BOOL && own->b;
            d->from = cv_text(fd, "from");
            d->key = cv_text(fd, "key");
            d->label = cv_text(fd, "label");
            d->kind = !type || !strcmp(type, "text") ? UI_FIELD_TEXT : !strcmp(type, "checkbox") ? UI_FIELD_CHECKBOX
                      : !strcmp(type, "number") ? UI_FIELD_NUMBER : !strcmp(type, "icon") ? UI_FIELD_ICON
                      : !strcmp(type, "info") ? UI_FIELD_INFO : !strcmp(type, "file") ? UI_FIELD_FILE
                      : !strcmp(type, "color") ? UI_FIELD_COLOR : UI_FIELD_CHOICE;
            if (v && v->k == CV_TEXT) d->value.text = (char *)v->s;
            static char shown_png[4][PATH_MAX];   /* (an icon field shows PNGs: a package or a theme's as one) */
            static int nshown;
            if (d->kind == UI_FIELD_ICON && d->value.text && icons_object_png(d->value.text, 128, shown_png[nshown % 4], sizeof shown_png[0]))
                d->value.text = shown_png[nshown++ % 4];
            else if (v && v->k == CV_INT) d->value.number = v->i;
            else if (v && v->k == CV_BOOL) d->value.checked = v->b;
            if (ch && ch->k == CV_ARRAY) {
                const char **c = calloc(ch->n ? ch->n : 1, sizeof *c);
                for (size_t j = 0; j < ch->n; j++) c[j] = ch->items[j].k == CV_TEXT ? ch->items[j].s : "";
                static char themed[4][PATH_MAX];   /* (an icon shown from a theme: its PNG) */
                static int nthemed;
                if (d->kind == UI_FIELD_ICON && ch->n && !strncmp(c[0], "theme:", 6) &&
                    icons_theme_png(NULL, c[0] + 6, 128, themed[nthemed % 4], sizeof themed[0]))
                    c[0] = themed[nthemed++ % 4];
                d->choices = c;
                d->nchoices = ch->n;
            }
        }
        pages[i].fields = fields;
        pages[i].nfields = kept;
        if (kept) (*n)++;   /* a page with nothing shown (only hidden fields): none */
        else free(fields);
    }
    return pages;
}

static void pages_free(ui_page_desc *pages, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        for (size_t k = 0; k < pages[i].nfields; k++) free((void *)pages[i].fields[k].choices);
        free((void *)pages[i].fields);
    }
    free(pages);
}

static void note_object(struct props *p, const cv *reply)
{
    const cv *o = reply ? cv_get(reply, "object") : NULL;
    int64_t ch;
    if (o && cv_int(o, "changes", &ch)) p->changes = ch;
    const char *path = o ? cv_text(o, "path") : NULL;
    if (path && *path) snprintf(p->target, sizeof p->target, "%s", path);   /* renamed */
    const char *c = o ? cv_text(o, "class") : NULL, *ct = o ? cv_text(o, "class_title") : NULL;
    const char *mk = o ? cv_text(o, "makes") : NULL, *mt = o ? cv_text(o, "makes_title") : NULL;
    p->makes = mk != NULL;
    if (mk) { c = mk; ct = mt; }   /* a template: the class it makes (Folder for the Folder template) */
    if (c) snprintf(p->cls, sizeof p->cls, "%s", c);
    if (ct) snprintf(p->cls_title, sizeof p->cls_title, "%s", ct);
}

static void page_previews(struct props *p, ui_page_desc *pages, size_t n);

static void props_refreshed(void *arg, const cv *reply)
{
    struct props *p = props_by_id((long)(intptr_t)arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!p || !res || !p->nb) return;
    note_object(p, reply);
    size_t n;
    ui_page_desc *pages = pages_from(res, &n);
    page_previews(p, pages, n);
    ui_notebook_update(p->nb, pages, n);
    pages_free(pages, n);
}

static void props_refresh(struct props *p) { svc_settings(p->target, props_refreshed, (void *)(intptr_t)p->id); }

static void preview_ready(void *ctx, const char *png)
{
    struct props *p = props_by_id((long)(intptr_t)ctx);
    if (p && p->nb && png) props_refresh(p);   /* (shown now: made before) */
}

/* The Background page's polish (0.4 step 7): a picture's preview beside
 * its path (made off the UI thread if it is not a PNG); with no colour of
 * its own, the colour used then (the scheme's desktop teal, or a folder's
 * window background), marked "default". */
static void page_previews(struct props *p, ui_page_desc *pages, size_t n)
{
    static char pics[4][PATH_MAX], dflt[4][8];
    static int npic, ndflt;
    bool desk = false;
    for (struct folder *f = folders; f && !desk; f = f->next) desk = f->desktop && !strcmp(f->path, p->target);
    for (size_t i = 0; i < n; i++)
        for (size_t k = 0; k < pages[i].nfields; k++) {
            ui_field_desc *d = (ui_field_desc *)&pages[i].fields[k];
            if (d->kind == UI_FIELD_FILE && d->nchoices && !strcmp(d->choices[0], "image") && d->value.text && *d->value.text) {
                char *out = pics[npic++ % 4];
                if (icons_preview(d->value.text, 96, out, PATH_MAX, preview_ready, (void *)(intptr_t)p->id)) d->preview = out;
            } else if (d->kind == UI_FIELD_COLOR && !strcmp(d->key, "bg_color")) {
                ui_color c = desk ? ui_scheme_now.desktop : ui_scheme_now.client_bg;
                char *out = dflt[ndflt++ % 4];
                snprintf(out, 8, "#%02x%02x%02x", (int)lround(c.r * 255), (int)lround(c.g * 255), (int)lround(c.b * 255));
                d->preview = out;
            }
        }
}

static void props_applied(void *arg, const cv *reply)
{
    struct props *p = props_by_id((long)(intptr_t)arg);
    if (!p || !p->nb) return;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (ok && ok->k == CV_BOOL && ok->b) {
        note_object(p, reply);
        const cv *res = cv_get(reply, "result");
        if (res && res->k == CV_BOOL && !res->b)   /* a rename without permission */
            ui_notebook_set_status(p->nb, "No permission to rename the file: the new name is kept as its title");
        else ui_notebook_set_status(p->nb, "");
        props_refresh(p);
        return;
    }
    const char *code = reply ? cv_text(reply, "error") : NULL, *msg = reply ? cv_text(reply, "message") : NULL;
    char m[300];
    if (code && !strcmp(code, "conflict")) snprintf(m, sizeof m, "Changed elsewhere meanwhile: these are the current values");
    else snprintf(m, sizeof m, "Not changed: %s", msg ? msg : reply ? "refused" : "lost the connection to wpsd");
    ui_notebook_set_status(p->nb, m);
    props_refresh(p);
}

static void icon_made(void *ctx, const char *png)
{
    struct icon_req *r = ctx;
    struct props *p = props_by_id(r->props);
    if (p && p->nb) {
        if (!png) {
            char m[300];
            snprintf(m, sizeof m, "No icon made of it: %s. The icon is as it was.", icons_import_error());
            ui_notebook_set_status(p->nb, m);
        }
        else {
            cv val = { .k = CV_TEXT, .s = (char *)png, .len = strlen(png) };
            svc_settings_set(p->target, r->key, &val, p->changes, props_applied, (void *)(intptr_t)p->id);
            events_raise("object.icon", p->target, "properties", (ui_rect){ 0, 0, 0, 0 });
        }
    }
    free(r);
}

static void props_icon_from(long props, const char *key, const char *image)
{
    struct props *p = props_by_id(props);
    if (!p) return;
    if (p->nb) ui_notebook_set_status(p->nb, "Making the icon...");
    struct icon_req *r = calloc(1, sizeof *r);
    r->props = props;
    snprintf(r->key, sizeof r->key, "%s", key);
    icons_import(image, icon_made, r);
}

/* Browse...'s choice: into the field, which applies it. */
static void props_file_chosen(long owner, const char *key, const char *path)
{
    struct props *p = props_by_id(owner);
    if (p && p->nb) ui_notebook_set_value(p->nb, key, path);
}

void props_set(void *ctx, const char *key, ui_field_kind kind, const ui_value *v)
{
    struct props *p = ctx;
    void *arg = (void *)(intptr_t)p->id;
    if (kind == UI_FIELD_ICON) {
        if (!v->text) { iconpick_open(p->title, props_icon_from, p->id, key); return; }   /* Choose... */
        if (*v->text) { props_icon_from(p->id, key, v->text); return; }                     /* an image dropped */
        /* Default: "" below */
    }
    if (kind == UI_FIELD_FILE && v->text && v->checked) {   /* a sound's Play */
        if (!events_play(v->text)) ui_notebook_set_status(p->nb, "That sound cannot be played (not found, or a format not read)");
        return;
    }
    if (kind == UI_FIELD_FILE && !v->text) {   /* Browse... (sounds: the sound theme's folder first) */
        const char *home = getenv("HOME"), *hint = ui_notebook_field_hint(p->nb, key);
        const char *start = home && *home ? home : "/";
        if (hint && !strcmp(hint, "audio") && access("/usr/share/sounds/freedesktop/stereo", R_OK) == 0) start = "/usr/share/sounds/freedesktop/stereo";
        filepick_open(p->title, hint, start, props_file_chosen, p->id, key);
        return;
    }
    if (!strcmp(key, "title") && kind == UI_FIELD_TEXT) {
        /* Decided: a file's title is its name; the service renames it (or,
         * without permission, keeps it as a title). */
        svc_rename(p->target, v->text ? v->text : "", props_applied, arg);
        return;
    }
    cv val = { 0 };
    if (kind == UI_FIELD_TEXT || kind == UI_FIELD_ICON || kind == UI_FIELD_FILE || kind == UI_FIELD_COLOR) { val.k = CV_TEXT; val.s = v->text ? v->text : ""; val.len = strlen(val.s); }
    else if (kind == UI_FIELD_CHECKBOX) { val.k = CV_BOOL; val.b = v->checked; }
    else { val.k = CV_INT; val.i = v->number; }
    svc_settings_set(p->target, key, &val, p->changes, props_applied, arg);
}

/* Default (object-properties-design.md 2.2): the page's own values removed,
 * the class's or the type's shown again. */
static void props_default(void *ctx, const char *const *keys, size_t n)
{
    struct props *p = ctx;
    char names[1024] = "";
    for (size_t i = 0; i < n; i++)
        snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", i ? ";" : "", keys[i]);
    svc_reset(p->target, names, props_applied, (void *)(intptr_t)p->id);
}

/* Class...: the notebook of all objects of its class, or for a data file of
 * its type or its kind of type (a menu). */
static char class_menu_targets[3][160];

static void class_chosen(void *arg, int64_t id)
{
    (void)arg;
    if (id >= 0 && id < 3 && class_menu_targets[id][0]) open_properties(class_menu_targets[id]);
}

static void props_class(void *ctx, ui_surface *s, double x, double y, uint32_t serial)
{
    struct props *p = ctx;
    const char *mime = p->target[0] == '/' && strcmp(p->cls, "OsdFolder") && !p->makes ? mime_of_file(p->target) : NULL;
    if (mime && (!strncmp(mime, "inode/", 6) || !strchr(mime, '/'))) mime = NULL;
    snprintf(class_menu_targets[0], sizeof class_menu_targets[0], "class:%s", p->cls);
    if (!mime) { open_properties(class_menu_targets[0]); return; }
    char l0[128], l1[128], l2[128];
    snprintf(class_menu_targets[1], sizeof class_menu_targets[1], "type:%s", mime);
    snprintf(class_menu_targets[2], sizeof class_menu_targets[2], "type:%.*s/*", (int)(strchr(mime, '/') - mime), mime);
    snprintf(l0, sizeof l0, "All of its class (%s)", p->cls_title[0] ? p->cls_title : p->cls);
    snprintf(l1, sizeof l1, "All files of its type (%s)", mime);
    snprintf(l2, sizeof l2, "All %.*s files", (int)(strchr(mime, '/') - mime), mime);
    ui_menu_item items[3] = { { 1, l1, true, false, NULL, 0 }, { 2, l2, true, false, NULL, 0 }, { 0, l0, true, false, NULL, 0 } };
    ui_menu_popup(s, items, 3, x, y, serial, class_chosen, NULL);
}

bool props_is_open(const char *target)
{
    for (struct props *p = props_open; p; p = p->next)
        if (p->nb && !strcmp(p->target, target)) return true;
    return false;
}

/* A notebook opened or closed: its object's icon hatched or not (OS/2's
 * in-use emphasis), in every window. */
static void props_redraw(void)
{
    for (struct folder *f = folders; f; f = f->next)
        if (!f->closing && f->cont) ui_container_changed(f->cont, 0, SIZE_MAX);
}

static void props_closed(void *ctx)
{
    struct props *p = ctx;
    for (struct props **pp = &props_open; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    bool shown = p->nb != NULL;
    free(p);
    if (shown) props_redraw();
}

static void got_settings(void *arg, const cv *reply)
{
    struct props *p = props_by_id((long)(intptr_t)arg);
    if (!p) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res) {
        fprintf(stderr, "wps-shell: no settings for %s: %s\n", p->target, reply ? cv_text(reply, "message") : "no service");
        props_closed(p);
        return;
    }
    note_object(p, reply);
    const cv *o = cv_get(reply, "object");
    const char *t = o ? cv_text(o, "title") : NULL;
    snprintf(p->title, sizeof p->title, "%s - Properties", t && *t ? t : p->target);
    size_t n;
    ui_page_desc *pages = pages_from(res, &n);
    page_previews(p, pages, n);
    p->nb = ui_notebook_open(app, p->title, pages, n, props_set, props_closed, p);
    ui_notebook_on_default(p->nb, props_default);
    if (strncmp(p->target, "class:", 6) && strncmp(p->target, "type:", 5) && p->cls[0])
        ui_notebook_set_extra(p->nb, "Class...", props_class);
    if (p->want_page && p->want_page < n) ui_notebook_show_page(p->nb, p->want_page);
    pages_free(pages, n);
    props_redraw();
}

/* Open an object's notebook, or leave the open one (Wayland lets no program
 * raise its own window on its own). */
/* Its settings notebook; one already open comes to the front (false). */
bool open_properties(const char *target)
{
    for (struct props *p = props_open; p; p = p->next)
        if (!strcmp(p->target, target)) {
            if (p->nb) ui_window_activate(ui_notebook_surface(p->nb));
            return false;
        }
    struct props *p = calloc(1, sizeof *p);
    p->id = next_props_id++;
    snprintf(p->target, sizeof p->target, "%s", target);
    p->next = props_open;
    props_open = p;
    svc_settings(target, got_settings, (void *)(intptr_t)p->id);
    return true;
}
