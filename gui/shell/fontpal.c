/* The Font Palette (fontpal.h). */
#define _GNU_SOURCE
#include "fontpal.h"

#include <fontconfig/fontconfig.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "events.h"

#define COLS 3
#define CELL_W 170
#define CELL_H 48
#define M 8
#define BTN_W (110 * ui_dialog_wider())
#define ROW (24 + ui_dialog_grow())
#define STATUS_H (20 + ui_dialog_grow())
#define MAX_SAMPLES 24

static ui_app *app;
static fontpal_apply_fn apply;

static char samples[MAX_SAMPLES][128];
static size_t nsamples;

static struct pal {
    ui_surface *s;
    size_t sel;
    bool closing;
    /* a press on a sample: a drag once it moves, a menu if released (button 2) */
    bool armed, popup_armed;
    size_t press_i;
    double px, py;
    uint32_t serial;
    char drag[128];
} pal;

/* ---------------------------------------------------------- the samples */

void fontpal_set_samples(const char *list)
{
    nsamples = 0;
    char copy[4096];
    snprintf(copy, sizeof copy, "%s", list ? list : "");
    for (char *q = copy, *t; nsamples < MAX_SAMPLES && (t = strsep(&q, ";"));)
        if (*t && ui_font_valid(t)) snprintf(samples[nsamples++], sizeof samples[0], "%s", t);
    if (pal.sel >= nsamples) pal.sel = 0;
    if (pal.s && !pal.closing) ui_surface_damage_all(pal.s);
}

static void save_samples(void)
{
    char list[4096] = "";
    for (size_t i = 0; i < nsamples; i++)
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ";" : "", samples[i]);
    if (apply) apply("samples", list);
}

/* ------------------------------------------------------- the Edit Font dialog */

#define EF_W 460
#define EF_H 380
#define EF_LIST_W 240
#define MAX_STYLES 32
static struct ef {
    ui_surface *s;
    ui_container *list, *style_list;
    char **fam;
    size_t nfam;
    size_t sample;
    ui_entry *size;
    char styles[MAX_STYLES][64];   /* the family's (fontconfig's), by weight */
    size_t nstyles;
    size_t styles_of;          /* the family they are for */
    char style[64];            /* the one chosen ("Regular", "SemiBold", ...) */
    int focus;                 /* 0 list, 1 size, 2 style, 3 OK, 4 Cancel */
    int pressed;               /* a button held: 3 or 4 */
    bool closing;
} ef;
enum { EF_FAMILY, EF_SIZE, EF_STYLE, EF_OK, EF_CANCEL, EF_NFOCUS };

static int cmp_str(const void *a, const void *b) { return strcoll(*(char *const *)a, *(char *const *)b); }

static void read_families(void)
{
    if (ef.fam) return;
    FcPattern *p = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, NULL);
    FcFontSet *fs = FcFontList(NULL, p, os);
    for (int i = 0; fs && i < fs->nfont; i++) {
        FcChar8 *f;
        if (FcPatternGetString(fs->fonts[i], FC_FAMILY, 0, &f) != FcResultMatch) continue;
        bool dup = false;
        for (size_t k = 0; k < ef.nfam && !dup; k++) dup = !strcmp(ef.fam[k], (const char *)f);
        if (dup) continue;
        ef.fam = realloc(ef.fam, (ef.nfam + 1) * sizeof *ef.fam);
        ef.fam[ef.nfam++] = strdup((const char *)f);
    }
    /* the generic names first: they follow the system's choice */
    qsort(ef.fam, ef.nfam, sizeof *ef.fam, cmp_str);
    static const char *const generic[] = { "Monospace", "Serif", "Sans" };
    for (size_t g = 0; g < 3; g++) {
        ef.fam = realloc(ef.fam, (ef.nfam + 1) * sizeof *ef.fam);
        memmove(ef.fam + 1, ef.fam, ef.nfam * sizeof *ef.fam);
        ef.fam[0] = strdup(generic[g]);
        ef.nfam++;
    }
    if (fs) FcFontSetDestroy(fs);
    FcObjectSetDestroy(os);
    FcPatternDestroy(p);
}

static size_t ef_count(void *ctx) { (void)ctx; return ef.nfam; }
static void ef_item(void *ctx, size_t i, ui_item *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (i >= ef.nfam) return;
    out->loaded = out->detailed = true;
    out->kind = UI_KIND_FILE;
    out->title = out->name = ef.fam[i];
}
static void ef_close(void);
static void ef_ok(void);
static void ef_activate(void *ctx, size_t i) { (void)ctx; (void)i; ef_ok(); }
static const ui_container_src ef_src = { ef_count, ef_item, NULL, ef_activate, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

static size_t st_count(void *ctx) { (void)ctx; return ef.nstyles; }
static void st_item(void *ctx, size_t i, ui_item *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (i >= ef.nstyles) return;
    out->loaded = out->detailed = true;
    out->kind = UI_KIND_FILE;
    out->title = out->name = ef.styles[i];
}
static const ui_container_src st_src = { st_count, st_item, NULL, ef_activate, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

static const char *family_now(void)
{
    size_t i = ef.list ? ui_container_cursor(ef.list) : SIZE_MAX;
    return i < ef.nfam ? ef.fam[i] : "Sans";
}

/* The family's styles, as fontconfig has them (Edit font: OS/2's style
 * list); a name fontconfig only matches ("Sans", "Serif"), or a family
 * listed with none: the four it can make. The chosen style kept if the
 * new family has it, else Regular (or its first). */
static void sync_styles(void)
{
    size_t fi = ef.list ? ui_container_cursor(ef.list) : SIZE_MAX;
    if (fi == ef.styles_of && ef.nstyles) return;
    ef.styles_of = fi;
    ef.nstyles = ui_font_styles(family_now(), ef.styles, MAX_STYLES);
    if (!ef.nstyles) {
        static const char *const made[] = { "Regular", "Bold", "Italic", "Bold Italic" };
        for (size_t k = 0; k < 4; k++) snprintf(ef.styles[k], sizeof ef.styles[k], "%s", made[k]);
        ef.nstyles = 4;
    }
    size_t at = SIZE_MAX, regular = SIZE_MAX;
    for (size_t k = 0; k < ef.nstyles; k++) {
        if (!strcasecmp(ef.styles[k], ef.style)) at = k;
        if (!strcasecmp(ef.styles[k], "Regular") || !strcasecmp(ef.styles[k], "Book") || !strcasecmp(ef.styles[k], "Normal"))
            if (regular == SIZE_MAX) regular = k;
    }
    if (at == SIZE_MAX) at = regular != SIZE_MAX ? regular : 0;
    snprintf(ef.style, sizeof ef.style, "%s", ef.styles[at]);
    if (ef.style_list) {
        ui_container_reset(ef.style_list);   /* (another count) */
        ui_container_set_cursor(ef.style_list, at);
    }
}

static void style_from_list(void)
{
    size_t k = ef.style_list ? ui_container_cursor(ef.style_list) : SIZE_MAX;
    if (k < ef.nstyles) snprintf(ef.style, sizeof ef.style, "%s", ef.styles[k]);
}

struct ef_rects { ui_rect list, size_l, size, style_l, style, preview, ok, cancel; };
static struct ef_rects ef_rects(void)
{
    ui_rect c = ui_frame_client(ef.s);
    struct ef_rects r;
    double bx = c.x + M + EF_LIST_W + 12, by = c.y + M, rw = c.x + c.w - M - bx;
    r.list = (ui_rect){ c.x, c.y, EF_LIST_W + M, c.h - ROW - 2 * M };
    r.size_l = (ui_rect){ bx, by, 60, ROW };
    r.size = (ui_rect){ bx + 64, by, 60, ROW };
    r.style_l = (ui_rect){ bx, by + ROW + 6, 60, ROW };
    double sy = r.style_l.y + ROW, py = c.y + c.h - M - ROW - 10 - 90;   /* the list: down to the preview */
    r.style = (ui_rect){ bx, sy, rw, fmax(ROW * 2, py - 8 - sy) };
    r.preview = (ui_rect){ bx, py, rw, 90 };
    r.ok = (ui_rect){ c.x + c.w - M - 2 * 90 - 8, c.y + c.h - M - ROW, 90, ROW };
    r.cancel = (ui_rect){ c.x + c.w - M - 90, c.y + c.h - M - ROW, 90, ROW };
    return r;
}

/* "16.Serif Bold", "10.Noto Sans SemiBold": the family, then its style
 * (none for Regular). */
static void spec_now(char *out, size_t n)
{
    int pt = atoi(ui_entry_text(ef.size));
    if (pt < 4) pt = 4;
    if (pt > 72) pt = 72;
    bool plain = !ef.style[0] || !strcasecmp(ef.style, "Regular");
    snprintf(out, n, "%d.%s%s%s", pt, family_now(), plain ? "" : " ", plain ? "" : ef.style);
}

static void ring(cairo_t *cr, ui_rect r)
{
    ui_set_color(cr, ui_scheme_now.cursor);
    double dash = 1;
    cairo_save(cr);
    cairo_set_dash(cr, &dash, 1, 0);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, r.x + 2.5, r.y + 2.5, r.w - 5, r.h - 5);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void button(cairo_t *cr, ui_rect r, const char *label, bool down, bool focus)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_fill(cr, r, k->face);
    ui_bevel(cr, r, !down);
    ui_set_color(cr, k->text);
    double tw = ui_text_width(UI_FONT_DIALOG, label);
    ui_text_draw(cr, UI_FONT_DIALOG, r.x + (r.w - tw) / 2 + down, r.y + (r.h + ui_font_ascent(UI_FONT_DIALOG)) / 2 - 1 + down, label, r.w - 6);
    if (focus) ring(cr, r);
}

static void ef_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)s;
    const ui_scheme *k = &ui_scheme_now;
    ui_frame_draw(ef.s, cr, clip, "Edit Font");
    ui_rect c = ui_frame_client(ef.s);
    struct ef_rects r = ef_rects();
    ui_fill(cr, (ui_rect){ r.list.x + r.list.w, c.y, c.w - r.list.w, c.h }, k->face);
    ui_fill(cr, (ui_rect){ c.x, r.list.y + r.list.h, r.list.w, c.h - r.list.h }, k->face);
    sync_styles();
    ui_container_draw(ef.list, cr, clip);
    ui_set_color(cr, k->text);
    ui_text_draw(cr, UI_FONT_DIALOG, r.size_l.x, r.size_l.y + (ROW + ui_font_ascent(UI_FONT_DIALOG)) / 2 - 1, "Size", r.size_l.w);
    ui_entry_draw(ef.size, cr, r.size, ef.focus == EF_SIZE);
    ui_set_color(cr, k->text);
    ui_text_draw(cr, UI_FONT_DIALOG, r.style_l.x, r.style_l.y + (ROW + ui_font_ascent(UI_FONT_DIALOG)) / 2 - 1, "Style", r.style_l.w);
    ui_container_draw(ef.style_list, cr, clip);
    /* the sample in the font as it is now */
    ui_fill(cr, r.preview, k->client_bg);
    ui_bevel(cr, r.preview, false);
    char spec[200];
    spec_now(spec, sizeof spec);
    ui_font f = ui_font_named(spec);
    cairo_save(cr);
    cairo_rectangle(cr, r.preview.x + 2, r.preview.y + 2, r.preview.w - 4, r.preview.h - 4);
    cairo_clip(cr);
    ui_set_color(cr, k->text);
    ui_text_draw(cr, f, r.preview.x + 8, r.preview.y + 8 + ui_font_ascent(f), spec, r.preview.w - 16);
    ui_text_draw(cr, f, r.preview.x + 8, r.preview.y + 12 + ui_font_line_h(f) + ui_font_ascent(f), "AaBbCc 가나다 0123", r.preview.w - 16);
    cairo_restore(cr);
    button(cr, r.ok, "OK", ef.pressed == EF_OK, ef.focus == EF_OK);
    button(cr, r.cancel, "Cancel", ef.pressed == EF_CANCEL, ef.focus == EF_CANCEL);
}

static void ef_set_focus(int f)
{
    ef.focus = f;
    ui_container_set_focused(ef.list, f == EF_FAMILY);
    ui_container_set_focused(ef.style_list, f == EF_STYLE);
    ui_text_input_focus(ef.s, f == EF_SIZE ? ef.size : NULL, ef_rects().size);
    if (f == EF_SIZE) ui_entry_select_all(ef.size);
    ui_surface_damage_all(ef.s);
}

static void ef_ok(void)
{
    if (!ef.s || ef.closing || ef.sample >= nsamples) return;
    sync_styles();
    style_from_list();
    char spec[200];
    spec_now(spec, sizeof spec);
    snprintf(samples[ef.sample], sizeof samples[0], "%s", spec);
    save_samples();
    if (pal.s && !pal.closing) ui_surface_damage_all(pal.s);
    ef_close();
}

static void ef_pointer(ui_surface *s, const ui_pointer_event *e)
{
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    bool used = false;
    if (e->kind != UI_PRESS || h != UI_HIT_CLIENT) used = ui_frame_pointer(s, e);
    struct ef_rects r = ef_rects();
    if (e->kind == UI_PRESS && !used && h == UI_HIT_CLIENT) {
        if (ui_rect_has(r.list, e->x, e->y)) {
            if (ef.focus != EF_FAMILY) ef_set_focus(EF_FAMILY);
            ui_container_pointer(ef.list, e);
            sync_styles();
            ui_surface_damage_all(s);   /* (the styles and the preview follow the family) */
            return;
        }
        if (ui_rect_has(r.style, e->x, e->y)) {
            if (ef.focus != EF_STYLE) ef_set_focus(EF_STYLE);
            ui_container_pointer(ef.style_list, e);
            style_from_list();
            ui_surface_damage_all(s);
            return;
        }
        if (e->button != UI_BTN1) return;
        if (ui_rect_has(r.size, e->x, e->y)) { ef_set_focus(EF_SIZE); ui_entry_pointer(ef.size, e, r.size); return; }
        if (ui_rect_has(r.ok, e->x, e->y)) { ef.pressed = EF_OK; ef_set_focus(EF_OK); return; }
        if (ui_rect_has(r.cancel, e->x, e->y)) { ef.pressed = EF_CANCEL; ef_set_focus(EF_CANCEL); return; }
        return;
    }
    if (e->kind == UI_RELEASE && ef.pressed) {
        int p = ef.pressed;
        ef.pressed = 0;
        ui_surface_damage_all(s);
        if (p == EF_OK && ui_rect_has(r.ok, e->x, e->y)) ef_ok();
        else if (p == EF_CANCEL && ui_rect_has(r.cancel, e->x, e->y)) ef_close();
        return;
    }
    if (!used || e->kind == UI_RELEASE || e->kind == UI_MOTION) {
        ui_container_pointer(ef.list, e);
        ui_container_pointer(ef.style_list, e);
    }
}

static void ef_key(ui_surface *s, const ui_key_event *e)
{
    if (e->sym == XKB_KEY_Escape) { ef_close(); return; }
    if (e->sym == XKB_KEY_Tab || e->sym == XKB_KEY_ISO_Left_Tab) {
        bool back = e->sym == XKB_KEY_ISO_Left_Tab || (e->mods & UI_MOD_SHIFT);
        ef_set_focus((ef.focus + (back ? EF_NFOCUS - 1 : 1)) % EF_NFOCUS);
        return;
    }
    bool enter = e->sym == XKB_KEY_Return || e->sym == XKB_KEY_KP_Enter;
    switch (ef.focus) {
    case EF_FAMILY: if (enter) ef_ok(); else { ui_container_key(ef.list, e); sync_styles(); ui_surface_damage_all(s); } return;
    case EF_SIZE: if (enter) ef_ok(); else if (ui_entry_key(ef.size, e) != UI_ENTRY_NONE) ui_surface_damage_all(s); return;
    case EF_STYLE: if (enter) ef_ok(); else { ui_container_key(ef.style_list, e); style_from_list(); ui_surface_damage_all(s); } return;
    case EF_OK: if (enter || e->sym == XKB_KEY_space) ef_ok(); return;
    default: if (enter || e->sym == XKB_KEY_space) ef_close(); return;
    }
}

static void ef_free_later(void *arg)
{
    (void)arg;
    ui_container_free(ef.list);
    ui_container_free(ef.style_list);
    ui_entry_free(ef.size);
    ui_surface_destroy(ef.s);
    ef.s = NULL;
    ef.list = ef.style_list = NULL;
    ef.size = NULL;
    ef.closing = false;
}

static void ef_close(void)
{
    if (!ef.s || ef.closing) return;
    ef.closing = true;
    ui_app_post(app, ef_free_later, NULL);
}

static void ef_close_req(ui_surface *s) { (void)s; ef_close(); }
static void ef_resized(ui_surface *s, double w, double h)
{
    (void)w; (void)h;
    struct ef_rects r = ef_rects();
    if (ef.list) ui_container_layout(ef.list, r.list);
    if (ef.style_list) ui_container_layout(ef.style_list, r.style);
    ui_surface_damage_all(s);
}
static void ef_focus_ev(ui_surface *s, bool on) { (void)on; ui_frame_damage(s); }
static const ui_surface_ops ef_ops = { .draw = ef_draw, .pointer = ef_pointer, .key = ef_key, .resized = ef_resized,
                                       .close = ef_close_req, .focus = ef_focus_ev,
                                       .drag_motion = ui_font_drag_motion, .drop = ui_font_drop };   /* a font: the Dialogs font */

/* Is this one of the families listed? (A sample's name split into its
 * family and style, as the toolkit does with fontconfig.) */
static bool has_family(const char *family, const char *style)
{
    if (style) {
        char st[MAX_STYLES][64];
        size_t n = ui_font_styles(family, st, MAX_STYLES);
        for (size_t k = 0; k < n; k++) if (!strcasecmp(st[k], style)) return true;
        return false;
    }
    for (size_t k = 0; k < ef.nfam; k++) if (!strcasecmp(ef.fam[k], family)) return true;
    return false;
}

bool fontpal_edit(size_t i)
{
    if (i >= nsamples) return false;
    read_families();
    if (ef.s && !ef.closing) { ef.sample = i; ui_window_activate(ef.s); return true; }
    ef.sample = i;
    /* the sample's family, size and style */
    char fam[128], style[64];
    bool bold, italic;
    const char *dot = strchr(samples[i], '.');
    int pt = atoi(samples[i]);
    if (!dot || !ui_font_split(dot + 1, has_family, fam, sizeof fam, style, sizeof style, &bold, &italic))
        snprintf(fam, sizeof fam, "Sans"), style[0] = 0, bold = italic = false;
    if (!style[0]) snprintf(style, sizeof style, "%s", bold && italic ? "Bold Italic" : bold ? "Bold" : italic ? "Italic" : "Regular");
    snprintf(ef.style, sizeof ef.style, "%s", style);
    ef.styles_of = SIZE_MAX;
    ef.nstyles = 0;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", pt);
    ef.size = ui_entry_new(ps);
    ef.pressed = 0;
    ef.s = ui_window_new(app, &ef_ops, NULL, "io.github.sungsookhim.Objectspace.EditFont", EF_W, EF_H);
    ui_window_set_title(ef.s, "Edit Font");
    ef.list = ui_container_new(ef.s, &ef_src, NULL, UI_VIEW_DETAILS);
    ui_container_set_font(ef.list, UI_FONT_DIALOG);   /* (a dialog's list) */
    ui_container_set_columns(ef.list, 0, NULL);
    ui_container_layout(ef.list, ef_rects().list);
    ef.style_list = ui_container_new(ef.s, &st_src, NULL, UI_VIEW_DETAILS);
    ui_container_set_font(ef.style_list, UI_FONT_DIALOG);
    ui_container_set_columns(ef.style_list, 0, NULL);
    ui_container_layout(ef.style_list, ef_rects().style);
    size_t at = 0;
    for (size_t k = 0; k < ef.nfam; k++) if (!strcasecmp(ef.fam[k], fam)) { at = k; break; }
    ui_container_set_cursor(ef.list, at);
    sync_styles();
    ef_set_focus(EF_FAMILY);
    return true;
}

bool fontpal_edit_is_open(void) { return ef.s && !ef.closing; }
size_t fontpal_families(void) { read_families(); return ef.nfam; }

bool fontpal_edit_choose(const char *family, int size, const char *style)
{
    if (!fontpal_edit_is_open()) return false;
    for (size_t k = 0; k < ef.nfam; k++) if (!strcmp(ef.fam[k], family)) ui_container_set_cursor(ef.list, k);
    sync_styles();
    char ps[16];
    snprintf(ps, sizeof ps, "%d", size);
    ui_entry_set_text(ef.size, ps);
    for (size_t k = 0; k < ef.nstyles; k++)
        if (!strcasecmp(ef.styles[k], style)) ui_container_set_cursor(ef.style_list, k);
    ef_ok();
    return true;
}

size_t fontpal_edit_styles(char (*out)[64], size_t max)
{
    if (!fontpal_edit_is_open()) return 0;
    sync_styles();
    size_t n = ef.nstyles < max ? ef.nstyles : max;
    memcpy(out, ef.styles, n * sizeof *out);
    return n;
}

bool fontpal_edit_family(const char *family)
{
    if (!fontpal_edit_is_open()) return false;
    for (size_t k = 0; k < ef.nfam; k++)
        if (!strcmp(ef.fam[k], family)) { ui_container_set_cursor(ef.list, k); sync_styles(); ui_surface_damage_all(ef.s); return true; }
    return false;
}

/* ------------------------------------------------------------ the palette */

static ui_rect cell(size_t i)
{
    ui_rect c = ui_frame_client(pal.s);
    return (ui_rect){ c.x + M + (double)(i % COLS) * CELL_W, c.y + M + (double)(i / COLS) * CELL_H, CELL_W - 6, CELL_H - 6 };
}

static ui_rect edit_btn(void)
{
    ui_rect c = ui_frame_client(pal.s);
    size_t rows = (nsamples + COLS - 1) / COLS;
    return (ui_rect){ c.x + M, c.y + M + (double)rows * CELL_H + 2, BTN_W, ROW };
}

static size_t sample_at(double x, double y)
{
    for (size_t i = 0; i < nsamples; i++) if (ui_rect_has(cell(i), x, y)) return i;
    return SIZE_MAX;
}

static bool edit_down;

static void pal_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)s;
    const ui_scheme *k = &ui_scheme_now;
    ui_frame_draw(pal.s, cr, clip, "Font Palette");
    ui_rect c = ui_frame_client(pal.s);
    ui_fill(cr, c, k->face);
    for (size_t i = 0; i < nsamples; i++) {
        ui_rect r = cell(i);
        ui_fill(cr, r, k->client_bg);
        ui_bevel(cr, r, false);
        ui_font f = ui_font_named(samples[i]);
        cairo_save(cr);
        cairo_rectangle(cr, r.x + 2, r.y + 2, r.w - 4, r.h - 4);
        cairo_clip(cr);
        double tw = ui_text_width(f, samples[i]);
        if (i == pal.sel) {
            ui_fill(cr, (ui_rect){ r.x + 2, r.y + 2, r.w - 4, r.h - 4 }, k->selection);
            ui_set_color(cr, k->selection_text);
        } else {
            ui_set_color(cr, k->text);
        }
        ui_text_draw(cr, f, r.x + fmax(6, (r.w - tw) / 2), r.y + (r.h + ui_font_ascent(f)) / 2 - 2, samples[i], r.w - 12);
        cairo_restore(cr);
    }
    button(cr, edit_btn(), "Edit font...", edit_down, false);
    ui_rect st = { c.x, c.y + c.h - STATUS_H, c.w, STATUS_H };
    ui_bevel(cr, (ui_rect){ st.x + 1, st.y + 1, st.w - 2, st.h - 2 }, false);
    ui_set_color(cr, k->text);
    cairo_save(cr);
    cairo_rectangle(cr, st.x + 2, st.y + 2, st.w - 4, st.h - 4);
    cairo_clip(cr);
    ui_text_draw(cr, UI_FONT_DIALOG, st.x + 6, st.y + (STATUS_H + ui_font_ascent(UI_FONT_DIALOG)) / 2 - 1,
                 "Drag a font onto a folder or its title bar (Shift: all of them), or TheBar", st.w - 12);
    cairo_restore(cr);
}

static void drag_send(void *ctx, const char *mime, int fd)
{
    (void)ctx; (void)mime;
    ui_app_write_async(app, fd, pal.drag, strlen(pal.drag));
}

static void drag_end(void *ctx, bool dropped, uint32_t action) { (void)ctx; (void)dropped; (void)action; }

static void start_drag(size_t i)
{
    snprintf(pal.drag, sizeof pal.drag, "%s", samples[i]);
    int sc = ui_surface_scale(pal.s);
    ui_font f = ui_font_named(samples[i]);
    double w = fmin(200, ui_text_width(f, samples[i]) + 12), h = ui_font_line_h(f) + 8;
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ceil(w) * sc, (int)ceil(h) * sc);
    cairo_surface_set_device_scale(img, sc, sc);
    cairo_t *cr = cairo_create(img);
    ui_fill(cr, (ui_rect){ 0, 0, w, h }, ui_scheme_now.client_bg);
    ui_set_color(cr, ui_scheme_now.text);
    ui_text_draw(cr, f, 6, 4 + ui_font_ascent(f), samples[i], w - 8);
    cairo_destroy(cr);
    const char *mimes[] = { MIME_FONT };
    ui_drag_start(pal.s, pal.serial, mimes, 1, UI_DND_COPY, img, sc, drag_send, drag_end, NULL);
    cairo_surface_destroy(img);
}

enum { PM_EDIT = 1, PM_MENUS, PM_TITLES, PM_FOLDERS, PM_WC, PM_RESET };

static void menu_chosen(void *ctx, int64_t id)
{
    size_t i = (size_t)(uintptr_t)ctx;
    if (i >= nsamples || !apply) return;
    switch (id) {
    case PM_EDIT: fontpal_edit(i); break;
    case PM_MENUS: apply("menus", samples[i]); break;
    case PM_TITLES: apply("titles", samples[i]); break;
    case PM_FOLDERS: apply("folders", samples[i]); break;
    case PM_WC: apply("thebar", samples[i]); break;
    case PM_RESET:
        apply("menus", "");
        apply("titles", "");
        apply("folders", "");
        apply("thebar", "");
        break;
    default: break;
    }
}

static void sample_menu(size_t i, double x, double y, uint32_t serial)
{
    ui_menu_item items[] = {
        { PM_EDIT, "Edit font...", true, false, NULL, 0 },
        { -1, "-", false, false, NULL, 0 },
        { PM_MENUS, "Use for menus", true, false, NULL, 0 },
        { PM_TITLES, "Use for title bars", true, false, NULL, 0 },
        { PM_FOLDERS, "Use for every folder", true, false, NULL, 0 },
        { PM_WC, "Use for TheBar", true, false, NULL, 0 },
        { -1, "-", false, false, NULL, 0 },
        { PM_RESET, "Interface font everywhere again", true, false, NULL, 0 },
    };
    ui_menu_popup(pal.s, items, sizeof items / sizeof *items, x, y, serial, menu_chosen, (void *)(uintptr_t)i);
}

static void pal_pointer(ui_surface *s, const ui_pointer_event *e)
{
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    if ((e->kind != UI_PRESS || h != UI_HIT_CLIENT) && ui_frame_pointer(s, e) && e->kind == UI_PRESS) return;
    if (e->kind == UI_PRESS && h == UI_HIT_CLIENT) {
        size_t i = sample_at(e->x, e->y);
        if (i != SIZE_MAX) {
            pal.sel = i;
            ui_surface_damage_all(s);
            if (e->button == UI_BTN1 && e->clicks == 2) { fontpal_edit(i); return; }
            if (ui_mouse_is(UI_ACT_DRAG, e) || ui_mouse_is(UI_ACT_POPUP, e)) {
                pal.armed = ui_mouse_is(UI_ACT_DRAG, e);
                pal.popup_armed = ui_mouse_is(UI_ACT_POPUP, e);
                pal.press_i = i;
                pal.px = e->x;
                pal.py = e->y;
                pal.serial = e->serial;
            }
            return;
        }
        if (e->button == UI_BTN1 && ui_rect_has(edit_btn(), e->x, e->y)) { edit_down = true; ui_surface_damage_all(s); }
        return;
    }
    if (e->kind == UI_MOTION && pal.armed && hypot(e->x - pal.px, e->y - pal.py) > 4) {
        pal.armed = pal.popup_armed = false;
        start_drag(pal.press_i);
        return;
    }
    if (e->kind == UI_RELEASE) {
        if (pal.popup_armed) sample_menu(pal.press_i, e->x, e->y, e->serial);
        pal.armed = pal.popup_armed = false;
        if (edit_down) {
            edit_down = false;
            ui_surface_damage_all(s);
            if (ui_rect_has(edit_btn(), e->x, e->y)) fontpal_edit(pal.sel);
        }
    }
}

static void pal_key(ui_surface *s, const ui_key_event *e)
{
    if (!nsamples) return;
    size_t n = nsamples;
    switch (e->sym) {
    case XKB_KEY_Right: pal.sel = (pal.sel + 1) % n; break;
    case XKB_KEY_Left: pal.sel = (pal.sel + n - 1) % n; break;
    case XKB_KEY_Down: if (pal.sel + COLS < n) pal.sel += COLS; break;
    case XKB_KEY_Up: if (pal.sel >= COLS) pal.sel -= COLS; break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: fontpal_edit(pal.sel); return;
    default: return;
    }
    ui_surface_damage_all(s);
}

static void pal_free_later(void *arg)
{
    (void)arg;
    ui_surface_destroy(pal.s);
    pal.s = NULL;
    pal.closing = false;
}

static void pal_close(ui_surface *s)
{
    (void)s;
    if (!pal.s || pal.closing) return;
    pal.closing = true;
    events_raise("window.close", "Font Palette", "window", (ui_rect){ 0, 0, 0, 0 });
    ui_app_post(app, pal_free_later, NULL);
}

static void pal_resized(ui_surface *s, double w, double h) { (void)w; (void)h; ui_surface_damage_all(s); }
static void pal_focus(ui_surface *s, bool on) { (void)on; ui_frame_damage(s); }
static const ui_surface_ops pal_ops = { .draw = pal_draw, .pointer = pal_pointer, .key = pal_key, .resized = pal_resized,
                                        .close = pal_close, .focus = pal_focus };   /* (no font drops: its own samples, let go on it, would change the Dialogs font) */

void fontpal_init(ui_app *a, fontpal_apply_fn fn)
{
    app = a;
    apply = fn;
    if (!nsamples)
        fontpal_set_samples("8.Sans;9.Sans;10.Sans;12.Sans;14.Sans;10.Sans Bold;10.Serif;12.Serif;14.Serif Bold;10.Monospace;12.Monospace;18.Sans");
}

void fontpal_open(void)
{
    if (pal.s && !pal.closing) { ui_window_activate(pal.s); return; }
    /* (the frame's title bar and borders on top of the client) */
    double fr = ui_scheme_now.title_h + 2 * ui_scheme_now.border;
    double w = 2 * M + COLS * CELL_W + 2 * ui_scheme_now.border, h = 2 * M + 4 * CELL_H + ROW + 2 * M + STATUS_H + fr;
    pal.s = ui_window_new(app, &pal_ops, NULL, "io.github.sungsookhim.Objectspace.FontPalette", w, h);
    ui_window_set_title(pal.s, "Font Palette");
    events_raise("window.open", "Font Palette", "window", (ui_rect){ 0, 0, 0, 0 });
}

bool fontpal_is_open(void) { return pal.s && !pal.closing; }
size_t fontpal_count(void) { return nsamples; }
const char *fontpal_sample(size_t i) { return i < nsamples ? samples[i] : NULL; }
bool fontpal_snapshot(const char *png) { return pal.s && ui_surface_snapshot(pal.s, png); }
