/* wps-shell: The bench report; the settings objects (System, Sound, Mouse, schemes, fonts, LaunchPad, WarpCenter) applied. */
#define _GNU_SOURCE
#include "shell.h"

static void apply_scheme(void);

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

void report(struct folder *f)
{
    const ui_frame_stat *st;
    size_t n;
    ui_surface_stats(f->win, &st, &n);
    size_t from = f->bench_from + 1, m = n > from ? n - from : 0;
    double *d = malloc((m ? m : 1) * sizeof *d), *iv = malloc((m ? m : 1) * sizeof *iv), frac = 0;
    double *mv = malloc((m ? m : 1) * sizeof *mv);
    size_t fulls = 0;
    for (size_t i = 0; i < m; i++) {
        d[i] = st[from + i].draw_ms;
        mv[i] = st[from + i].move_ms;
        iv[i] = st[from + i].interval_ms;
        frac += st[from + i].drawn_fraction;
        fulls += st[from + i].full;
    }
    qsort(d, m, sizeof *d, cmp_d);
    qsort(iv, m, sizeof *iv, cmp_d);
    qsort(mv, m, sizeof *mv, cmp_d);
    double secs = (ui_now_ms() - f->bench_start) / 1000.0;
    size_t late = 0;
    double med = m ? iv[m / 2] : 0;
    for (size_t i = 0; i < m; i++) late += iv[i] > med * 1.5;
#define P(v, p) (m ? (v)[(size_t)((p) / 100.0 * (double)(m - 1) + 0.5)] : 0)
    printf("{\"folder\":\"%s\",\"total\":%zu,\"view\":\"%s\",\"scale\":%d,\"size\":[%.0f,%.0f],\"frames\":%zu,"
           "\"fps\":%.1f,\"draw_ms\":{\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f},"
           "\"interval_ms\":{\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f},\"late_frames\":%zu,"
           "\"full_redraws\":%zu,\"drawn_fraction_mean\":%.3f,\"move_ms\":{\"p50\":%.2f,\"p99\":%.2f},"
           "\"verified\":%zu,\"verify_failed\":%zu,\"verify_diff_px\":%zu}\n",
           f->path, f->m.total, ui_container_view(f->cont) == UI_VIEW_ICON ? "icon" : "details", ui_surface_scale(f->win),
           ui_surface_width(f->win), ui_surface_height(f->win), m, secs > 0 ? (double)m / secs : 0, P(d, 50), P(d, 99),
           m ? d[m - 1] : 0, P(iv, 50), P(iv, 99), m ? iv[m - 1] : 0, late, fulls, m ? frac / (double)m : 0, P(mv, 50), P(mv, 99), f->verified, f->verify_failed, f->verify_diff);
    fflush(stdout);
    free(d);
    free(iv);
    free(mv);
}

const ui_surface_ops win_ops = {
    .draw = win_draw, .pointer = win_pointer, .key = win_key, .resized = win_resized,
    .drag_motion = win_drag_motion, .drag_leave = win_drag_leave, .drop = win_drop,
    .focus = win_focus, .close = win_close, .frame = win_frame,
};

void got_title(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *t = res ? cv_text(res, "title") : NULL;
    if (!f || !t || !*t) return;
    snprintf(f->title, sizeof f->title, "%s", t);
    update_title(f);
}

/* ------------------------------------------------------- settings objects */

/* The System, Sound, Mouse and Scheme Palette objects (G3): read at start
 * and whenever one changes (their notebooks), applied at once. */
static int64_t setup_sub;

double next_w, next_h;   /* the next folder window's size (0: the default) */

bool restore_windows = true;   /* System: "Restore open windows at login" */

/* The LaunchPad's and the WarpCenter's "Show at start" (automatic, yes, no). */
static char lp_show[16] = "automatic", wc_show[16] = "automatic";

static char scheme_name[32] = "classic";

void lp_props(const char *addr) { open_properties(addr); }   /* (the LaunchPad's Properties) */

/* A settings object's address, by class ("" if not known yet). */
const char *setup_addr(const char *cls)
{
    for (size_t i = 0; i < NSETUP; i++) if (!strcmp(setup_cls[i], cls)) return setup_ids[i];
    return "";
}

static void setup_set(const char *cls, const char *key, const char *val)
{
    const char *a = setup_addr(cls);
    if (!*a) return;
    cv v = { .k = CV_TEXT, .s = (char *)val, .len = strlen(val) };
    svc_settings_set(a, key, &v, -1, ignore_reply, NULL);   /* (applied when the change comes back) */
}

/* The Font Palette's choices (fontpal.h), kept in the settings objects. */
void font_apply(const char *what, const char *spec)
{
    if (!strcmp(what, "samples")) setup_set("WPFontPalette", "fonts", spec);
    else if (!strcmp(what, "warpcenter")) setup_set("WPWarpCenter", "font", spec);
    else if (!strcmp(what, "menus")) setup_set("WPSystem", "font_menus", spec);
    else if (!strcmp(what, "titles")) setup_set("WPSystem", "font_titles", spec);
    else if (!strcmp(what, "folders")) setup_set("WPSystem", "font_folders", spec);
}

void font_dropped_wc(const char *spec) { font_apply("warpcenter", spec); }

static void got_setup_sub(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (res && res->k == CV_INT) setup_sub = res->i;
}

static void apply_setup_object(const char *cls, const cv *settings)
{
    if (!settings || settings->k != CV_MAP) return;
    if (!strcmp(cls, "WPSound")) events_setup_begin();
    for (size_t i = 0; i < settings->n; i++) {
        const cv *k = &settings->items[2 * i], *v = &settings->items[2 * i + 1];
        if (k->k != CV_TEXT || v->k != CV_TEXT) continue;
        const char *key = k->s, *val = v->s;
        gnome_wps_value(cls, key, val);
        if (!strcmp(cls, "WPSound")) events_setup_set(key, val);
        else if (!strcmp(cls, "WPMouse")) {
            ui_mouse_set(key, val);
            char kv[160];   /* the compositor's too (WPS session): its drag pointer follows them */
            snprintf(kv, sizeof kv, "%s=%s", key, val);
            winlist_ext("mouse", kv);
        }
        else if (!strcmp(cls, "WPSchemePalette") && !strcmp(key, "scheme")) snprintf(scheme_name, sizeof scheme_name, "%s", val);
        else if (!strcmp(cls, "WPLaunchPad") && !strcmp(key, "show_at_start")) snprintf(lp_show, sizeof lp_show, "%s", val);
        else if (!strcmp(cls, "WPLaunchPad")) launchpad_setting(key, val);
        else if (!strcmp(cls, "WPWarpCenter")) {
            if (!strcmp(key, "show_at_start")) snprintf(wc_show, sizeof wc_show, "%s", val);
            else warpcenter_setting(key, val);
        }
        else if (!strcmp(cls, "WPMessages")) messages_setting(key, val);
        else if (!strcmp(cls, "WPFontPalette") && !strcmp(key, "fonts")) fontpal_set_samples(val);
        else if (!strcmp(cls, "WPSystem")) {
            if (ui_scheme_set_option(key, val)) {   /* corners, buttons, border, shadows, translucency, title layout */
                char kv[160];   /* the compositor's frames too (WPS session) */
                snprintf(kv, sizeof kv, "%s=%s", key, val);
                winlist_ext("frame", kv);
            }
            else if (!strcmp(key, "confirm_delete")) confirm_delete = !strcmp(val, "1");
            else if (!strcmp(key, "confirm_folder_delete")) confirm_folder_delete = !strcmp(val, "1");
            else if (!strcmp(key, "restore_windows")) restore_windows = !strcmp(val, "1");
            /* (font_folders, show_ext, show_hidden: all folders' - each window
             * reads its own, resolved; defaults_changed) */
            else if (!strcmp(key, "font_folders")) {
                snprintf(folders_font, sizeof folders_font, "%s", val);   /* (shown by the self-tests) */
            } else if (!strcmp(key, "font_menus")) {
                ui_menu_set_font(ui_font_named(val));
            } else if (!strcmp(key, "font_titles")) {
                snprintf(titles_font, sizeof titles_font, "%s", val);
                ui_frame_set_title_font(ui_font_named(val));
                char kv[200];   /* the compositor's frames too */
                snprintf(kv, sizeof kv, "title_font=%s", val);
                winlist_ext("frame", kv);
                for (struct folder *x = folders; x; x = x->next) if (!x->closing) ui_frame_damage(x->win);
            }
        }
    }
}

static void show_warpcenter(void)
{
    if (warpcenter_is_open()) return;   /* (the bar: always in view) */
    for (struct folder *f = folders; f; f = f->next)
        if (f->desktop && !f->closing) {
            warpcenter_open(app, f->path, getenv("WPS_WARPCENTER_TOP") != NULL || warpcenter_top(), launchpad_open_folder);
            warpcenter_messages(messages_unread(), messages_open);
            return;
        }
}

void templates_dir(char *out, size_t n)
{
    const char *t = getenv("WPS_TEMPLATES_DIR");
    if (t && *t) snprintf(out, n, "%s", t);
    else user_dir("TEMPLATES", "Templates", out, n);
}

/* Opened, or brought forward: never two (only the desktop's process
 * has one). */
static void show_launchpad(void)
{
    if (launchpad_is_open()) { launchpad_raise(); return; }
    bool desktop = false;
    for (struct folder *f = folders; f && !desktop; f = f->next) desktop = f->desktop && !f->closing;
    if (!desktop) return;
    char tdir[PATH_MAX];
    templates_dir(tdir, sizeof tdir);
    launchpad_open(app, launchpad_open_folder, tdir, setup_folder[0] ? setup_folder : NULL);
}

/* Restore Desktop: asked first; what was put back, said after. */
static void defaults_restored(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL, *got = res ? cv_get(res, "restored") : NULL;
    static const char *const ok[] = { "OK" };
    if (!res) {
        char m[512];
        snprintf(m, sizeof m, "The default objects could not be restored: %s",
                 reply && cv_text(reply, "message") ? cv_text(reply, "message") : "no answer from wpsd");
        ui_dialog_open(app, "Restore Desktop", m, ok, 1, 0, NULL, NULL);
        return;
    }
    char m[2048];
    size_t n = got && got->k == CV_ARRAY ? got->n : 0;
    if (!n) snprintf(m, sizeof m, "All the default objects are in place: nothing was changed.");
    else {
        snprintf(m, sizeof m, "Put back: ");
        for (size_t i = 0; i < n; i++) {
            const cv *t = &got->items[i];
            if (t->k != CV_TEXT) continue;
            size_t l = strlen(m);
            snprintf(m + l, sizeof m - l, "%s%s", i ? ", " : "", t->s);
        }
        strncat(m, ".", sizeof m - strlen(m) - 1);
    }
    ui_dialog_open(app, "Restore Desktop", m, ok, 1, 0, NULL, NULL);
}

static void restore_chosen(void *ctx, int choice)
{
    (void)ctx;
    if (choice != 0) return;
    const char *desk = NULL;
    for (struct folder *f = folders; f && !desk; f = f->next) if (f->desktop && !f->closing) desk = f->path;
    svc_restore_defaults(desk, defaults_restored, NULL);
}

static void restore_defaults_ask(void)
{
    static const char *const b[] = { "Restore", "Cancel" };
    ui_dialog_open(app, "Restore Desktop",
                   "Put the default objects back where they belong? System, Programs and the Trash Can on the desktop; "
                   "Templates, Startup, the Minimized Window Viewer and the settings objects in System. Ones moved "
                   "elsewhere or into the Trash Can are moved back; ones gone are made again. Nothing else changes.",
                   b, 2, 1, restore_chosen, NULL);
}

/* An open's reply that names a view (svc_reply_view). */
bool open_view(const char *v)
{
    if (!strcmp(v, "messages")) messages_open();
    else if (!strcmp(v, "minimized-windows")) winlist_open(true);   /* the Minimized Window Viewer */
    else if (!strcmp(v, "launchpad")) show_launchpad();
    else if (!strcmp(v, "fontpalette")) fontpal_open();
    else if (!strcmp(v, "warpcenter")) show_warpcenter();
    else if (!strcmp(v, "restore-defaults")) restore_defaults_ask();
    else return false;
    return true;
}

void got_setup(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL, *objs = res ? cv_get(res, "objects") : NULL;
    if (!objs || objs->k != CV_MAP) return;
    const char *ids[NSETUP];
    size_t n = 0;
    for (size_t i = 0; i < objs->n && i < NSETUP; i++) {
        const cv *cls = &objs->items[2 * i], *o = &objs->items[2 * i + 1];
        if (cls->k != CV_TEXT) continue;
        apply_setup_object(cls->s, cv_get(o, "settings"));
        const char *a = cv_text(o, "address");
        if (a) {
            snprintf(setup_ids[n], sizeof setup_ids[n], "%s", a);
            snprintf(setup_cls[n], sizeof setup_cls[n], "%s", cls->s);
            ids[n] = setup_ids[n];
            n++;
        }
    }
    apply_scheme();
    gnome_wps_read();   /* GNOME follows (or, the first time, leads) */
    for (struct folder *f = folders; f; f = f->next) if (!f->closing) ui_surface_damage_all(f->win);
    if (!setup_sub && n) svc_subscribe_objects(ids, n, got_setup_sub, NULL);   /* changes made in their notebooks */
    const char *folder = cv_text(res, "folder");
    /* The desktop's LaunchPad (G4), once System Setup is known (its first
     * buttons include it). WPS_NO_LAUNCHPAD: none (tests). */
    bool desktop = false;
    for (struct folder *f = folders; f && !desktop; f = f->next) desktop = f->desktop && !f->closing;
    /* In the WPS session the WarpCenter, and not the LaunchPad (the
     * owner's decision for 0.2; the LaunchPad stays in System). Inside
     * GNOME, which has its own bar, the LaunchPad as before. */
    /* The LaunchPad and WarpCenter objects' "Show at start" can say
     * otherwise (only at start: closing them is not offered). */
    const char *wcs = getenv("WPS_WARPCENTER");
    bool wpsession = getenv("WPS_SESSION") != NULL;
    bool want_wc = wcs ? !strcmp(wcs, "1") : wpsession && strcmp(wc_show, "no");
    bool want_lp = getenv("WPS_LAUNCHPAD") ? true : !strcmp(lp_show, "automatic") ? !wpsession : !strcmp(lp_show, "yes");
    if (wcs && !strcmp(wcs, "1")) want_lp = want_lp && (getenv("WPS_LAUNCHPAD") || !strcmp(lp_show, "yes"));
    static bool started;   /* (settings read again later change neither) */
    bool first = !started && desktop;
    if (desktop) started = true;
    if (folder) snprintf(setup_folder, sizeof setup_folder, "%s", folder);
    if (first && want_wc) show_warpcenter();
    if (first) windows_at_login();
    if (first && !getenv("WPS_NO_LAUNCHPAD") && want_lp) show_launchpad();
}

/* The Scheme Palette's scheme: colours for every window; the desktop's
 * picture is GNOME's either way. */
static void apply_scheme(void)
{
    if (!ui_scheme_use(scheme_name)) ui_scheme_use("classic");
    /* in the WPS session the compositor draws other programs' frames: the
     * same scheme and font (under GNOME the extension ignores these) */
    winlist_ext("scheme", ui_scheme_now.name);
    char font[300];
    snprintf(font, sizeof font, "%g %s", ui_scheme_now.font_px, ui_text_family());
    winlist_ext("font", font);
    for (struct folder *f = folders; f; f = f->next) {
        if (f->closing) continue;
        layout(f);   /* border and title sizes may have changed */
        ui_container_layout_changed(f->cont);
        ui_surface_damage_all(f->win);
    }
}

/* GNOME changed a shared setting: the settings object takes it. */
void gnome_set_wps(const char *cls, const char *key, const char *value)
{
    for (int i = 0; i < NSETUP; i++)
        if (!strcmp(setup_cls[i], cls)) {
            cv v = { .k = CV_TEXT, .s = (char *)value, .len = strlen(value) };
            svc_settings_set(setup_ids[i], key, &v, -1, ignore_reply, NULL);
        }
}

/* GNOME's interface font changed: every window lays out again. */
void gnome_font(void)
{
    for (struct folder *f = folders; f; f = f->next) {
        if (f->closing) continue;
        layout(f);
        ui_container_layout_changed(f->cont);
        ui_surface_damage_all(f->win);
    }
}

/* Is this event about one of the settings objects? Then read them again. */
bool setup_event(const cv *ev)
{
    int64_t sub;
    if (!setup_sub || !cv_int(ev, "sub", &sub) || sub != setup_sub) return false;
    svc_setup(got_setup, NULL);
    return true;
}

/* What the toolkit reports (minimize, maximize, restore, menus). */
void toolkit_action(const char *name, ui_surface *s, ui_rect at)
{
    struct folder *f = NULL;
    for (struct folder *o = folders; o && !f; o = o->next) if (o->win == s) f = o;
    events_raise(name, f ? f->path : "", !strncmp(name, "window.", 7) ? "folder" : "", at);
}

struct folder *open_folder(const char *path, ui_view view) { return open_window(path, view, false); }

void launchpad_open_folder(const char *path) { open_folder(path, UI_VIEW_ICON); }

void desk_backdrop(void *ctx, cairo_t *cr, ui_rect clip) { (void)ctx; wallpaper_draw(cr, clip); }

void desk_wallpaper_changed(void *ctx)
{
    struct folder *f = from_arg(ctx);
    if (f) TIMING(f, "background: %s", wallpaper_showing());
    if (f) ui_container_layout_changed(f->cont);   /* redraws the whole list area */
}
