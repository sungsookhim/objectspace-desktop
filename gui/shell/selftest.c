/* osd-shell: The self-tests (OSD_SHELL_SELFTEST=MODE): scenarios run in the window's frame callback. */
#define _GNU_SOURCE
#include "shell.h"

#include <spawn.h>
#include <sys/wait.h>

static void selftest_record(const osd_event *e, void *ctx)
{
    char *seen = ctx;
    if (strlen(seen) + strlen(e->name) + 2 < 1024) { strcat(seen, " "); strcat(seen, e->name); }
}

static void selftest(ui_surface *s, double now)
{
    static double t0;
    static int step;
    if (!t0) t0 = now;
    double t = now - t0;
    ui_menu_item items[] = { { 1, "One", true, false, NULL, 0 }, { 2, "Two", true, false, NULL, 0 } };
    if (step == 0 && t > 200) { ui_frame_system_menu(s, 0); ui_frame_system_menu(s, 0); step++; }
    else if (step == 1 && t > 500) { ui_frame_system_menu(s, 0); step++; }           /* after a dismissal */
    else if (step == 2 && t > 520) { ui_frame_system_menu(s, 0); step++; }           /* click again: closes */
    else if (step == 3 && t > 800) { ui_menu_popup(s, items, 2, 100, 120, 0, selftest_chosen, NULL); step++; }
    else if (step == 4 && t > 820) { ui_menu_popup(s, items, 2, 300, 200, 0, selftest_chosen, NULL); ui_frame_system_menu(s, 0); step++; }
    else if (step == 5 && t > 1000) {
        /* The menu opened last is open; a press on the window must close it. */
        ui_menu_item one[] = { { 1, "One", true, false, NULL, 0 } };
        ui_menu_popup(s, one, 1, 50, 50, 0, selftest_chosen, NULL);
        bool open_before = ui_menu_is_open();
        extern bool ui_menu_press_outside(ui_surface *);   /* what a press on the window does */
        bool used = ui_menu_press_outside(s);
        fprintf(stderr, "selftest: press outside: open before %d, used %d, open after %d\n", open_before, used, ui_menu_is_open());
        step++;
    }
    else if (step == 6 && t > 1500) { fprintf(stderr, "selftest: done\n"); ui_app_quit(app); step++; }
}

/* OSD_SHELL_SELFTEST=live: files created and deleted in the folder appear
 * and disappear. =close: closing one of two windows (as its close button
 * does) leaves the other and the process running. */
static void selftest_live(struct folder *f, double now)
{
    static double t0;
    static int step;
    static size_t base;
    char p[PATH_MAX + 32];
    if (!t0) { if (!f->m.complete) return; t0 = now; base = f->m.total; }
    double t = now - t0;
    if (step == 0) {
        for (int i = 0; i < 3; i++) {
            snprintf(p, sizeof p, "%s/selftest-%d.txt", f->path, i);
            FILE *x = fopen(p, "w");
            if (x) fclose(x);
        }
        step++;
    } else if (step == 1 && t > 1500) {
        fprintf(stderr, "selftest: after creating 3 files: %zu objects (was %zu)\n", f->m.total, base);
        for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/selftest-%d.txt", f->path, i); unlink(p); }
        step++;
    } else if (step == 2 && t > 3000) {
        fprintf(stderr, "selftest: after deleting them: %zu objects\n", f->m.total);
        ui_app_quit(app);
        step++;
    }
}

static void selftest_close(ui_surface *s, double now)
{
    static double t0;
    static int step;
    if (!t0) t0 = now;
    if (step == 0 && now - t0 > 300) {
        int n = 0;
        for (struct folder *x = folders; x; x = x->next) n++;
        fprintf(stderr, "selftest: %d windows; closing one\n", n);
        win_close(s);
        step++;
    }
}

static void selftest_after_close(void *arg)
{
    (void)arg;
    int n = 0;
    for (struct folder *x = folders; x; x = x->next) n++;
    fprintf(stderr, "selftest: %d window(s) left, process running\n", n);
    ui_app_quit(app);
}

/* OSD_SHELL_SELFTEST=fields: what a notebook's fields set, printed. */
static ui_notebook *selftest_nb;

static void selftest_file_chosen(long owner, const char *key, const char *path)
{
    (void)owner;
    const char *base = strrchr(path, '/');
    fprintf(stderr, "selftest: fields: picked %s\n", base ? base + 1 : path);
    if (selftest_nb) ui_notebook_set_value(selftest_nb, key, path);
}

static void selftest_field_set(void *ctx, const char *key, ui_field_kind kind, const ui_value *v)
{
    (void)ctx;
    if (kind == UI_FIELD_FILE && !v->text) {
        fprintf(stderr, "selftest: fields: Browse...\n");
        filepick_open("Fields", ui_notebook_field_hint(selftest_nb, key), "/", selftest_file_chosen, 0, key);
        return;
    }
    const char *base = v->text ? strrchr(v->text, '/') : NULL;
    fprintf(stderr, "selftest: fields: set %s = %s\n", key, base ? base + 1 : v->text ? v->text : "");
}

/* OSD_SHELL_SELFTEST=layout: the icons' cells, checked. */
static size_t selftest_index(struct folder *f, const char *name)
{
    for (size_t i = 0; i < f->m.total; i++)
        if (model_name(&f->m, i) && !strcmp(model_name(&f->m, i), name)) return i;
    return SIZE_MAX;
}

static void selftest_cells(struct folder *f, const char *when)
{
    size_t n = f->m.total, over = 0;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            double xi, yi, xj, yj, hi = layout_item_h(f->lay, i), hj = layout_item_h(f->lay, j);
            layout_pos(f->lay, i, &xi, &yi);
            layout_pos(f->lay, j, &xj, &yj);
            double wi = layout_item_w(f->lay, i), wj = layout_item_w(f->lay, j);
            if (xi < xj + wj && xj < xi + wi && yi < yj + hj && yj < yi + hi) over++;
        }
    /* the second row: below the first row's tallest title */
    double top1 = 1e9, tall = 0, row2 = 1e9;
    for (size_t i = 0; i < n; i++) { double x, y; layout_pos(f->lay, i, &x, &y); if (y < top1) top1 = y; }
    for (size_t i = 0; i < n; i++) {
        double x, y;
        layout_pos(f->lay, i, &x, &y);
        if (y == top1 && layout_item_h(f->lay, i) > tall) tall = layout_item_h(f->lay, i);
        if (y > top1 && y < row2) row2 = y;
    }
    fprintf(stderr, "selftest: layout: %s: %zu overlaps; tallest of row 1 %.0f, row 2 at %.0f\n", when, over, tall, row2 - top1);
}

/* (the colors self-test) every folder's background, after a Shift drop */
static void colors_class_read(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    for (size_t i = 0; res && res->k == CV_ARRAY && i < res->n; i++) {
        const char *k = cv_text(&res->items[i], "name"), *v = cv_text(&res->items[i], "value");
        if (k && v && !strcmp(k, "bg_color")) fprintf(stderr, "selftest: colors: with Shift: every folder's %s\n", v);
    }
}

static void selftest_lock_page(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *v = "missing";
    for (size_t i = 0; res && res->k == CV_ARRAY && i < res->n; i++) {
        const cv *fs = cv_get(&res->items[i], "fields");
        for (size_t k = 0; fs && fs->k == CV_ARRAY && k < fs->n; k++) {
            const char *key = cv_text(&fs->items[k], "key");
            const cv *val = cv_get(&fs->items[k], "value");
            if (key && !strcmp(key, "lock_pos") && val && val->k == CV_BOOL)
                v = !strcmp(cv_text(&res->items[i], "title") ? cv_text(&res->items[i], "title") : "", "General") ? (val->b ? "on" : "off") : "elsewhere";
        }
    }
    fprintf(stderr, "selftest: layout: b.txt's General page: Lock position %s\n", v);
}

/* OSD_SHELL_SELFTEST=openwith: a data file's Open cascade, printed. */
static void selftest_cascade(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    char out[1024] = "";
    for (size_t i = 0; res && res->k == CV_ARRAY && i < res->n; i++) {
        int64_t id = 0;
        cv_int(&res->items[i], "id", &id);
        const cv *sub = cv_get(&res->items[i], "items");
        if (id != OSD_MI_OPEN) continue;
        for (size_t k = 0; sub && sub->k == CV_ARRAY && k < sub->n; k++) {
            const cv *ck = cv_get(&sub->items[k], "checked");
            snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s%s", k ? ", " : "", cv_text(&sub->items[k], "label"),
                     ck && ck->k == CV_BOOL && ck->b ? " (default)" : "");
        }
    }
    fprintf(stderr, "selftest: openwith: Open: %s\n", out);
}

/* OSD_SHELL_SELFTEST=templates: New > printed, its Folder chosen. */
static void selftest_new_menu(struct menu_ctx *m, const ui_menu_item *items, size_t n)
{
    char out[512] = "";
    int64_t folder_id = -1;
    for (size_t i = 0; i < n; i++) {
        if (items[i].id != OSD_MI_NEW_MENU) continue;
        for (size_t k = 0; k < items[i].nitems; k++) {
            snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s", k ? ", " : "", items[i].items[k].label);
            if (!strcmp(items[i].items[k].label, "Folder")) folder_id = items[i].items[k].id;
        }
    }
    fprintf(stderr, "selftest: templates: New: %s\n", out);
    menu_hook = NULL;
    menu_chosen(m, folder_id);
}

static bool selftest_pos(struct folder *f, const char *name, double *x, double *y)
{
    for (size_t i = 0; i < f->m.total; i++)
        if (model_name(&f->m, i) && !strcmp(model_name(&f->m, i), name)) { layout_pos(f->lay, i, x, y); return true; }
    return false;
}

void win_frame(ui_surface *s, double now)
{
    struct folder *f = ui_surface_data(s);
    const char *st = getenv("OSD_SHELL_SELFTEST");
    if (st && !strcmp(st, "ext")) {
        /* Show file extensions off (0.3 step 8): titles without them, and
         * renaming keeps the file's. <D>: report.odt archive.tar.gz notes dir.d/ */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char out[300] = "";
        if (step == 0) {
            f->show_ext = false;
            for (size_t i = 0; i < f->m.total; i++) {
                ui_item it;
                src_item(f, i, &it);
                snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s", i ? ", " : "", it.title);
            }
            fprintf(stderr, "selftest: ext: titles: %s\n", out);
            for (size_t i = 0; i < f->m.total; i++)
                if (!strcmp(model_name(&f->m, i), "report.odt")) src_rename(f, i, "summary");
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            struct stat sst;
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/summary.odt", f->path);
            fprintf(stderr, "selftest: ext: renamed \"summary\": summary.odt %d\n", stat(p, &sst) == 0);
            for (size_t i = 0; i < f->m.total; i++)
                if (!strcmp(model_name(&f->m, i), "summary.odt")) src_rename(f, i, "summary.txt");
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            struct stat sst;
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/summary.txt.odt", f->path);   /* (as with hidden extensions elsewhere: its type stays) */
            fprintf(stderr, "selftest: ext: renamed \"summary.txt\": summary.txt.odt %d\n", stat(p, &sst) == 0);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "fonts")) {
        /* The Font Palette (0.3 step 9): a font dropped on this folder
         * (its titles in it, kept), with Shift (every folder's), on its
         * title bar; Edit font... <D> has a.txt and a long-named file. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete || !setup_addr("OsdSystem")[0]) return; t0 = now; }
        double t = now - t0;
        if (step == 0) {
            fontpal_open();
            fprintf(stderr, "selftest: fonts: palette open %d, %zu samples, first %s\n", fontpal_is_open(), fontpal_count(),
                    fontpal_sample(0));
            const char *d = "14.Serif\n";
            win_drop(s, 300, 300, MIME_FONT, d, strlen(d), UI_DND_COPY, 0, 0);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            const char *shot = getenv("OSD_SHELL_SHOT");
            if (shot) fontpal_snapshot(shot);
            fprintf(stderr, "selftest: fonts: dropped on the folder: its font %s, %.0f px, a tall title's cell %.0f\n", f->icon_font,
                    ui_font_px(f->font), layout_item_h(f->lay, 0) > layout_item_h(f->lay, 1) ? layout_item_h(f->lay, 0) : layout_item_h(f->lay, 1));
            const char *d = "9.Sans Bold";
            win_drop(s, 300, 300, MIME_FONT, d, strlen(d), UI_DND_COPY, 0, UI_MOD_SHIFT);
            const char *d2 = "11.Serif Italic";
            win_drop(s, 200, 12, MIME_FONT, d2, strlen(d2), UI_DND_COPY, 0, 0);              /* its title bar */
            const char *d3 = "12.Monospace";
            win_drop(s, 200, 12, MIME_FONT, d3, strlen(d3), UI_DND_COPY, 0, UI_MOD_SHIFT);   /* every title bar */
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            fprintf(stderr, "selftest: fonts: with Shift: every folder's %s; its own still %s; its title bar's %s; with Shift every title bar's %s\n",
                    folders_font, f->icon_font, f->title_font, titles_font);
            fontpal_edit(0);
            fprintf(stderr, "selftest: fonts: Edit font: open %d, %zu families\n", fontpal_edit_is_open(), fontpal_families());
            fontpal_edit_choose("Serif", 16, "Bold");
            step++;
            t0 = now;
        } else if (step == 3 && t > 1500) {
            fprintf(stderr, "selftest: fonts: the first sample now %s (kept: %s)\n", fontpal_sample(0), setup_addr("OsdFontPalette")[0] ? "yes" : "no");
            f->icon_font[0] = 0;
            bg_read(f);   /* read back from its settings */
            step++;
            t0 = now;
        } else if (step == 4 && t > 1500) {
            fprintf(stderr, "selftest: fonts: read back: %s\n", f->icon_font);
            /* (0.4 step 8) Edit font's style list: a family's own styles */
            fontpal_edit(0);
            static const char *const fams[] = { "Noto Sans", "DejaVu Sans", "Cantarell", "Liberation Sans" };
            char st[32][64];
            size_t n = 0;
            const char *fam = NULL, *style = NULL;
            for (size_t k = 0; k < 4 && !style; k++) {
                if (!fontpal_edit_family(fams[k])) continue;
                n = fontpal_edit_styles(st, 32);
                for (size_t j = 0; j < n && !style; j++)
                    if (strcasecmp(st[j], "Regular") && strcasecmp(st[j], "Bold") && strcasecmp(st[j], "Italic") && strcasecmp(st[j], "Bold Italic"))
                        fam = fams[k], style = st[j];
            }
            fprintf(stderr, "selftest: fonts: Edit font: %s has %zu styles; chose %s\n", fam ? fam : "(none)", n, style ? style : "(none)");
            char fs[64], ss[64];
            snprintf(fs, sizeof fs, "%s", fam ? fam : "Sans");
            snprintf(ss, sizeof ss, "%s", style ? style : "Bold");
            fontpal_edit_choose(fs, 12, ss);
            step++;
            t0 = now;
        } else if (step == 5 && t > 1500) {
            ui_font sf = ui_font_named(fontpal_sample(0));
            fprintf(stderr, "selftest: fonts: the first sample now %s (loaded %d, %.0f px)\n", fontpal_sample(0), sf != UI_FONT_REGULAR,
                    ui_font_px(sf));
            open_properties(f->path);
            step++;
            t0 = now;
        } else if (step == 6 && t > 1500) {
            /* a font on its notebook: on the icon font's field, that field;
             * elsewhere (here on a colour field), the Dialogs font */
            ui_notebook *nb = props_open && !strcmp(props_open->target, f->path) ? props_open->nb : NULL;
            double x = 0, y = 0;
            bool spot = nb && ui_notebook_field_spot(nb, "icon_font", &x, &y);
            fprintf(stderr, "selftest: fonts: its notebook: the icon font's field %d, dropped on it: %u\n", spot,
                    spot ? ui_surface_test_drop(ui_notebook_surface(nb), x, y, MIME_FONT, "10.Serif", 0) : 0);
            spot = nb && ui_notebook_field_spot(nb, "bg_color", &x, &y);
            fprintf(stderr, "selftest: fonts: dropped elsewhere on it: %u\n",
                    spot ? ui_surface_test_drop(ui_notebook_surface(nb), x, y, MIME_FONT, "14.Sans", 0) : 0);
            step++;
            t0 = now;
        } else if (step == 7 && t > 2000) {
            ui_notebook *nb = props_open && !strcmp(props_open->target, f->path) ? props_open->nb : NULL;
            const char *shot = getenv("OSD_SHELL_SHOT");
            if (shot && nb) {
                char p[PATH_MAX];
                snprintf(p, sizeof p, "%s-notebook.png", shot);
                ui_surface_snapshot(ui_notebook_surface(nb), p);
            }
            fprintf(stderr, "selftest: fonts: its icon font %s; the scheme's Dialogs font %s (%.0f px, rows %.0f px taller)\n",
                    f->icon_font, ui_scheme_now.font_dialogs, ui_font_px(UI_FONT_DIALOG), ui_dialog_grow());
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "colors")) {
        /* The Colour Palette (0.4 step 7): a colour dropped on this
         * folder (its background), with Ctrl (its icons' titles), with
         * Shift (every folder's), on its title bar (its own; with Shift
         * the scheme's active title bar); on its notebook's colour field; Edit colour...
         * <D> has a.txt. */
        static double t0;
        static int step;
        static ui_notebook *nb;
        if (!t0) { if (!f->m.complete || !setup_addr("OsdColorPalette")[0]) return; t0 = now; }
        double t = now - t0;
        const char *shot = getenv("OSD_SHELL_SHOT");
        if (step == 0) {
            colorpal_open();
            fprintf(stderr, "selftest: colors: palette open %d, %zu samples, first %s\n", colorpal_is_open(), colorpal_count(),
                    colorpal_sample(0));
            fprintf(stderr, "selftest: colors: on the background: %u\n", ui_surface_test_drop(s, 300, 300, UI_MIME_COLOR, "#336699", 0));
            fprintf(stderr, "selftest: colors: with Ctrl: %u\n", ui_surface_test_drop(s, 300, 300, UI_MIME_COLOR, "#ffff00", UI_MOD_CTRL));
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            if (shot) {
                char p[PATH_MAX];
                snprintf(p, sizeof p, "%s-palette.png", shot);
                colorpal_snapshot(p);
                snprintf(p, sizeof p, "%s-folder.png", shot);
                ui_surface_snapshot(s, p);
            }
            fprintf(stderr, "selftest: colors: its background %s, its titles %s\n", f->bg_color, f->title_color);
            ui_surface_test_drop(s, 300, 300, UI_MIME_COLOR, "#800000", UI_MOD_SHIFT);   /* every folder's */
            fprintf(stderr, "selftest: colors: on its title bar: %u\n", ui_surface_test_drop(s, 200, 12, UI_MIME_COLOR, "#800080", 0));
            fprintf(stderr, "selftest: colors: with Shift: %u\n", ui_surface_test_drop(s, 200, 12, UI_MIME_COLOR, "#008000", UI_MOD_SHIFT));
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            const ui_color c = ui_scheme_now.title_active;
            fprintf(stderr, "selftest: colors: its own still %s; its title bar %s; with Shift the scheme's active title bar #%02x%02x%02x\n",
                    f->bg_color, f->title_bar_color,
                    (int)lround(c.r * 255), (int)lround(c.g * 255), (int)lround(c.b * 255));
            open_properties(f->path);
            svc_props("class:OsdFolder", colors_class_read, NULL);
            step++;
            t0 = now;
        } else if (step == 3 && t > 1500) {
            nb = props_open && !strcmp(props_open->target, f->path) ? props_open->nb : NULL;
            double x = 0, y = 0;
            bool spot = nb && ui_notebook_field_spot(nb, "bg_color", &x, &y);
            fprintf(stderr, "selftest: colors: its notebook: the colour field %d\n", spot);
            if (spot) fprintf(stderr, "selftest: colors: dropped on the field: %u\n",
                              ui_surface_test_drop(ui_notebook_surface(nb), x, y, UI_MIME_COLOR, "#c0ffc0", 0));
            step++;
            t0 = now;
        } else if (step == 4 && t > 1500) {
            if (shot && nb) {
                char p[PATH_MAX];
                snprintf(p, sizeof p, "%s-background.png", shot);
                ui_surface_snapshot(ui_notebook_surface(nb), p);
            }
            fprintf(stderr, "selftest: colors: its background now %s\n", f->bg_color);
            colorpal_edit(0);
            fprintf(stderr, "selftest: colors: Edit colour: open %d, shows %s\n", colorpal_edit_is_open(), colorpal_edit_hex());
            step++;
            t0 = now;
        } else if (step == 5 && t > 1000) {
            colorpal_edit_pick(0.5, 0);   /* the hue across the middle (cyan), full saturation */
            colorpal_edit_bright(1);
            fprintf(stderr, "selftest: colors: picked: %s\n", colorpal_edit_hex());
            step++;
            t0 = now;
        } else if (step == 6 && t > 1000) {
            if (shot) {
                char p[PATH_MAX];
                snprintf(p, sizeof p, "%s-edit.png", shot);
                colorpal_edit_snapshot(p);
            }
            colorpal_edit_type("#123456");
            colorpal_edit_ok();
            step++;
            t0 = now;
        } else if (step == 7 && t > 1500) {
            fprintf(stderr, "selftest: colors: the first sample now %s\n", colorpal_sample(0));
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "soundpage")) {
        /* The Sound object's page (0.3 step 9): each event a sound field
         * (Browse..., None, Play); Play plays it; a file set is used. */
        static double t0;
        static int step;
        static unsigned played0;
        if (!t0) { if (!f->m.complete || !setup_addr("OsdSound")[0]) return; t0 = now; }
        double t = now - t0;
        struct props *p = NULL;
        for (struct props *x = props_open; x; x = x->next) if (!strcmp(x->target, setup_addr("OsdSound"))) p = x;
        if (step == 0) {
            open_properties(setup_addr("OsdSound"));
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            fprintf(stderr, "selftest: soundpage: notebook %d, window.open's hint %s\n", p && p->nb,
                    p && p->nb ? ui_notebook_field_hint(p->nb, "window.open") : "?");
            played0 = sound_played();
            char wav[PATH_MAX];   /* (a WAV: read with or without libsndfile) */
            snprintf(wav, sizeof wav, "%s/beep.wav", f->path);
            ui_value v = { .text = wav, .checked = true };
            if (p) props_set(p, "job.done", UI_FIELD_FILE, &v);
            fprintf(stderr, "selftest: soundpage: Play: PipeWire %s (formats %s), started %u\n", sound_ready() ? "connected" : "not there",
                    sound_formats(), sound_played() - played0);
            cv sv = { .k = CV_TEXT, .s = "/usr/share/sounds/freedesktop/stereo/bell.oga" };
            sv.len = strlen(sv.s);
            svc_settings_set(setup_addr("OsdSound"), "window.open", &sv, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            const char *w = events_sound_for("window.open");
            fprintf(stderr, "selftest: soundpage: a file set: window.open plays %s; Play started %u by now\n", w ? strrchr(w, '/') + 1 : "nothing",
                    sound_played() - played0);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "templates")) {
        /* Templates (0.3 step 8): New > at a spot; Create another into a
         * folder chosen in the picker; a template dragged makes an object,
         * Shift+dragged moves. <D> has a.txt (made a template here) and sub/. */
        static double t0, ax, ay;
        static int step;
        if (!t0) { if (!f->m.complete || !f->arranged_by_name) return; t0 = now; }
        double t = now - t0;
        char p[PATH_MAX];
        if (step == 0) {
            menu_hook = selftest_new_menu;
            src_menu(f, SIZE_MAX, 420, 330, 0);
            step++;
            t0 = now;
        } else if (step == 1 && t > 2000) {
            double x, y, lx, ly;
            ui_container_to_list(f->cont, 420, 330, &lx, &ly);
            bool made = selftest_pos(f, "Folder", &x, &y);
            fprintf(stderr, "selftest: templates: New > Folder: made %d, at the click %d\n", made, made && fabs(x - lx) < 2 && fabs(y - ly) < 2);
            size_t a = SIZE_MAX;
            for (size_t i = 0; i < f->m.total; i++) if (model_name(&f->m, i) && !strcmp(model_name(&f->m, i), "a.txt")) a = i;
            create_another(f, a);
            ui_dialog_set_text(another_now->dlg, "made.txt");
            ui_key_event tab = { .sym = XKB_KEY_Tab }, enter = { .sym = XKB_KEY_Return };
            ui_dialog_key(another_now->dlg, &tab);
            ui_dialog_key(another_now->dlg, &enter);   /* Folder... */
            snprintf(p, sizeof p, "%s/sub", f->path);
            fprintf(stderr, "selftest: templates: Folder...: the picker %d\n", filepick_is_open());
            filepick_go(p);
            filepick_use();
            fprintf(stderr, "selftest: templates: asked again: in %s\n", another_now && another_now->dlg ? strrchr(another_now->dir, '/') : "?");
            if (another_now && another_now->dlg) ui_dialog_key(another_now->dlg, &enter);   /* Create */
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            struct stat sst;
            snprintf(p, sizeof p, "%s/sub/made.txt", f->path);
            fprintf(stderr, "selftest: templates: Create another: sub/made.txt %d\n", stat(p, &sst) == 0);
            snprintf(p, sizeof p, "%s/a.txt", f->path);
            cv v = { .k = CV_BOOL, .b = true };
            svc_settings_set(p, "template", &v, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if ((step == 3 || step == 5) && t > 1500) {
            /* a.txt dragged within its folder: plain, then with Shift */
            snprintf(p, sizeof p, "%s/a.txt", f->path);
            selftest_pos(f, "a.txt", &ax, &ay);
            struct dragging d = { 0 };
            char *ps[1] = { p };
            bool tp[1] = { true };
            double dx[1] = { 0 }, dy[1] = { 0 };
            d.paths = ps; d.n = 1; d.tpl = tp; d.dx = dx; d.dy = dy;
            snprintf(d.src_folder, sizeof d.src_folder, "%s", f->path);
            char data[PATH_MAX + 16];
            snprintf(data, sizeof data, "%s\ttemplate\n", p);
            struct dragging *was = cur_drag;
            cur_drag = &d;
            win_drop(s, 560, step == 3 ? 400 : 450, MIME_OSD, data, strlen(data), UI_DND_MOVE, 0, step == 3 ? 0 : UI_MOD_SHIFT);
            cur_drag = was;
            step++;
            t0 = now;
        } else if (step == 4 && t > 2000) {
            double x, y;
            struct stat sst;
            snprintf(p, sizeof p, "%s/a (2).txt", f->path);
            fprintf(stderr, "selftest: templates: dragged: a new object %d, the template stayed %d\n", stat(p, &sst) == 0,
                    selftest_pos(f, "a.txt", &x, &y) && x == ax && y == ay);
            step++;
            t0 = now;
        } else if (step == 6 && t > 1500) {
            double x, y;
            struct stat sst;
            snprintf(p, sizeof p, "%s/a (3).txt", f->path);
            fprintf(stderr, "selftest: templates: Shift+dragged: moved %d, nothing made %d\n",
                    selftest_pos(f, "a.txt", &x, &y) && (x != ax || y != ay), stat(p, &sst) != 0);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "openwith")) {
        /* Open With (0.3 step 7): <D> has a.txt; text/plain's default is
         * the Hold program (selftest.sh), which keeps it open 2 s. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/a.txt", f->path);
        if (step == 0) {
            svc_menu(p, selftest_cascade, NULL);
            apppick_open("a.txt", p, open_with_chosen, f->id);
            step++;
            t0 = now;
        } else if (step == 1 && apppick_is_open()) {
            size_t hold = SIZE_MAX;
            for (size_t i = 0; i < apppick_count(); i++) if (!strcmp(apppick_name(i), "Hold")) hold = i;
            fprintf(stderr, "selftest: openwith: Other program: %zu programs, Hold among them %d\n", apppick_count(), hold != SIZE_MAX);
            const char *shot = getenv("OSD_SHELL_SHOT");
            if (shot) apppick_snapshot(shot);
            apppick_choose(hold, false);
            fprintf(stderr, "selftest: openwith: chosen: picker open %d\n", apppick_is_open());
            step++;
            t0 = now;
        } else if (step == 2 && t > 1000) {
            size_t a = SIZE_MAX;
            for (size_t i = 0; i < f->m.total; i++) if (model_name(&f->m, i) && !strcmp(model_name(&f->m, i), "a.txt")) a = i;
            struct row *rw = row_at(f, a);
            fprintf(stderr, "selftest: openwith: a.txt in use (opened with Hold, tracked) %d\n", rw && rw->in_use);
            ui_app_quit(app);
            step++;
        } else if (step == 1 && t > 5000) {
            fprintf(stderr, "selftest: openwith: the picker never filled\n");
            ui_app_quit(app);
        }
        return;
    }
    if (st && !strcmp(st, "layout")) {
        /* OS/2's layout and Lock position (0.3 step 6). <D> has a.txt
         * (a long title), b.txt .. h.txt. */
        static double t0, bx, by;
        static int step;
        if (!t0) { if (!f->m.complete || !f->arranged_by_name) return; t0 = now; }
        double t = now - t0;
        size_t b = selftest_index(f, "b.txt"), c = selftest_index(f, "c.txt");
        if (step == 0 && t > 1000) {
            size_t a = selftest_index(f, "a-rather-long-title-for-a-file-that-takes-four-lines.txt");
            fprintf(stderr, "selftest: layout: the long title's cell %.0f (a cell %.0f)\n", layout_item_h(f->lay, a), UI_CELL_H);
            selftest_cells(f, "arranged");
            layout_pos(f->lay, b, &bx, &by);
            lock_items(f, b);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            char bp[PATH_MAX];
            snprintf(bp, sizeof bp, "%s/b.txt", f->path);
            svc_settings(bp, selftest_lock_page, NULL);
            /* b and c dragged within the folder */
            struct dragging d = { 0 };
            char p0[PATH_MAX], p1[PATH_MAX], *ps[2] = { p0, p1 };
            snprintf(p0, sizeof p0, "%s/b.txt", f->path);
            snprintf(p1, sizeof p1, "%s/c.txt", f->path);
            double lx, ly, dx[2], dy[2];
            ui_container_to_list(f->cont, 300, 300, &lx, &ly);
            dx[0] = 400 - lx; dy[0] = 300 - ly;
            dx[1] = 500 - lx; dy[1] = 300 - ly;
            d.paths = ps; d.n = 2; d.dx = dx; d.dy = dy;
            snprintf(d.src_folder, sizeof d.src_folder, "%s", f->path);
            struct dragging *was = cur_drag;
            cur_drag = &d;
            const char *mimes[1] = { MIME_OSD }, *mime;
            d.n = 1;   /* the locked one alone over its folder: refused (the no-drop pointer) */
            uint32_t a1 = win_drag_motion(s, 600, 420, mimes, 1, UI_DND_COPY | UI_DND_MOVE, 0, &mime);
            d.n = 2;
            uint32_t a2 = win_drag_motion(s, 600, 420, mimes, 1, UI_DND_COPY | UI_DND_MOVE, 0, &mime);
            win_drag_leave(s);
            fprintf(stderr, "selftest: layout: drag motion: locked alone refused %d, with another moves %d\n", a1 == 0, a2 == UI_DND_MOVE);
            reposition(f, 300, 300);
            cur_drag = was;
            double x, y, cx, cy;
            layout_pos(f->lay, b, &x, &y);
            layout_pos(f->lay, c, &cx, &cy);
            fprintf(stderr, "selftest: layout: dragged: the locked one stayed %d, the other moved %d (%s)\n",
                    x == bx && y == by, cx == 500 && cy == 300, f->error);
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            arrange(f);
            double x, y;
            layout_pos(f->lay, b, &x, &y);
            fprintf(stderr, "selftest: layout: Arrange: the locked one stayed %d\n", x == bx && y == by);
            selftest_cells(f, "arranged around it");
            step++;
            t0 = now;
        } else if (step == 3 && t > 2000) {
            double x, y;
            layout_pos(f->lay, b, &x, &y);
            fprintf(stderr, "selftest: layout: read back: locked %d, in place %d\n", layout_is_locked(f->lay, "b.txt"), x == bx && y == by);
            lock_items(f, b);
            step++;
            t0 = now;
        } else if (step == 4 && t > 1500) {
            fprintf(stderr, "selftest: layout: unlocked: %d\n", !layout_is_locked(f->lay, "b.txt"));
            cv v = { .k = CV_INT, .i = 1 };
            svc_settings_set(f->path, "icon_titles", &v, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 5 && t > 2000) {
            size_t a = selftest_index(f, "a-rather-long-title-for-a-file-that-takes-four-lines.txt");
            fprintf(stderr, "selftest: layout: two lines: whole %d, the long title's cell %.0f\n", f->whole_titles, layout_item_h(f->lay, a));
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "search")) {
        /* The search field (0.3 step 5): Ctrl+F, "rep" typed, Escape; then
         * Show hidden files off and on. <D> has report-2025.ods,
         * report-2026.ods, reports/, notes.txt, .hidden. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char names[300] = "";
        for (size_t i = 0; i < f->m.total && strlen(names) < 250; i++)
            snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", i ? " " : "", model_name(&f->m, i) ? model_name(&f->m, i) : "?");
        if (step == 0) {
            fprintf(stderr, "selftest: search: at first %zu: %s\n", f->m.total, names);
            ui_key_event k = { .sym = XKB_KEY_f, .mods = UI_MOD_CTRL };
            win_key(s, &k);
            fprintf(stderr, "selftest: search: Ctrl+F: field shown %d, focused %d\n", f->find_on, f->find_focus);
            const char *typed = "rep";
            for (const char *c = typed; *c; c++) {   /* as typed (the input method's commit path) */
                ui_key_event kc = { .sym = (uint32_t)*c, .utf8 = { *c, 0 } };
                win_key(s, &kc);
            }
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500 && f->m.complete) {
            fprintf(stderr, "selftest: search: \"%s\": %zu: %s\n", f->filter, f->m.total, names);
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            win_key(s, &esc);
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500 && f->m.complete) {
            fprintf(stderr, "selftest: search: Escape: field shown %d, %zu objects\n", f->find_on, f->m.total);
            ui_key_event cf = { .sym = XKB_KEY_f, .mods = UI_MOD_CTRL };
            win_key(s, &cf);
            int shown = f->find_on;
            win_key(s, &cf);
            fprintf(stderr, "selftest: search: Ctrl+F twice: shown %d, then %d\n", shown, f->find_on);
            win_key(s, &cf);
            find_focus(f, false);   /* the keyboard on the objects (a click back into the window) */
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            win_key(s, &esc);
            fprintf(stderr, "selftest: search: Escape, the field not focused: shown %d\n", f->find_on);
            cv off = { .k = CV_BOOL, .b = false };   /* its View page's Show hidden files: listed again */
            svc_settings_set(f->path, "show_hidden", &off, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 3 && t > 1500 && f->m.complete) {
            fprintf(stderr, "selftest: search: hidden files off: %zu: %s\n", f->m.total, names);
            cv on = { .k = CV_BOOL, .b = true };
            svc_settings_set(f->path, "show_hidden", &on, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 4 && t > 1500 && f->m.complete) {
            fprintf(stderr, "selftest: search: hidden files on: %zu\n", f->m.total);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "trashlive") && getenv("OSD_SELFTEST_TRASH")) {
        /* The Trash Can's window follows deletes and restores of several
         * objects at once (a.txt b.txt c.txt in <D>) */
        static double t0;
        static int step;
        static long dw = -1;
        char tp[PATH_MAX];
        snprintf(tp, sizeof tp, "%s/files", getenv("OSD_SELFTEST_TRASH"));
        struct folder *tw = NULL, *d = dw >= 0 ? folder_by_id(dw) : NULL;
        for (struct folder *x = folders; x; x = x->next)
            if (!x->closing && !strcmp(x->path, tp)) tw = x;
        if (f == tw) return;   /* (driven from <D>'s window) */
        if (!t0) { if (!f->m.complete) return; t0 = now; dw = f->id; d = f; }
        if (f != d) return;
        double t = now - t0;
        char rows[300] = "";
        for (size_t i = 0; tw && i < tw->m.total && strlen(rows) < 250; i++)
            snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s%s", i ? " " : "",
                     model_name(&tw->m, i) ? model_name(&tw->m, i) : "?");
        if (step == 0) {
            mkdir(tp, 0700);
            open_window(tp, UI_VIEW_ICON, false);
            step++;
        } else if (step == 1 && t > 1500) {
            ui_container_select_none(d->cont);
            size_t at = SIZE_MAX;
            for (size_t i = 0; i < d->m.total; i++)
                if (model_name(&d->m, i) && strstr("a.txt b.txt c.txt", model_name(&d->m, i))) { ui_container_select(d->cont, i); at = i; }
            last_dialog = NULL;
            if (at != SIZE_MAX) delete_request(d, at, false);
            ui_key_event ret = { .sym = XKB_KEY_Return };
            if (last_dialog) ui_surface_send_key(ui_dialog_surface(last_dialog), &ret);
            last_dialog = NULL;
            step++;
        } else if (step == 2 && t > 4000) {
            fprintf(stderr, "selftest: trashlive: after deleting three: the Trash Can shows %zu: %s\n", tw ? tw->m.total : 0, rows);
            if (tw && tw->m.total) {
                ui_container_select_none(tw->cont);
                for (size_t i = 0; i < tw->m.total; i++) ui_container_select(tw->cont, i);
                restore_items(tw, 0);
            }
            step++;
        } else if (step == 3 && t > 7000) {
            fprintf(stderr, "selftest: trashlive: after restoring them: the Trash Can shows %zu: %s\n", tw ? tw->m.total : 0, rows);
            DIR *dd = opendir(tp);
            int left = 0;
            for (struct dirent *e; dd && (e = readdir(dd));) left += e->d_name[0] != '.';
            if (dd) closedir(dd);
            fprintf(stderr, "selftest: trashlive: in the trash folder itself: %d\n", left);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "fields")) {
        /* A notebook's file and colour fields (0.3): a colour typed (a bad
         * one put back), Browse... into <D>/pics (pictures only), one
         * picked into the field; a dropped path. */
        static double t0;
        static int step;
        ui_notebook *nb = selftest_nb;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        if (step == 0) {
            static const char *const kind[] = { "image" };
            static ui_field_desc fd[2];
            fd[0] = (ui_field_desc){ "picture", "Image", UI_FIELD_FILE, { .text = "" }, kind, 1, false, NULL, NULL };
            fd[1] = (ui_field_desc){ "colour", "Colour", UI_FIELD_COLOR, { .text = "#336699" }, NULL, 0, false, NULL, NULL };
            static ui_page_desc pd[1];
            pd[0] = (ui_page_desc){ "Background", fd, 2 };
            selftest_nb = ui_notebook_open(app, "Fields", pd, 1, selftest_field_set, NULL, NULL);
            step++;
        } else if (step == 1 && t > 800) {
            ui_notebook_set_value(nb, "colour", "nonsense");
            ui_notebook_set_value(nb, "colour", "#C00");
            fprintf(stderr, "selftest: fields: hint \"%s\"\n", ui_notebook_field_hint(nb, "picture"));
            const char *shot = getenv("OSD_FIELDS_SHOT");   /* prefix: -notebook.png, -picker.png */
            char png[PATH_MAX];
            if (shot) { snprintf(png, sizeof png, "%s-notebook.png", shot); ui_surface_snapshot(ui_notebook_surface(nb), png); }
            /* Browse... (as its button does): the owner opens the picker */
            ui_value v = { .text = NULL };
            selftest_field_set(nb, "picture", UI_FIELD_FILE, &v);
            step++;
        } else if (step == 2 && t > 1500) {
            char pics[PATH_MAX + 8];
            snprintf(pics, sizeof pics, "%s/pics", f->path);
            filepick_go(pics);
            char names[200] = "";
            for (size_t i = 0; i < filepick_count(); i++)
                snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", i ? " " : "", filepick_name(i));
            fprintf(stderr, "selftest: fields: the picker in pics shows: %s\n", names);
            step++;
        } else if (step == 3 && t > 2300) {
            const char *shot = getenv("OSD_FIELDS_SHOT");
            char png[PATH_MAX];
            if (shot) { snprintf(png, sizeof png, "%s-picker.png", shot); filepick_snapshot(png); }
            for (size_t i = 0; i < filepick_count(); i++)
                if (!strcmp(filepick_name(i), "sub")) filepick_activate(i);   /* into a folder */
            fprintf(stderr, "selftest: fields: into sub: %s\n", strrchr(filepick_folder(), '/') + 1);
            for (size_t i = 0; i < filepick_count(); i++)
                if (!strcmp(filepick_name(i), "b.png")) filepick_activate(i);   /* picked */
            fprintf(stderr, "selftest: fields: picker open after picking: %d\n", filepick_is_open());
            step++;
        } else if (step == 4 && t > 3000) {
            ui_notebook_close(nb);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "first") && f->desktop) {
        /* A new desktop's first start: its icons as they come, never more
         * than it ends with (an object listed twice for a moment) */
        static double t0;
        static long last = -1, most = 0;
        if (!t0) t0 = now;
        long n = (long)f->m.total;
        if (n != last) {
            char names[400] = "";
            for (size_t i = 0; i < f->m.total && strlen(names) < 300; i++) {
                struct row *rw = row_at(f, i);
                snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", i ? ", " : "",
                         rw && rw->title ? rw->title : model_name(&f->m, i) ? model_name(&f->m, i) : "?");
            }
            fprintf(stderr, "selftest: first start: %.1f s: %ld icons: %s\n", (now - t0) / 1000, n, names);
            last = n;
            if (n > most) most = n;
        }
        if (now - t0 > 5000) {
            fprintf(stderr, "selftest: first start: at most %ld icons, %ld at the end\n", most, n);
            ui_app_quit(app);
            t0 = 1e18;
        }
        return;
    }
    if (st && !strcmp(st, "ctxmenu")) {
        /* A right click on the background, as GNOME delivers it (with a
         * real-looking serial); the menu must still be open afterwards. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        ui_rect c = ui_frame_client(s);
        ui_pointer_event e = { .x = c.x + c.w - 60, .y = c.y + c.h - 60, .button = ui_mouse_button(UI_ACT_POPUP), .clicks = 1, .serial = 4242 };
        if (step == 0) { e.kind = UI_PRESS; win_pointer(s, &e); e.kind = UI_RELEASE; win_pointer(s, &e); step++; }
        else if (step == 1 && now - t0 > 700) { fprintf(stderr, "selftest: right click on background: menu open %d\n", ui_menu_is_open()); step++; }
        else if (step == 2) {
            /* and on an item */
            e.x = c.x + 40; e.y = c.y + 30;
            e.kind = UI_PRESS; win_pointer(s, &e); e.kind = UI_RELEASE; win_pointer(s, &e); step++; t0 = now;
        }
        else if (step == 3 && now - t0 > 700) {
            fprintf(stderr, "selftest: right click on an item: menu open %d\n", ui_menu_is_open());
            /* Choose View > Details view as the background menu would. */
            struct menu_ctx *mc = calloc(1, sizeof *mc);
            mc->folder = f->id;
            mc->item = SIZE_MAX;
            ui_view before = ui_container_view(f->cont);
            menu_chosen(mc, OSD_MI_VIEW_DETAILS);
            fprintf(stderr, "selftest: View > Details view: %s -> %s\n", before == UI_VIEW_ICON ? "icon" : "details",
                    ui_container_view(f->cont) == UI_VIEW_ICON ? "icon" : "details");
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "rename")) {
        /* F2 on old-name.txt, type "renamed", Enter: the file is renamed and
         * the window shows the new name. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        if (step == 0) {
            size_t i = SIZE_MAX;
            for (size_t k = 0; k < f->m.total; k++)
                if (model_name(&f->m, k) && !strcmp(model_name(&f->m, k), "old-name.txt")) i = k;
            if (i == SIZE_MAX) { fprintf(stderr, "selftest: old-name.txt not found\n"); ui_app_quit(app); return; }
            ui_container_set_cursor(f->cont, i);
            ui_key_event k = { .sym = XKB_KEY_F2 };
            win_key(s, &k);
            fprintf(stderr, "selftest: F2 starts a rename: %d\n", ui_container_renaming(f->cont));
            for (const char *p = "renamed"; *p; p++) {
                ui_key_event c = { .sym = (uint32_t)*p };
                c.utf8[0] = *p;
                win_key(s, &c);
            }
            ui_key_event ret = { .sym = XKB_KEY_Return };
            win_key(s, &ret);
            step++;
        } else if (step == 1 && now - t0 > 1500) {
            char p[PATH_MAX + 32];
            snprintf(p, sizeof p, "%s/renamed.txt", f->path);
            bool on_disk = access(p, F_OK) == 0;
            bool shown = false;
            for (size_t k = 0; k < f->m.total; k++)
                if (model_name(&f->m, k) && !strcmp(model_name(&f->m, k), "renamed.txt")) shown = true;
            fprintf(stderr, "selftest: renamed.txt on disk %d, in the window %d, status '%s'\n", on_disk, shown, f->error);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "props")) {
        /* Properties on old-name.txt, all by keyboard: Tab to Title, type,
         * Enter (renames the file); Tab to Template, Space; Escape closes. */
        static double t0;
        static int step;
        static ui_surface *nbs;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        if (step == 0) {
            size_t i = SIZE_MAX;
            for (size_t k = 0; k < f->m.total; k++)
                if (model_name(&f->m, k) && !strcmp(model_name(&f->m, k), "old-name.txt")) i = k;
            struct menu_ctx *mc = calloc(1, sizeof *mc);
            mc->folder = f->id;
            mc->item = i;
            menu_chosen(mc, OSD_MI_PROPERTIES);
            step++;
        } else if (step == 1 && props_open && props_open->nb) {
            nbs = ui_notebook_surface(props_open->nb);
            fprintf(stderr, "selftest: notebook open: %s\n", props_open->title);
            ui_key_event tab = { .sym = XKB_KEY_Tab }, ret = { .sym = XKB_KEY_Return };
            ui_surface_send_key(nbs, &tab);
            for (const char *p = "titled.txt"; *p; p++) {
                ui_key_event c = { .sym = (uint32_t)*p };
                c.utf8[0] = *p;
                ui_surface_send_key(nbs, &c);
            }
            ui_surface_send_key(nbs, &ret);
            step++;
            t0 = now;
        } else if (step == 2 && now - t0 > 800) {
            ui_key_event tab = { .sym = XKB_KEY_Tab }, sp = { .sym = XKB_KEY_space, .utf8 = " " };
            ui_surface_send_key(nbs, &tab);
            ui_surface_send_key(nbs, &sp);
            step++;
            t0 = now;
        } else if (step == 3 && now - t0 > 800) {
            fprintf(stderr, "selftest: notebook now for %s\n", props_open ? props_open->target : "(closed)");
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            ui_surface_send_key(nbs, &esc);
            step++;
            t0 = now;
        } else if (step == 4 && now - t0 > 500) {
            fprintf(stderr, "selftest: after Escape the notebook is %s\n", props_open ? "open" : "closed");
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "tree")) {
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        ui_key_event right = { .sym = XKB_KEY_Right }, left = { .sym = XKB_KEY_Left }, down = { .sym = XKB_KEY_Down };
        char rows[512] = "";
        for (size_t i = 0; i < f->ntvis && strlen(rows) < 400; i++) {
            ui_item it;
            src_item(f, i, &it);
            snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s%*s%s%s", i ? " | " : "", it.depth, "", it.name,
                     it.expandable ? (it.expanded ? "[-]" : "[+]") : "");
        }
        if (step == 0) { ui_key_event k2 = { .sym = XKB_KEY_2, .mods = UI_MOD_CTRL }; win_key(s, &k2); step++; }
        else if (step == 1 && t > 600) {
            fprintf(stderr, "selftest: tree: %s\n", rows);
            ui_container_set_cursor(f->cont, 0);
            win_key(s, &down);    /* to a */
            win_key(s, &right);   /* expand a */
            step++;
        } else if (step == 2 && t > 1200) {
            fprintf(stderr, "selftest: a expanded: %s\n", rows);
            for (size_t i = 0; i < f->ntvis; i++)
                if (!strcmp(f->tvis[i]->name, "b")) ui_container_set_cursor(f->cont, i);
            win_key(s, &right);   /* expand b: it has no subfolders */
            char p[PATH_MAX + 16];
            snprintf(p, sizeof p, "%s/a/a3", f->path);
            mkdir(p, 0755);        /* a new subfolder appears in the expanded a */
            step++;
        } else if (step == 3 && t > 2400) {
            fprintf(stderr, "selftest: b read, a3 made: %s\n", rows);
            for (size_t i = 0; i < f->ntvis; i++)
                if (!strcmp(f->tvis[i]->name, "a")) ui_container_set_cursor(f->cont, i);
            win_key(s, &left);     /* collapse a */
            step++;
        } else if (step == 4 && t > 3000) {
            fprintf(stderr, "selftest: a collapsed: %s\n", rows);
            step++;
        } else if (step == 5 && t > 3300) {
            size_t d = ui_surface_verify(s, getenv("OSD_VERIFY_PNG"));   /* OSD_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;   /* a change still being drawn: next frame */
            fprintf(stderr, "selftest: tree pixels against a fresh redraw: %zu differ\n", d);
            for (size_t i = 0; i < f->ntvis; i++)
                if (!strcmp(f->tvis[i]->name, "a")) ui_container_set_cursor(f->cont, i);
            win_key(s, &right);    /* expand a again */
            step++;
        } else if (step == 6 && t > 4000) {
            /* Delete with a selection in two folders: a/a1 and a/a2 in a, b
             * in the folder itself */
            ui_container_select_none(f->cont);
            size_t at = SIZE_MAX;
            for (size_t i = 0; i < f->ntvis; i++) {
                const char *nm = f->tvis[i]->name;
                if (!strcmp(nm, "a1") || !strcmp(nm, "b") || !strcmp(nm, "a2")) { ui_container_select(f->cont, i); at = i; }
            }
            last_dialog = NULL;
            if (at != SIZE_MAX) delete_request(f, at, false);
            fprintf(stderr, "selftest: tree Delete asked: %s\n", last_dialog ? "yes" : "no");
            ui_key_event ret = { .sym = XKB_KEY_Return };
            if (last_dialog) ui_surface_send_key(ui_dialog_surface(last_dialog), &ret);
            last_dialog = NULL;
            step++;
        } else if (step == 7 && t > 6000) {
            char p1[PATH_MAX + 8], p2[PATH_MAX + 8], p3[PATH_MAX + 8], p4[PATH_MAX + 8];
            snprintf(p1, sizeof p1, "%s/a/a1", f->path);
            snprintf(p2, sizeof p2, "%s/b", f->path);
            snprintf(p3, sizeof p3, "%s/a/a2", f->path);
            snprintf(p4, sizeof p4, "%s/a", f->path);
            fprintf(stderr, "selftest: after the tree's Delete: a/a1 %s, b %s, a/a2 %s, a %s\n", access(p1, F_OK) ? "gone" : "there",
                    access(p2, F_OK) ? "gone" : "there", access(p3, F_OK) ? "gone" : "there", access(p4, F_OK) ? "gone" : "there");
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "dnd") && f == folders) {   /* the last window opened: <D>/sub */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char D[PATH_MAX];
        snprintf(D, sizeof D, "%s", f->path);
        char *sl = strrchr(D, '/');
        *sl = 0;
        char data[PATH_MAX * 2];
        if (step == 0) {
            /* What a drag from the first window offers. */
            struct folder *src = f->next;
            size_t i = SIZE_MAX;
            for (size_t k = 0; k < src->m.total; k++)
                if (model_name(&src->m, k) && !strcmp(model_name(&src->m, k), "with space.txt")) i = k;
            src_drag(src, i, 0);
            if (cur_drag)   /* first line of each (the test prints, it does not change them) */
                fprintf(stderr, "selftest: drag data: osd \"%.*s\" uris \"%.*s\"\n", (int)strcspn(cur_drag->osd, "\n"),
                        cur_drag->osd, (int)strcspn(cur_drag->uris, "\r"), cur_drag->uris);
            /* A folder dropped into itself is refused. */
            struct dragging fake = { 0 };
            char *paths[1] = { f->path };
            fake.paths = paths;
            fake.n = 1;
            struct dragging *was = cur_drag;
            cur_drag = &fake;
            const char *mime, *mimes[] = { MIME_OSD };
            uint32_t act = win_drag_motion(s, 20, 60, mimes, 1, UI_DND_COPY | UI_DND_MOVE, 0, &mime);
            cur_drag = was;
            fprintf(stderr, "selftest: sub dropped into itself: action %u (0 = refused)\n", act);
            snprintf(data, sizeof data, "%s/a.txt\n", D);
            win_drop(s, 20, 60, MIME_OSD, data, strlen(data), UI_DND_MOVE, 0, 0);                      /* move */
            snprintf(data, sizeof data, "%s/b.txt\n", D);
            win_drop(s, 20, 60, MIME_OSD, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL);            /* copy */
            snprintf(data, sizeof data, "%s/sub2\n", D);
            win_drop(s, 20, 60, MIME_OSD, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL | UI_MOD_SHIFT);   /* shadow */
            data[0] = 0;
            char *u = NULL;
            size_t ul = 0, uc = 0;
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/with space.txt", D);
            uri_append(&u, &ul, &uc, p);
            win_drop(s, 20, 60, MIME_URIS, u, ul, UI_DND_MOVE, 0, 0);                                 /* from another app */
            free(u);
            step++;
        } else if (step == 1 && t > 1500) {
            snprintf(data, sizeof data, "%s/b.txt\n", D);
            win_drop(s, 20, 60, MIME_OSD, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL);            /* b.txt is there now */
            step++;
        } else if (step == 2 && last_dialog) {
            fprintf(stderr, "selftest: a name clash asks: dialog open\n");
            ui_key_event r = { .sym = XKB_KEY_r, .utf8 = "r" };
            ui_surface_send_key(ui_dialog_surface(last_dialog), &r);   /* Rename */
            last_dialog = NULL;
            step++;
            t0 = now;
        } else if (step == 3 && t > 1500) {
            fprintf(stderr, "selftest: status: %s\n", f->error);
            snprintf(data, sizeof data, "%s/b.txt\n", D);
            win_drop(s, 20, 60, MIME_OSD, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL);            /* and again */
            step++;
            t0 = now;
        } else if (step == 4 && last_dialog) {
            ui_key_event r = { .sym = XKB_KEY_r, .utf8 = "r" };
            ui_surface_send_key(ui_dialog_surface(last_dialog), &r);
            last_dialog = NULL;
            step++;
            t0 = now;
        } else if (step == 5 && (last_dialog || t > 2000)) {
            fprintf(stderr, "selftest: a second Rename asked again: %s\n", last_dialog ? "yes" : "no");
            /* What the window shows, with no other change to prompt a refresh. */
            char shown[1000] = "";
            for (size_t i = 0; i < f->m.total; i++) {
                struct page *pg = f->m.pages[i / PAGE];
                struct row *r = &pg->rows[i % PAGE];
                snprintf(shown + strlen(shown), sizeof shown - strlen(shown), " %s%s", r->name,
                         r->shadow ? (r->kind == UI_KIND_FOLDER ? "(shadow of a folder)" : "(shadow)") : "");
            }
            fprintf(stderr, "selftest: the window shows:%s\n", shown);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "dnd")) return;
    if (st && !strcmp(st, "place")) {
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char line[600] = "";
        for (size_t i = 0; i < f->m.total; i++) {
            double x, y;
            layout_pos(f->lay, i, &x, &y);
            snprintf(line + strlen(line), sizeof line - strlen(line), " %s@%.0f,%.0f", model_name(&f->m, i), x, y);
        }
        if (step == 0 && t > 500) {
            fprintf(stderr, "selftest: opened:%s\n", line);
            size_t bi = SIZE_MAX;
            for (size_t i = 0; i < f->m.total; i++) if (!strcmp(model_name(&f->m, i), "b.txt")) bi = i;
            /* Press the drag button on b's icon, as a real drag begins. */
            double bx, by;
            layout_pos(f->lay, bi, &bx, &by);
            ui_rect c = ui_frame_client(s);
            double sx = c.x + 4 + bx + 50, sy = c.y + 4 + by + 20;   /* on the icon */
            ui_pointer_event pr = { .kind = UI_PRESS, .x = sx, .y = sy, .button = ui_mouse_button(UI_ACT_DRAG), .clicks = 1 };
            win_pointer(s, &pr);
            src_drag(f, bi, 0);
            char data[PATH_MAX];
            snprintf(data, sizeof data, "%s/b.txt\n", f->path);
            win_drop(s, sx + 150, sy + 200, MIME_OSD, data, strlen(data), UI_DND_MOVE, 0, 0);
            ui_pointer_event rl = { .kind = UI_RELEASE, .x = sx + 150, .y = sy + 200, .button = ui_mouse_button(UI_ACT_DRAG) };
            win_pointer(s, &rl);
            step++;
        } else if (step == 1 && t > 1500) {
            fprintf(stderr, "selftest: after moving b:%s\n", line);
            struct menu_ctx *mc = calloc(1, sizeof *mc);
            mc->folder = f->id;
            mc->item = SIZE_MAX;
            menu_chosen(mc, OSD_MI_ARRANGE);
            step++;
        } else if (step == 2 && t > 2500) {
            fprintf(stderr, "selftest: after Arrange:%s\n", line);
            step++;
        } else if (step == 3 && t > 2800) {
            size_t d = ui_surface_verify(s, getenv("OSD_VERIFY_PNG"));   /* OSD_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "shadows")) {
        static double t0;
        static int step;
        static struct folder *home;
        if (!home) { if (!f->m.complete) return; home = f; t0 = now; }
        if (f != home) return;
        double t = now - t0;
        char data[PATH_MAX];
        size_t si = SIZE_MAX, ti = SIZE_MAX, li = SIZE_MAX;
        for (size_t i = 0; i < f->m.total; i++) {
            const char *n = model_name(&f->m, i);
            if (!n) continue;
            struct page *pg = f->m.pages[i / PAGE];
            if (pg && pg->rows[i % PAGE].shadow) si = i;
            if (!strcmp(n, "T")) ti = i;
            if (!strcmp(n, "link-to-T")) li = i;
        }
        int nT = 0;
        char tpath[PATH_MAX];
        snprintf(tpath, sizeof tpath, "%s/T", f->path);
        for (struct folder *o = folders; o; o = o->next) nT += !o->closing && !strcmp(o->path, tpath);
        if (step == 0) {
            snprintf(data, sizeof data, "%s/T\n", f->path);
            ui_rect c = ui_frame_client(s);   /* on the background, away from the icons */
            win_drop(s, c.x + c.w - 60, c.y + c.h - 60, MIME_OSD, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL | UI_MOD_SHIFT);
            step++;
        } else if (step == 1 && t > 1500 && si != SIZE_MAX) {
            ui_item it;
            src_item(f, si, &it);
            fprintf(stderr, "selftest: shadow row: shadow %d, a folder %d, title '%s'\n", it.shadow, it.kind == UI_KIND_FOLDER, it.title);
            src_activate(f, si);
            step++;
            t0 = now;
        } else if (step == 2 && t > 1200) {
            fprintf(stderr, "selftest: windows on T after opening the shadow: %d\n", nT);
            src_activate(f, si);
            step++;
            t0 = now;
        } else if (step == 3 && t > 1200) {
            ui_item it;
            src_item(f, ti, &it);
            ui_item sh;
            src_item(f, si, &sh);
            fprintf(stderr, "selftest: after opening it again: %d window(s); T shown in use: %d, its shadow too: %d\n", nT,
                    it.in_use, sh.in_use);
            for (struct folder *o = folders; o; o = o->next)
                if (!strcmp(o->path, tpath)) win_close(o->win);
            step++;
            t0 = now;
        } else if (step == 4 && t > 800) {
            ui_item it;
            src_item(f, ti, &it);
            fprintf(stderr, "selftest: T window closed: in use %d\n", it.in_use);
            if (li != SIZE_MAX) {
                ui_item lk;
                src_item(f, li, &lk);
                fprintf(stderr, "selftest: link-to-T drawn as: a folder %d, link mark %d\n", lk.kind == UI_KIND_FOLDER, lk.link);
            }
            if (li != SIZE_MAX) src_activate(f, li);
            step++;
            t0 = now;
        } else if (step == 5 && t > 1200) {
            fprintf(stderr, "selftest: a plain link to T opened T: %d window(s)\n", nT);
            step++;
        } else if (step == 6) {
            size_t d = ui_surface_verify(s, getenv("OSD_VERIFY_PNG"));   /* OSD_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "dropkeys")) {
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        static const char *names[] = { "move", "copy", "shadow" };
        if (step == 0) {
            int a = drop_how(UI_DND_MOVE, UI_DND_ASK, 0), b = drop_how(UI_DND_COPY, UI_DND_ASK, 0), c = drop_how(UI_DND_ASK, UI_DND_ASK, 0),
                d = drop_how(UI_DND_MOVE, UI_DND_ASK, UI_MOD_CTRL | UI_MOD_SHIFT);
            fprintf(stderr, "selftest: ask->move: %s; ask->copy: %s; ask unchanged: %s; Ctrl+Shift held: %s\n",
                    a < 0 ? "ask" : names[a], b < 0 ? "ask" : names[b], c < 0 ? "ask" : names[c], d < 0 ? "ask" : names[d]);
            /* The logged drag, end to end: began as ask, changed to move, no keys seen. */
            char data[PATH_MAX];
            snprintf(data, sizeof data, "%s/T\n", f->path);
            ui_rect cl = ui_frame_client(s);
            win_drop(s, cl.x + cl.w - 60, cl.y + cl.h - 60, MIME_OSD, data, strlen(data), UI_DND_MOVE, UI_DND_ASK, 0);
            step++;
        } else if (step == 1 && now - t0 > 1500) {
            bool made = false;
            for (size_t i = 0; i < f->m.total; i++) {
                struct page *pg = f->m.pages[i / PAGE];
                made |= pg && pg->rows[i % PAGE].shadow;
            }
            fprintf(stderr, "selftest: the logged drag made a shadow: %d\n", made);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "droppos")) {
        /* Drops onto the background land where they were dropped. */
        static double t0;
        static int step;
        static double sx, sy, cx, cy;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char data[PATH_MAX];
        ui_rect cl = ui_frame_client(s);
        if (step == 0) {
            sx = cl.x + 260; sy = cl.y + 200;
            snprintf(data, sizeof data, "%s/T\n", f->path);
            win_drop(s, sx, sy, MIME_OSD, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL | UI_MOD_SHIFT);   /* a shadow */
            cx = cl.x + 420; cy = cl.y + 120;
            snprintf(data, sizeof data, "%s/f.txt\n", f->path);
            win_drop(s, cx, cy, MIME_OSD, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL);                 /* a copy (renamed) */
            step++;
        } else if (step == 1 && last_dialog) {
            ui_key_event r = { .sym = XKB_KEY_r, .utf8 = "r" };
            ui_surface_send_key(ui_dialog_surface(last_dialog), &r);
            last_dialog = NULL;
            step++;
            t0 = now;
        } else if (step == 2 && t > 2000) {
            double lsx, lsy, lcx, lcy;
            ui_container_to_list(f->cont, sx, sy, &lsx, &lsy);
            ui_container_to_list(f->cont, cx, cy, &lcx, &lcy);
            for (size_t i = 0; i < f->m.total; i++) {
                const char *n = model_name(&f->m, i);
                struct page *pg = f->m.pages[i / PAGE];
                double x, y;
                layout_pos(f->lay, i, &x, &y);
                if (pg && pg->rows[i % PAGE].shadow)
                    fprintf(stderr, "selftest: shadow at %.0f,%.0f (dropped at %.0f,%.0f)\n", x, y, lsx, lsy);
                if (n && !strcmp(n, "f (2).txt"))
                    fprintf(stderr, "selftest: copy 'f (2).txt' at %.0f,%.0f (dropped at %.0f,%.0f)\n", x, y, lcx, lcy);
            }
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "inuse")) {
        /* a.txt opened with a program that holds it for 2 s (the runner sets
         * up the default application); then a lock file marks b.txt. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        size_t ai = SIZE_MAX, bi = SIZE_MAX;
        for (size_t i = 0; i < f->m.total; i++) {
            const char *n = model_name(&f->m, i);
            if (n && !strcmp(n, "a.txt")) ai = i;
            if (n && !strcmp(n, "b.txt")) bi = i;
        }
        ui_item ia, ib;
        if (ai != SIZE_MAX) src_item(f, ai, &ia);
        if (bi != SIZE_MAX) src_item(f, bi, &ib);
        if (step == 0 && ai != SIZE_MAX) { src_activate(f, ai); step++; t0 = now; }
        else if (step == 1 && t > 800) { fprintf(stderr, "selftest: a.txt open: in use %d\n", ia.in_use); step++; }
        else if (step == 2 && t > 3200) {
            fprintf(stderr, "selftest: its program ended: in use %d\n", ia.in_use);
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/.~lock.b.txt#", f->path);
            FILE *lf = fopen(p, "w");
            if (lf) fclose(lf);
            step++;
            t0 = now;
        } else if (step == 3 && t > 1500) {
            fprintf(stderr, "selftest: b.txt with a lock file: in use %d\n", bi != SIZE_MAX && ib.in_use);
            step++;
        } else if (step == 4) {
            size_t d = ui_surface_verify(s, getenv("OSD_VERIFY_PNG"));   /* OSD_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "delete")) {
        /* a.txt and sub selected, Delete, Enter: into the trash; then b.txt,
         * Shift+Delete, Enter: gone for good. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        size_t ai = SIZE_MAX, si = SIZE_MAX, bi = SIZE_MAX;
        for (size_t i = 0; i < f->m.total; i++) {
            const char *n = model_name(&f->m, i);
            if (n && !strcmp(n, "a.txt")) ai = i;
            if (n && !strcmp(n, "sub")) si = i;
            if (n && !strcmp(n, "b.txt")) bi = i;
        }
        ui_key_event del = { .sym = XKB_KEY_Delete }, sdel = { .sym = XKB_KEY_Delete, .mods = UI_MOD_SHIFT },
                     ret = { .sym = XKB_KEY_Return };
        char p[PATH_MAX + 64];
        const char *dh = getenv("OSD_SELFTEST_TRASH");
        if (step == 0 && ai != SIZE_MAX && si != SIZE_MAX) {
            ui_container_select_none(f->cont);
            ui_container_select(f->cont, ai);
            ui_container_select(f->cont, si);
            win_key(s, &del);
            fprintf(stderr, "selftest: Delete asks: %s\n", last_dialog ? "yes" : "no");
            if (last_dialog) ui_surface_send_key(ui_dialog_surface(last_dialog), &ret);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            snprintf(p, sizeof p, "%s/files/a.txt", dh ? dh : "");
            bool a_in = access(p, F_OK) == 0;
            snprintf(p, sizeof p, "%s/files/sub", dh ? dh : "");
            fprintf(stderr, "selftest: after Delete: a.txt shown %d, sub shown %d; in the trash: a.txt %d, sub %d; status '%s'\n",
                    ai != SIZE_MAX, si != SIZE_MAX, a_in, access(p, F_OK) == 0, f->error);
            if (bi != SIZE_MAX) {
                ui_container_select_none(f->cont);
                ui_container_select(f->cont, bi);
                win_key(s, &sdel);
                if (last_dialog) ui_surface_send_key(ui_dialog_surface(last_dialog), &ret);
            }
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            snprintf(p, sizeof p, "%s/files/b.txt", dh ? dh : "");
            char q[PATH_MAX];
            snprintf(q, sizeof q, "%s/b.txt", f->path);
            fprintf(stderr, "selftest: after Shift+Delete: b.txt shown %d, on disk %d, in the trash %d; status '%s'\n",
                    bi != SIZE_MAX, access(q, F_OK) == 0, access(p, F_OK) == 0, f->error);
            step++;
        } else if (step == 3) {
            size_t d = ui_surface_verify(s, getenv("OSD_VERIFY_PNG"));   /* OSD_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "desktop")) {
        /* The desktop: frameless, its folder's icons on the wallpaper. */
        static double t0;
        static int step;
        if (!f->desktop) return;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        if (step == 0 && (strcmp(wallpaper_showing(), "color") || t > 20000)) {
            ui_rect c = ui_frame_client(s);
            fprintf(stderr, "selftest: desktop %s: %zu objects, frameless %d (%.0fx%.0f of %.0fx%.0f), background %s after %.1f s\n",
                    f->path, f->m.total, c.x == 0 && c.y == 0 && c.w == ui_surface_width(s) && c.h == ui_surface_height(s),
                    c.w, c.h, ui_surface_width(s), ui_surface_height(s), wallpaper_showing(), t / 1000);
            win_close(s);   /* the desktop stays */
            step++;
            t0 = now;
        } else if (step == 1 && t > 300) {
            fprintf(stderr, "selftest: after Close: desktop still open %d\n", !f->closing);
            if (getenv("OSD_SHELL_SHOT")) {   /* a picture of it (with OSD_UI_NO_LAYER=1: all on one surface) */
                cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ui_surface_width(s), (int)ui_surface_height(s));
                cairo_t *icr = cairo_create(img);
                win_draw(s, icr, (ui_rect){ 0, 0, ui_surface_width(s), ui_surface_height(s) });
                cairo_destroy(icr);
                cairo_surface_write_to_png(img, getenv("OSD_SHELL_SHOT"));
                cairo_surface_destroy(img);
            }
            step++;
        } else if (step == 2) {
            size_t d = ui_surface_verify(s, getenv("OSD_VERIFY_PNG"));   /* OSD_VERIFY_PNG=prefix saves a mismatch */
            static double last;
            (void)last;
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "templates")) {
        /* Two windows: <D>/Templates (the templates folder) and <D>. Drop
         * the built-in Folder template on <D>, type a name; drop Data File
         * and the Letter.odt template; Create another on the new folder. */
        static double t0;
        static int step;
        struct folder *tw = NULL;
        for (struct folder *o = folders; o; o = o->next)
            if (strlen(o->path) > 10 && !strcmp(o->path + strlen(o->path) - 10, "/Templates")) tw = o;
        if (f == tw || !tw) return;
        if (!t0) { if (!f->m.complete || !tw->m.complete) return; t0 = now; }
        double t = now - t0;
        ui_rect cl = ui_frame_client(s);
        char data[PATH_MAX + 32], tpl_folder[PATH_MAX] = "", tpl_data[PATH_MAX] = "", tpl_letter[PATH_MAX] = "";
        char rows[600] = "";
        for (size_t i = 0; i < tw->m.total; i++) {
            struct row *r = row_at(tw, i);
            if (!r || !r->name) continue;
            ui_item it;
            src_item(tw, i, &it);
            snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s%s(%s%s)", rows[0] ? ", " : "", it.title ? it.title : r->name,
                     r->kind == UI_KIND_FOLDER ? "folder" : r->kind == UI_KIND_PROGRAM ? "program" : "file", r->tpl ? ", template" : "");
            char tg[PATH_MAX];
            target_of(tw, i, tg, sizeof tg);
            if (r->tpl && r->name[0] == '\x01' && r->kind == UI_KIND_FOLDER) snprintf(tpl_folder, sizeof tpl_folder, "%s", tg);
            if (r->tpl && r->name[0] == '\x01' && r->kind == UI_KIND_FILE) snprintf(tpl_data, sizeof tpl_data, "%s", tg);
            if (r->tpl && !strcmp(r->name, "Letter.odt")) snprintf(tpl_letter, sizeof tpl_letter, "%s", tg);
        }
        char p[PATH_MAX + 32];
        if (step == 0 && t > 300 && tpl_folder[0] && tpl_data[0] && tpl_letter[0]) {
            fprintf(stderr, "selftest: the templates folder: %s\n", rows);
            snprintf(data, sizeof data, "%s\ttemplate\n", tpl_folder);
            win_drop(s, cl.x + 220, cl.y + 150, MIME_OSD, data, strlen(data), UI_DND_MOVE, 0, 0);
            step++;
            t0 = now;
        } else if (step == 0 && t > 5000) {
            fprintf(stderr, "selftest: the templates folder lacks the templates: %s\n", rows);
            ui_app_quit(app);
            step = 99;
        } else if (step == 1 && t > 1500) {
            size_t ri = ui_container_rename_item(f->cont);
            fprintf(stderr, "selftest: dropped the Folder template: Folder made %d, its name being edited %d\n",
                    (snprintf(p, sizeof p, "%s/Folder", f->path), access(p, F_OK) == 0), ri != SIZE_MAX && !strcmp(model_name(&f->m, ri), "Folder"));
            if (ri != SIZE_MAX) {
                ui_key_event sa = { .sym = XKB_KEY_a, .mods = UI_MOD_CTRL };
                ui_surface_send_key(s, &sa);
                for (const char *c = "Letters"; *c; c++) {
                    ui_key_event k = { .sym = (uint32_t)*c };
                    k.utf8[0] = *c;
                    ui_surface_send_key(s, &k);
                }
                ui_key_event en = { .sym = XKB_KEY_Return };
                ui_surface_send_key(s, &en);
            }
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            snprintf(p, sizeof p, "%s/Letters", f->path);
            fprintf(stderr, "selftest: typed a name: Letters on disk %d\n", access(p, F_OK) == 0);
            snprintf(data, sizeof data, "%s\ttemplate\n%s\ttemplate\n", tpl_data, tpl_letter);
            win_drop(s, cl.x + 420, cl.y + 150, MIME_OSD, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL);
            step++;
            t0 = now;
        } else if (step == 3 && t > 2000) {
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            if (ui_container_rename_item(f->cont) != SIZE_MAX) ui_surface_send_key(s, &esc);
            char a1[PATH_MAX + 32], a2[PATH_MAX + 32];
            snprintf(a1, sizeof a1, "%s/Data File", f->path);
            snprintf(a2, sizeof a2, "%s/Letter.odt", f->path);
            snprintf(p, sizeof p, "%s/Templates/Letter.odt", f->path);
            fprintf(stderr, "selftest: dropped Data File and Letter.odt: made %d %d; the templates stay %d\n",
                    access(a1, F_OK) == 0, access(a2, F_OK) == 0, access(p, F_OK) == 0);
            size_t li = SIZE_MAX;
            for (size_t i = 0; i < f->m.total; i++)
                if (model_name(&f->m, i) && !strcmp(model_name(&f->m, i), "Letters")) li = i;
            struct menu_ctx *mc = calloc(1, sizeof *mc);
            mc->folder = f->id;
            mc->item = li;
            menu_chosen(mc, OSD_MI_CREATE_ANOTHER);
            step++;
            t0 = now;
        } else if (step == 4 && t > 1500) {
            size_t ri = ui_container_rename_item(f->cont);
            snprintf(p, sizeof p, "%s/New Folder", f->path);
            fprintf(stderr, "selftest: Create another on Letters: New Folder %d, its name being edited %d\n", access(p, F_OK) == 0,
                    ri != SIZE_MAX && !strcmp(model_name(&f->m, ri), "New Folder"));
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            if (ri != SIZE_MAX) ui_surface_send_key(s, &esc);
            step++;
        } else if (step == 5) {
            size_t d = ui_surface_verify(s, getenv("OSD_VERIFY_PNG"));   /* OSD_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            const char *shot = getenv("OSD_SHELL_SHOT");   /* the templates window (with OSD_UI_NO_LAYER=1) */
            if (shot) {
                cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ui_surface_width(tw->win), (int)ui_surface_height(tw->win));
                cairo_t *icr = cairo_create(img);
                win_draw(tw->win, icr, (ui_rect){ 0, 0, ui_surface_width(tw->win), ui_surface_height(tw->win) });
                cairo_destroy(icr);
                cairo_surface_write_to_png(img, shot);
                cairo_surface_destroy(img);
            }
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "follow")) {
        /* A 3000-entry folder follows changes without being read again:
         * 40 new files, 20 renamed, 20 deleted; the selection stays. */
        static double t0;
        static int step;
        char p[PATH_MAX + 64], q[PATH_MAX + 64];
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        if (step == 0 && t > 500) {
            /* a row on screen (a big folder's window has read only those) */
            ssize_t keep = f->vis_first + 5 < f->m.total && model_name(&f->m, f->vis_first + 5) ? (ssize_t)(f->vis_first + 5) : -1;
            static char kept[NAME_MAX + 1];
            if (keep >= 0) {
                snprintf(kept, sizeof kept, "%s", model_name(&f->m, (size_t)keep));
                ui_container_select(f->cont, (size_t)keep);
                ui_container_set_cursor(f->cont, (size_t)keep);
            }
            setenv("OSD_SELFTEST_KEPT", kept, 1);
            reloads = 0;
            int adds = getenv("FOLLOW_ADD") ? atoi(getenv("FOLLOW_ADD")) : 40;   /* a burst, for timing */
            for (int i = 0; i < adds; i++) { snprintf(p, sizeof p, "%s/new-%04d", f->path, i); FILE *x = fopen(p, "w"); if (x) fclose(x); }
            for (int i = 0; i < 20; i++) {
                snprintf(p, sizeof p, "%s/file-%04d", f->path, i * 7);
                snprintf(q, sizeof q, "%s/moved-%02d", f->path, i);
                rename(p, q);
            }
            for (int i = 0; i < 20; i++) { snprintf(p, sizeof p, "%s/file-%04d", f->path, 2000 + i); unlink(p); }
            step++;
            t0 = now;
        } else if (step == 1 && t > (getenv("FOLLOW_WAIT") ? atof(getenv("FOLLOW_WAIT")) : 2500)) {
            /* the rows against the folder itself */
            /* every row read so far exists, and the count is the folder's */
            size_t on_disk = 0, wrong = 0, loaded = 0;
            DIR *d = opendir(f->path);
            struct dirent *de;
            while (d && (de = readdir(d))) on_disk += de->d_name[0] != '.';
            if (d) closedir(d);
            for (size_t i = 0; i < f->m.total; i++) {
                const char *n = model_name(&f->m, i);
                if (!n) continue;
                loaded++;
                snprintf(p, sizeof p, "%s/%s", f->path, n);
                if (access(p, F_OK) != 0) wrong++;
            }
            const char *kept = getenv("OSD_SELFTEST_KEPT");
            ssize_t k = kept && *kept ? model_find(&f->m, kept) : -1;
            fprintf(stderr, "selftest: on disk %zu, in the window %zu (%zu names read, %zu not on disk); full re-reads %d; "
                    "%s still selected %d\n", on_disk, f->m.total, loaded, wrong, reloads, kept ? kept : "?",
                    k >= 0 && ui_container_is_selected(f->cont, (size_t)k));
            step++;
        } else if (step == 2) {
            size_t d = ui_surface_verify(s, getenv("OSD_VERIFY_PNG"));   /* OSD_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "events")) {
        /* Action events and their sounds: a menu, then deleting a.txt
         * (asked first: alert.warning; then object.trash). */
        static double t0;
        static int step;
        static char seen[1024];
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        if (step == 0) {
            events_subscribe(selftest_record, seen);
            ui_menu_item one[] = { { 1, "One", true, false, NULL, 0 } };
            ui_menu_popup(s, one, 1, 50, 50, 0, selftest_chosen, NULL);
            step++;
            t0 = now;
        } else if (step == 1 && t > 500) {
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            ui_surface_send_key(s, &esc);
            ssize_t ai = model_find(&f->m, "a.txt");
            if (ai >= 0) delete_request(f, (size_t)ai, false);
            ui_key_event ret = { .sym = XKB_KEY_Return };
            if (last_dialog) ui_surface_send_key(ui_dialog_surface(last_dialog), &ret);
            step++;
            t0 = now;
        } else if (step == 2 && t > 2000) {
            fprintf(stderr, "selftest: events:%s\n", seen);
            fprintf(stderr, "selftest: sound: PipeWire %s, formats %s, %u sounds started\n", sound_ready() ? "connected" : "not there",
                    sound_formats(), sound_played());
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "setup")) {
        /* The settings objects, live: the Scheme Palette to dark, System's
         * corners and delete confirmation, a Sound event; then a delete
         * that is not asked about. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete || !setup_ids[0][0]) return; t0 = now; }
        double t = now - t0;
        const char *sys = NULL, *snd = NULL, *pal = NULL;
        for (int i = 0; i < 4; i++) {
            /* the order is the service's: System, Sound, Mouse, Scheme Palette */
            if (i == 0) sys = setup_ids[i];
            if (i == 1) snd = setup_ids[i];
            if (i == 3) pal = setup_ids[i];
        }
        if (step == 0) {
            fprintf(stderr, "selftest: at start: scheme %s, corners %.0f, confirm delete %d, window.open sound %s\n",
                    ui_scheme_now.name, ui_scheme_now.corner_radius, confirm_delete, events_sound_for("window.open") ? "yes" : "none");
            cv one = { .k = CV_INT, .i = 1 }, three = { .k = CV_INT, .i = 3 }, no = { .k = CV_BOOL, .b = false },
               bell = { .k = CV_TEXT, .s = (char *)"bell", .len = 4 };
            svc_settings_set(pal, "scheme", &one, -1, ignore_reply, NULL);
            svc_settings_set(sys, "corner_radius", &three, -1, ignore_reply, NULL);
            svc_settings_set(sys, "confirm_delete", &no, -1, ignore_reply, NULL);
            svc_settings_set(snd, "window.open", &bell, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            fprintf(stderr, "selftest: after the notebooks' changes: scheme %s, corners %.0f, confirm delete %d, window.open sound %s\n",
                    ui_scheme_now.name, ui_scheme_now.corner_radius, confirm_delete,
                    events_sound_for("window.open") ? strrchr(events_sound_for("window.open"), '/') + 1 : "none");
            ssize_t ai = model_find(&f->m, "a.txt");
            last_dialog = NULL;
            if (ai >= 0) delete_request(f, (size_t)ai, false);
            fprintf(stderr, "selftest: Delete asked: %s\n", last_dialog ? "yes" : "no");
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            fprintf(stderr, "selftest: a.txt shown %d\n", model_find(&f->m, "a.txt") >= 0);
            const char *shot = getenv("OSD_SHELL_SHOT");   /* the window in the dark scheme (OSD_UI_NO_LAYER=1) */
            if (shot) {
                cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ui_surface_width(s), (int)ui_surface_height(s));
                cairo_t *icr = cairo_create(img);
                win_draw(s, icr, (ui_rect){ 0, 0, ui_surface_width(s), ui_surface_height(s) });
                cairo_destroy(icr);
                cairo_surface_write_to_png(img, shot);
                cairo_surface_destroy(img);
            }
            step++;
        } else if (step == 3) {
            size_t d = ui_surface_verify(s, getenv("OSD_VERIFY_PNG"));   /* OSD_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "newfolder")) {
        /* As reported: a folder from the Folder template, opened; a file
         * from the Data File template dropped into its window; then the
         * open folder renamed. */
        static double t0;
        static int step;
        static struct folder *nw;
        struct folder *tw = NULL;
        for (struct folder *o = folders; o; o = o->next)
            if (strlen(o->path) > 10 && !strcmp(o->path + strlen(o->path) - 10, "/Templates")) tw = o;
        if (f == tw || !tw || (nw && f == nw)) return;
        if (!t0) { if (!f->m.complete || !tw->m.complete) return; t0 = now; }
        double t = now - t0;
        char tf[PATH_MAX] = "", td[PATH_MAX] = "", data[PATH_MAX + 32], p[PATH_MAX + 32];
        for (size_t i = 0; i < tw->m.total; i++) {
            struct row *r = row_at(tw, i);
            if (!r || !r->name || !r->tpl || r->name[0] != '\x01') continue;
            if (r->kind == UI_KIND_FOLDER) target_of(tw, i, tf, sizeof tf);
            if (r->kind == UI_KIND_FILE) target_of(tw, i, td, sizeof td);
        }
        if (step == 0 && tf[0] && td[0]) {
            snprintf(data, sizeof data, "%s\ttemplate\n", tf);
            win_drop(s, 200, 150, MIME_OSD, data, strlen(data), UI_DND_MOVE, 0, 0);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            if (ui_container_rename_item(f->cont) != SIZE_MAX) ui_surface_send_key(s, &esc);
            snprintf(p, sizeof p, "%s/Folder", f->path);
            nw = open_window(p, UI_VIEW_ICON, false);
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500 && nw && nw->m.complete) {
            snprintf(data, sizeof data, "%s\ttemplate\n", td);
            win_drop(nw->win, 150, 120, MIME_OSD, data, strlen(data), UI_DND_MOVE, 0, 0);
            step++;
            t0 = now;
        } else if (step == 3 && t > 2000) {
            snprintf(p, sizeof p, "%s/Folder/Data File", f->path);
            fprintf(stderr, "selftest: in the new folder: Data File on disk %d, in its window %d (window %zu objects, rename open %d)\n",
                    access(p, F_OK) == 0, model_find(&nw->m, "Data File") >= 0, nw->m.total, ui_container_rename_item(nw->cont) != SIZE_MAX);
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            if (ui_container_rename_item(nw->cont) != SIZE_MAX) ui_surface_send_key(nw->win, &esc);
            /* three more Data Files, each dropped at its own spot */
            snprintf(data, sizeof data, "%s\ttemplate\n", td);
            static const double spots[3][2] = { { 350, 120 }, { 150, 300 }, { 400, 330 } };
            for (int k = 0; k < 3; k++) win_drop(nw->win, spots[k][0], spots[k][1], MIME_OSD, data, strlen(data), UI_DND_MOVE, 0, 0);
            step = 10;
            t0 = now;
        } else if (step == 10 && t > 3000) {
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            if (ui_container_rename_item(nw->cont) != SIZE_MAX) ui_surface_send_key(nw->win, &esc);
            static const double spots[4][2] = { { 150, 120 }, { 350, 120 }, { 150, 300 }, { 400, 330 } };
            char out[1024] = "";
            for (size_t i = 0; i < nw->m.total; i++) {
                ui_item it = { 0 };
                src_item(nw, i, &it);
                double x, y, lx = 0, ly = 0;
                layout_pos(nw->lay, i, &x, &y);
                /* where it was dropped (each in drop order: Data File, (2), (3), (4)) */
                const char *n = model_name(&nw->m, i);
                int k = !n ? -1 : !strcmp(n, "Data File") ? 0 : !strcmp(n, "Data File (2)") ? 1 : !strcmp(n, "Data File (3)") ? 2 :
                        !strcmp(n, "Data File (4)") ? 3 : -1;
                if (k >= 0) ui_container_to_list(nw->cont, spots[k][0], spots[k][1], &lx, &ly);
                snprintf(out + strlen(out), sizeof out - strlen(out), " [%s titled '%s' at %.0f,%.0f, dropped at %.0f,%.0f]",
                         n ? n : "?", it.title ? it.title : "", x, y, lx, ly);
            }
            fprintf(stderr, "selftest: four Data Files:%s\n", out);
            /* rename the open folder (as its icon's rename does) */
            snprintf(p, sizeof p, "%s/Folder", f->path);
            svc_rename(p, "Renamed", ignore_reply, NULL);
            step = 4;
            t0 = now;
        } else if (step == 4 && t > 2000) {
            snprintf(p, sizeof p, "%s/Renamed", f->path);
            ssize_t ri = model_find(&f->m, "Renamed");
            ui_item it = { 0 };
            if (ri >= 0) src_item(f, (size_t)ri, &it);
            fprintf(stderr, "selftest: after renaming the open folder: its window shows %s; its icon in use %d\n", nw->path, it.in_use);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "appearance")) {
        /* The System object's window options: strong shadows, translucent
         * title bars, the mirrored layout; then the frame and the buffer. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete || !setup_ids[0][0]) return; t0 = now; }
        double t = now - t0;
        if (step == 0) {
            cv two = { .k = CV_INT, .i = 2 }, one = { .k = CV_INT, .i = 1 };
            svc_settings_set(setup_ids[0], "shadows", &two, -1, ignore_reply, NULL);
            svc_settings_set(setup_ids[0], "translucency", &one, -1, ignore_reply, NULL);
            svc_settings_set(setup_ids[0], "title_layout", &one, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            ui_rect c = ui_frame_client(s);
            double tw = ui_surface_width(s), ty = c.y - ui_scheme_now.title_h / 2;
            fprintf(stderr, "selftest: options: shadows %d, translucency %d, mirrored %d\n", ui_scheme_now.shadows,
                    ui_scheme_now.translucency, ui_scheme_now.title_mirrored);
            fprintf(stderr, "selftest: mirrored frame: left end is %s, right end is %s\n",
                    ui_frame_hit(s, 12, ty) == UI_HIT_CLOSE ? "close" : "something else",
                    ui_frame_hit(s, tw - 12, ty) == UI_HIT_SYSMENU ? "the system menu" : "something else");
            step++;
        } else if (step == 2) {
            size_t d = ui_surface_verify(s, getenv("OSD_VERIFY_PNG"));   /* OSD_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw (inside the shadow): %zu differ\n", d);
            if (getenv("OSD_SHELL_SHOT")) fprintf(stderr, "selftest: buffer saved %d\n", ui_surface_snapshot(s, getenv("OSD_SHELL_SHOT")));
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "gnome")) {
        /* Settings shared with GNOME, both ways (the desktop process). */
        static double t0;
        static int step;
        if (!f->desktop) return;
        if (!t0) { if (!f->m.complete || !setup_cls[3][0]) return; t0 = now; }
        double t = now - t0;
        const char *mouse = NULL, *pal = NULL;
        for (int i = 0; i < 4; i++) {
            if (!strcmp(setup_cls[i], "OsdMouse")) mouse = setup_ids[i];
            if (!strcmp(setup_cls[i], "OsdSchemePalette")) pal = setup_ids[i];
        }
        char v[256];
        #define GGET(schema, key) (snprintf(v, sizeof v, "gsettings get %s %s", schema, key), \
            (void)0, ({ FILE *p_ = popen(v, "r"); v[0] = 0; if (p_) { if (!fgets(v, sizeof v, p_)) v[0] = 0; pclose(p_); } \
            v[strcspn(v, "\n")] = 0; v; }))
        if (step == 0 && t > 1000) {
            fprintf(stderr, "selftest: at start: scheme %s, GNOME color-scheme %s\n", ui_scheme_now.name,
                    GGET("org.gnome.desktop.interface", "color-scheme"));
            if (system("gsettings set org.gnome.desktop.interface color-scheme prefer-dark")) {}
            step++;
            t0 = now;
        } else if (step == 1 && t > 2500) {
            fprintf(stderr, "selftest: GNOME to dark: Objectspace scheme %s\n", ui_scheme_now.name);
            cv d = { .k = CV_TEXT, .s = (char *)"650", .len = 3 };
            svc_settings_set(mouse, "double_click_ms", &d, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 2 && t > 2500) {
            fprintf(stderr, "selftest: Objectspace double-click 650: GNOME %s, the toolkit %d\n",
                    GGET("org.gnome.desktop.peripherals.mouse", "double-click"), ui_mouse.double_click_ms);
            if (system("gsettings set org.gnome.desktop.interface font-name 'DejaVu Sans 12'")) {}
            step++;
            t0 = now;
        } else if (step == 3 && t > 2500) {
            fprintf(stderr, "selftest: GNOME font DejaVu Sans 12: Objectspace font %.0f px\n", ui_scheme_now.font_px);
            cv hc = { .k = CV_TEXT, .s = (char *)"high contrast", .len = 13 };
            svc_settings_set(pal, "scheme", &hc, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 4 && t > 2500) {
            fprintf(stderr, "selftest: Objectspace high contrast: GNOME high-contrast %s; Objectspace scheme %s\n",
                    GGET("org.gnome.desktop.a11y.interface", "high-contrast"), ui_scheme_now.name);
            ui_app_quit(app);
            step++;
        }
        #undef GGET
        return;
    }
    if (st && !strcmp(st, "lpfill")) {
        /* Three objects in the first Launcher drawer, then nothing (for
         * PROBE_MODE=drawer to click). */
        static bool done;
        if (!f->desktop || done || launcher_count() < 2) return;
        done = true;
        const char *names[] = { "notes.txt", "Projects", "added-later.txt" };
        for (int k = 0; k < 3; k++) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", f->path, names[k]);
            if (access(p, F_OK) == 0) launcher_drawer(0, p);
        }
        fprintf(stderr, "selftest: first drawer filled\n");
        return;
    }
    if (st && !strcmp(st, "launcher2")) {
        /* The Launcher in 0.3 (step 10): vertical, small icons, no text;
         * a drawer open follows its folder. <D> has notes.txt. */
        static double t0;
        static int step;
        static size_t before;
        if (!f->desktop) return;
        if (!t0) { if (launcher_count() < 3) return; t0 = now; }
        double t = now - t0;
        if (step == 0) step = 1;
        if (step == 1 && t > 500) {
            /* (its tooltip: tests/comp/launcher.sh, where the pointer is the test's) */
            double w0 = launcher_width(), h0 = launcher_height();
            launcher_setting("orientation", "vertical");
            double w1 = launcher_width(), h1 = launcher_height();
            launcher_setting("text", "0");
            launcher_setting("small_icons", "1");
            double w2 = launcher_width(), h2 = launcher_height();
            fprintf(stderr, "selftest: launcher2: horizontal %.0fx%.0f, vertical %.0fx%.0f (taller %d), small without text %.0fx%.0f (smaller %d)\n",
                    w0, h0, w1, h1, h1 > w1 && w0 > h0, w2, h2, w2 < w1 && h2 < h1);
            launcher_drawer_open(0);
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            before = launcher_drawer_count();
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/notes.txt", f->path);
            launcher_drawer(0, p);   /* (as a drop on its arrow: a shadow in its folder) */
            step++;
            t0 = now;
        } else if (step == 3 && t > 2000) {
            fprintf(stderr, "selftest: launcher2: drawer open: %zu, then %zu (followed)\n", before, launcher_drawer_count());
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "launcher")) {
        /* The Launcher on the desktop: its first buttons, a button
         * pressed (a folder opens), a drop on it and on a drawer arrow. */
        static double t0;
        static int step;
        if (!f->desktop) return;
        if (!t0) { if (launcher_count() < 3) return; t0 = now; }
        double t = now - t0;
        char rows[600] = "";
        #define LROWS() do { rows[0] = 0; for (size_t i_ = 0; launcher_title(i_); i_++) \
            snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s'%s'", i_ ? ", " : "", launcher_title(i_)); } while (0)
        if (step == 0 && t > 500) {
            LROWS();
            fprintf(stderr, "selftest: Launcher (%zu): %s\n", launcher_count(), rows);
            char shot[PATH_MAX];
            if (getenv("OSD_SHELL_SHOT")) {
                snprintf(shot, sizeof shot, "%s-launcher.png", getenv("OSD_SHELL_SHOT"));
                launcher_snapshot(shot);
            }
            size_t tb = SIZE_MAX;
            for (size_t i = 0; launcher_title(i); i++) if (!strcmp(launcher_title(i), "templates")) tb = i;
            fprintf(stderr, "selftest: press Templates: %d\n", tb != SIZE_MAX && launcher_press(4 + tb));
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/notes.txt", f->path);
            launcher_add(p);
            launcher_drawer(0, p);
            step++;
            t0 = now;
        } else if (step == 1 && t > 2000) {
            LROWS();
            fprintf(stderr, "selftest: after the drop (%zu): %s\n", launcher_count(), rows);
            int open = 0;
            for (struct folder *o = folders; o; o = o->next) open += !o->closing && !o->desktop && strstr(o->path, "templates");
            fprintf(stderr, "selftest: templates window open: %d\n", open);
            char d[PATH_MAX];
            snprintf(d, sizeof d, "%s Drawers", launcher_dir());
            DIR *dd = opendir(d);
            int drawers = 0;
            struct dirent *de;
            while (dd && (de = readdir(dd))) drawers += de->d_name[0] != '.';
            if (dd) closedir(dd);
            fprintf(stderr, "selftest: drawers made: %d\n", drawers);
            if (getenv("OSD_SHELL_SHOT")) {
                char shot[PATH_MAX];
                snprintf(shot, sizeof shot, "%s-launcher2.png", getenv("OSD_SHELL_SHOT"));
                launcher_snapshot(shot);
            }
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            ui_app_quit(app);
            step++;
        }
        #undef LROWS
        return;
    }
    if (st && !strcmp(st, "folderscroll")) {
        /* A folder's icon view dragged down in small steps (as the
         * picker's: the same container), checked against a fresh redraw. */
        static double t0, tv;
        static int step;
        static double at;
        static size_t checks, bad;
        if (f != folders || f->closing || !f->m.complete) return;
        if (!t0) { t0 = now; tv = now; return; }
        if (step == 0 && now - t0 > 800) {
            fprintf(stderr, "selftest: folderscroll: %s\n", f->m.total > 300 ? "many objects" : "few objects");
            step++; tv = now;
        } else if (step == 1) {
            if (now - tv < 60) {
                int moves = getenv("PICK_MOVES") ? atoi(getenv("PICK_MOVES")) : 1;
                double stepf = getenv("PICK_STEP") ? atof(getenv("PICK_STEP")) : 0.002;
                for (int k = 0; k < moves; k++) { at += stepf; ui_container_scroll_to_fraction(f->cont, at); }
                return;
            }
            size_t d = ui_surface_verify(f->win, bad ? NULL : getenv("OSD_VERIFY_PNG"));
            if (d == SIZE_MAX && now - tv < 600) return;
            if (d != SIZE_MAX) { checks++; if (d > 200) bad++; }
            tv = now;
            if (at >= 0.5) {
                fprintf(stderr, "selftest: folderscroll: %s pauses checked, %zu with leftovers\n", checks >= 20 ? "20 or more" : "too few", bad);
                step++;
            }
        } else if (step == 2) { ui_app_quit(app); step++; }
        return;
    }
    if (st && !strcmp(st, "pickscroll")) {
        /* The picker's list dragged down by its thumb (several moves a
         * frame, previews arriving meanwhile): what is shown checked
         * against a fresh redraw after each pause (the owner's: leftovers
         * while dragging). */
        static double t0, tv;
        static int step;
        static double at;
        static size_t worst, checks, bad;
        if (f != folders || f->closing || !f->m.complete) return;
        ui_surface_damage_all(f->win);
        if (!t0) { t0 = now; return; }
        double t = now - t0;
        if (step == 0 && t > 300) { iconpick_open("test", NULL, 0, "icon"); step++; t0 = now; }
        else if (step == 1 && (iconpick_ready() || t > 10000)) {
            fprintf(stderr, "selftest: pickscroll: %s\n", iconpick_count() > 100 ? "many icons" : "few icons");
            step++; t0 = now; tv = now;
        } else if (step == 2) {
            ui_surface *ps = iconpick_surface();
            if (!ps) { step = 9; return; }
            if (now - tv < 60) {   /* a drag: three moves in this frame */
                int moves = getenv("PICK_MOVES") ? atoi(getenv("PICK_MOVES")) : 3;
                double stepf = getenv("PICK_STEP") ? atof(getenv("PICK_STEP")) : 0.0015;
                for (int k = 0; k < moves; k++) { at += stepf; iconpick_scroll(at); }
                return;
            }
            char vp[PATH_MAX];
            snprintf(vp, sizeof vp, "%s-%zu", getenv("OSD_VERIFY_PNG") ? getenv("OSD_VERIFY_PNG") : "", bad);
            size_t d = ui_surface_verify(ps, getenv("PICK_DEBUG") && bad < 6 ? vp : bad ? NULL : getenv("OSD_VERIFY_PNG"));   /* (PICK_DEBUG: the first six saved) */
            if (d == SIZE_MAX && now - tv < 600) return;   /* still being drawn */
            if (d && d != SIZE_MAX && getenv("PICK_DEBUG")) fprintf(stderr, "selftest: pickscroll-debug: at %.4f: %zu differ\n", at, d);
            /* (a few pixels: a title's anti-aliasing where two strips meet) */
            if (d != SIZE_MAX) { checks++; if (d > 200) bad++; if (d > worst) worst = d; }
            tv = now;
            if (at >= 0.3) {
                fprintf(stderr, "selftest: pickscroll: %s pauses checked, %zu with leftovers\n", checks >= 20 ? "20 or more" : "too few", bad);
                if (getenv("PICK_DEBUG")) fprintf(stderr, "selftest: pickscroll-debug: %zu checked, worst %zu pixels\n", checks, worst);
                step = 9;
            }
        } else if (step == 9) { ui_app_quit(app); step++; }
        return;
    }
    if (st && !strcmp(st, "iconlib")) {
        /* The picker's icon library (OSD_ICON_LIBRARY), chosen as a user
         * does: by the button, then back to the themes and again. */
        static double t0;
        static int step;
        if (f != folders || f->closing || !f->m.complete) return;
        ui_surface_damage_all(f->win);
        if (!t0) { t0 = now; return; }
        double t = now - t0;
        if (step == 0 && t > 300) { iconpick_open("test", NULL, 0, "icon"); step++; t0 = now; }
        else if (step == 1 && t > 500) { iconpick_source(true); step++; t0 = now; }
        else if (step == 2 && t > 1500) {
            fprintf(stderr, "selftest: library: %zu icons\n", iconpick_count());
            if (getenv("OSD_SHELL_SHOT")) iconpick_snapshot(getenv("OSD_SHELL_SHOT"));
            iconpick_source(false); step++; t0 = now;
        } else if (step == 3 && t > 1500) { iconpick_source(true); step++; t0 = now; }
        else if (step == 4 && t > 1500) {
            fprintf(stderr, "selftest: library again: %zu icons\n", iconpick_count());
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "icons")) {
        /* An object's own icon (G4): Properties' Icon page with an image
         * dropped on it, then one chosen in the picker, then Default. */
        static double t0;
        static int step;
        static char target[PATH_MAX];
        if (f != folders || f->closing || !f->m.complete) return;
        ui_surface_damage_all(f->win);   /* frames keep coming */
        if (!t0) { t0 = now; snprintf(target, sizeof target, "%s/a.txt", f->path); return; }
        double t = now - t0;
        struct props *p = NULL;
        for (struct props *q = props_open; q; q = q->next) if (!strcmp(q->target, target)) p = q;
        ssize_t row = model_find(&f->m, "a.txt");
        const char *icon = row >= 0 && row_ptr(&f->m, (size_t)row)->icon ? row_ptr(&f->m, (size_t)row)->icon : "";
        char shot[PATH_MAX];
        const char *sh = getenv("OSD_SHELL_SHOT");
        if (step == 0 && t > 500) { open_properties(target); step++; t0 = now; }
        else if (step == 1 && p && p->nb && t > 800) {
            ui_value v = { .text = "/usr/share/icons/Adwaita/scalable/places/folder-music.svg" };   /* as dropped */
            props_set(p, "icon", UI_FIELD_ICON, &v);
            step++; t0 = now;
        } else if (step == 2 && (*icon || t > 6000)) {
            fprintf(stderr, "selftest: dropped image: icon %s, drawn %d\n", *icon ? strrchr(icon, '/') + 1 : "(none)", icons_get(icon) != NULL);
            ui_notebook_show_page(p->nb, 1);   /* the Icon page, for its picture */
            ui_value v = { .text = NULL };   /* Choose... */
            props_set(p, "icon", UI_FIELD_ICON, &v);
            step++; t0 = now;
        } else if (step == 3 && (iconpick_ready() || t > 10000)) {
            fprintf(stderr, "selftest: picker: %zu icons in the themes\n", iconpick_count());
            iconpick_search("folder");
            fprintf(stderr, "selftest: picker, \"folder\": %zu, first '%s'\n", iconpick_count(), iconpick_name(0) ? iconpick_name(0) : "");
            if (sh && p->nb) { snprintf(shot, sizeof shot, "%s-notebook.png", sh); ui_surface_snapshot(ui_notebook_surface(p->nb), shot); }
            if (sh) { snprintf(shot, sizeof shot, "%s-folder.png", sh); ui_surface_snapshot(f->win, shot); }
            step++; t0 = now;
        } else if (step == 4 && t > 1500) {
            if (sh) { snprintf(shot, sizeof shot, "%s-picker.png", sh); iconpick_snapshot(shot); }
            iconpick_choose(0);
            step++; t0 = now;
        } else if (step == 5 && (t > 6000 || (*icon && !strstr(icon, "folder-music")))) {
            fprintf(stderr, "selftest: chosen in the picker: icon %s\n", *icon ? strrchr(icon, '/') + 1 : "(none)");
            ui_value v = { .text = "" };   /* Default */
            props_set(p, "icon", UI_FIELD_ICON, &v);
            step++; t0 = now;
        } else if (step == 6 && (!*icon || t > 4000)) {
            fprintf(stderr, "selftest: after Default: icon %s\n", *icon ? icon : "(its kind's)");
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "a11y")) {
        /* For a screen reader (tests/gui/a11y.py listens): the cursor
         * through the folder by keys, then a dialog's buttons. */
        static double t0;
        static int step;
        static ui_dialog *d;
        if (f != folders || f->closing || !f->m.complete) return;
        if (!t0) { t0 = now; return; }
        double t = now - t0;
        ui_surface_damage_all(f->win);   /* frames keep coming: this runs on each */
        ui_key_event tab = { .sym = XKB_KEY_Tab };
        if (step < 3 && t > 2500 + step * 500.0) {
            ui_key_event k = { .sym = step == 0 ? XKB_KEY_End : step == 1 ? XKB_KEY_Home : XKB_KEY_Right };
            ui_container_key(f->cont, &k);
            step++;
        }
        else if (step == 3 && t > 4500) {
            static const char *const b[] = { "Delete", "Cancel" };
            d = ui_dialog_open(app, "Delete", "Delete gamma?", b, 2, 1, NULL, NULL);
            step++;
        } else if (step == 4 && t > 6000) {
            ui_dialog_key(d, &tab);
            step++;
        } else if (step == 5 && t > 6800) {
            findwin_open(f->path, NULL);   /* its fields: Name, then Tab: Folder, Include subfolders */
            step++;
        } else if (step >= 6 && step <= 7 && t > 7800 + (step - 6) * 500.0) {
            findwin_key(XKB_KEY_Tab);
            step++;
        } else if (step == 8 && t > 8800) {
            findwin_key(XKB_KEY_space);   /* the check box, unticked */
            step++;
        } else if (step == 9 && t > 11000) {
            ui_app_quit(app);
        }
        return;
    }
    if (st && !strcmp(st, "typeahead")) {
        /* Keys typed in a folder: the item they spell is the cursor's. */
        static double t0;
        if (f != folders || f->closing || !f->m.complete) return;
        if (!t0) { t0 = now; return; }
        if (now - t0 < 500) return;
        static const char *const typed[] = { "b", "b", "b", "ca", "CAT", "x" };
        for (size_t k = 0; k < sizeof typed / sizeof *typed; k++) {
            for (const char *p = typed[k]; *p; p++) {
                ui_key_event e = { .sym = (uint32_t)(unsigned char)*p };
                e.utf8[0] = *p;
                ui_container_key(f->cont, &e);
            }
            size_t c = ui_container_cursor(f->cont);
            ui_item it = { 0 };
            if (c != SIZE_MAX) src_item(f, c, &it);
            fprintf(stderr, "selftest: typed '%s': %s\n", typed[k], it.title ? it.title : "(none)");
            /* the next word: as after a pause */
            if (k != 0 && k != 1) ui_container_typeahead_reset(f->cont);   /* b, b, b: one after another */
        }
        ui_app_quit(app);
        return;
    }
    if (st && !strcmp(st, "find")) {
        /* Find (G4 search) on the folder: typed as a user types (each key
         * a new search), then a pattern, then folders only. */
        static double t0;
        static int step;
        if (f != folders || f->closing) return;
        if (!t0) t0 = now;
        double t = now - t0;
        static const char *const typed[] = { "note", "*.TXT", "a" };
        if (step == 0 && t > 500) {
            findwin_open(f->path, NULL);
            findwin_type(typed[0]);
            step++;
            t0 = now;
        } else if (step == 11 && t > 400) {
            if (getenv("OSD_SHELL_SHOT")) {
                char shot[PATH_MAX];
                snprintf(shot, sizeof shot, "%s-preedit.png", getenv("OSD_SHELL_SHOT"));
                findwin_snapshot(shot);
            }
            ui_text_input_inject(app, "日本", NULL);
            step = 10;
            t0 = now;
        } else if (step == 10 && (t > 5000 || (!findwin_searching() && t > 300))) {
            fprintf(stderr, "selftest: find by input method: %zu: %s\n", findwin_count(), findwin_result(0) ? strrchr(findwin_result(0), '/') + 1 : "");
            ui_app_quit(app);
            return;
        } else if (step >= 1 && step <= 3 && (t > 5000 || (!findwin_searching() && t > 300))) {
            char rows[1200] = "";
            size_t pl = strlen(f->path);
            for (size_t i = 0; findwin_result(i); i++)
                snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s'%s'", i ? ", " : "", findwin_result(i) + pl);
            fprintf(stderr, "selftest: find '%s'%s: %zu: %s\n", typed[step - 1], step == 3 ? " (folders)" : "", findwin_count(), rows);
            if (step == 2 && getenv("OSD_SHELL_SHOT")) findwin_snapshot(getenv("OSD_SHELL_SHOT"));
            if (step == 1) findwin_type(typed[1]);
            else if (step == 3) {
                /* an input method: 日本 composed (preedit), then committed */
                findwin_kind(0);
                findwin_type("");
                ui_text_input_inject(app, NULL, "にほん");
                step = 11;   /* drawn, then committed */
                t0 = now;
                return;
            }
            else if (step == 2) {
                /* Type: Folders (the type field, as Space there does), then the name */
                findwin_kind(1);
                findwin_type(typed[2]);
            }
            step++;
            t0 = now;
        }
        return;
    }
    if (st && !strcmp(st, "winlist")) {
        /* The Window List and the Minimized Window Viewer, with the
         * extension: the folder window run.sh opens, minimized and restored. */
        static double t0;
        static int step;
        if (!f->desktop) return;
        if (!t0) { if (!winlist_count(false)) return; t0 = now; }
        double t = now - t0;
        char rows[600] = "";
        #define ROWS(minimized) do { rows[0] = 0; for (size_t i_ = 0; winlist_title(i_, minimized); i_++) \
            snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s'%s'", i_ ? ", " : "", winlist_title(i_, minimized)); } while (0)
        if (step == 0 && t > 1000) {
            winlist_open(false);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1000) {
            ROWS(false);
            fprintf(stderr, "selftest: Window List (%zu): %s\n", winlist_count(false), rows);
            char shot[PATH_MAX];
            if (getenv("OSD_SHELL_SHOT")) {
                snprintf(shot, sizeof shot, "%s-list.png", getenv("OSD_SHELL_SHOT"));
                winlist_snapshot(false, shot);
            }
            fprintf(stderr, "selftest: minimize Projects: sent %d\n", winlist_command("minimize", "Projects"));
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            winlist_open(true);
            step++;
            t0 = now;
        } else if (step == 3 && t > 1000) {
            ROWS(true);
            fprintf(stderr, "selftest: Minimized Window Viewer (%zu): %s\n", winlist_count(true), rows);
            char shot[PATH_MAX];
            if (getenv("OSD_SHELL_SHOT")) {
                snprintf(shot, sizeof shot, "%s-minimized.png", getenv("OSD_SHELL_SHOT"));
                winlist_snapshot(true, shot);
            }
            fprintf(stderr, "selftest: restore it: sent %d\n", winlist_command("activate", "Projects"));
            step++;
            t0 = now;
        } else if (step == 4 && t > 1500) {
            fprintf(stderr, "selftest: after restoring: %zu minimized\n", winlist_count(true));
            fprintf(stderr, "selftest: maximize Projects: sent %d\n", winlist_command("maximize", "Projects"));
            step++;
            t0 = now;
        } else if (step == 5 && t > 1500) {
            bool mn = false, mx = false;
            winlist_state("Projects", &mn, &mx);
            fprintf(stderr, "selftest: after maximizing: minimized %d maximized %d\n", mn, mx);
            fprintf(stderr, "selftest: restore Projects: sent %d\n", winlist_command("restore", "Projects"));
            step++;
            t0 = now;
        } else if (step == 6 && t > 1500) {
            bool mn = false, mx = false;
            winlist_state("Projects", &mn, &mx);
            fprintf(stderr, "selftest: after restoring: minimized %d maximized %d\n", mn, mx);
            /* another program (run.sh APP=), ended unasked */
            const char *other = getenv("OSD_SELFTEST_TERMINATE");
            if (other && !*other) other = NULL;
            if (other) fprintf(stderr, "selftest: terminate %s: sent %d\n", other, winlist_command("terminate", other));
            step++;
            t0 = now;
        } else if (step == 7 && t > 2000) {
            const char *other = getenv("OSD_SELFTEST_TERMINATE");
            if (other && !*other) other = NULL;
            bool mn, mx;
            if (other) fprintf(stderr, "selftest: after terminating: %s %s\n", other, winlist_state(other, &mn, &mx) ? "still there" : "gone");
            ui_app_quit(app);
            step++;
        }
        #undef ROWS
        return;
    }
    if (st && !strcmp(st, "group")) {
        /* A work area's group inside GNOME (0.4 step 10): the extension's
         * group command finds a program's window by its process - here a
         * shell that started the test client, so by its parent - and
         * minimizes, restores and closes it. */
        static double t0;
        static int step;
        static pid_t pid;
        static char ids[32];
        if (!f->desktop) return;
        if (!t0) { t0 = now; return; }
        double t = now - t0;
        bool mn = false, mx = false;
        char line[96];
        if (step == 0 && t > 1500) {
            char exe[PATH_MAX], client[PATH_MAX + 32];
            ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
            exe[n > 0 ? n : 0] = 0;
            char *sl = strrchr(exe, '/');
            if (sl) *sl = 0;
            snprintf(client, sizeof client, "%s/tests/ssd-client Grouped 3060a0; true", exe);   /* (a child of the shell, not it) */
            const char *disp = getenv("OSD_SELFTEST_DISPLAY");
            char env_disp[128];
            snprintf(env_disp, sizeof env_disp, "WAYLAND_DISPLAY=%s", disp && *disp ? disp : "osd-hl");
            char *argv[] = { "env", env_disp, "sh", "-c", client, NULL };
            extern char **environ;
            if (posix_spawnp(&pid, "env", NULL, NULL, argv, environ) != 0) pid = 0;
            snprintf(ids, sizeof ids, "%d", (int)pid);
            fprintf(stderr, "selftest: group: a program started %d\n", pid > 0);
            step++;
            t0 = now;
        } else if (step == 1 && t > 3000) {
            fprintf(stderr, "selftest: group: its window %s\n", winlist_state("Grouped", &mn, &mx) ? "shown" : "missing");
            snprintf(line, sizeof line, "minimize\t%s\t", ids);
            winlist_ext("group", line);
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            winlist_state("Grouped", &mn, &mx);
            fprintf(stderr, "selftest: group: minimized with it %d\n", mn);
            snprintf(line, sizeof line, "restore\t%s\t", ids);
            winlist_ext("group", line);
            step++;
            t0 = now;
        } else if (step == 3 && t > 1500) {
            winlist_state("Grouped", &mn, &mx);
            fprintf(stderr, "selftest: group: restored with it %d\n", !mn);
            snprintf(line, sizeof line, "close\t%s\t", ids);
            winlist_ext("group", line);
            step++;
            t0 = now;
        } else if (step == 4 && t > 2000) {
            fprintf(stderr, "selftest: group: closed with it %d\n", !winlist_state("Grouped", &mn, &mx));
            if (pid > 0) waitpid(pid, NULL, WNOHANG);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "live")) { selftest_live(f, now); return; }
    if (st && !strcmp(st, "close")) {
        if (f == folders) {   /* the first window closes itself */
            selftest_close(s, now);
            if (f->closing) ui_app_post(app, selftest_after_close, NULL);
        }
        return;
    }
    if (st) { selftest(s, now); return; }
    if (f->bench_secs <= 0) return;
    if (f->bench_start == 0) {
        if (!f->m.total) return;
        size_t n;
        const ui_frame_stat *st;
        ui_surface_stats(s, &st, &n);
        f->bench_start = now;
        f->bench_next_jump = now + 3000;
        f->bench_from = n;
    }
    /* Every 30 frames, check the picture against a full redraw. */
    if (getenv("OSD_SHELL_VERIFY") && ++f->frames_seen % 30 == 0) {
        size_t d = ui_surface_verify(s, f->verify_failed ? NULL : getenv("OSD_SHELL_VERIFY"));
        if (d != SIZE_MAX) {
            f->verified++;
            if (d) { f->verify_failed++; f->verify_diff += d; }
        }
    }
    if (now - f->bench_start > f->bench_secs * 1000) {
        report(f);
        f->bench_secs = 0;
        ui_surface_set_continuous(s, false);
        ui_app_quit(app);
        return;
    }
    if (now < f->bench_next_jump) ui_container_scroll_by(f->cont, 60);
    else {
        ui_container_scroll_to_fraction(f->cont, (double)(rand_r(&f->bench_seed) % 1000000) / 1e6);
        f->bench_next_jump = now + 500;
    }
}
