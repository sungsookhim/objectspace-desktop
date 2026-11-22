/*
 * Settings notebooks (gui-design.md, Widget set 4), laid out as in Warp 4:
 * tabs across the top, the page's fields with their labels, and Undo,
 * Default and Help at the bottom. Changes apply as they are made (text on
 * Enter or when it loses focus); Undo puts back the values the notebook
 * opened with.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "ui.h"

#define TAB_H 24
#define ROW_H 30
#define FIELD_H 24
#define LABEL_W 140
#define MARGIN 14
#define BUTTON_W 80
#define BUTTON_H 24
#define STATUS_H 20

struct field {
    char *key, *label;
    ui_field_kind kind;
    ui_value v, orig;        /* now, and when the notebook opened */
    char **choices;
    size_t nchoices;
    ui_entry *entry;         /* text and number */
    cairo_surface_t *img;    /* icon: its picture, read from img_path */
    char *img_path;
    bool spin_down;          /* an arrow held (number) */
};

struct page {
    char *title;
    struct field *f;
    size_t n;
};

struct ui_notebook {
    ui_surface *win;
    char title[256];
    struct page *pages;
    size_t npages;
    size_t page;             /* shown */
    int focus;               /* field on the page; -1: none; npages-relative buttons below */
    int pressed_button;      /* 0 Undo, 1 Default, 2 Help; -1 none */
    struct field *icon_field;   /* an icon field's button held down (icon_pressed 0, 1), or NULL */
    int icon_pressed;
    struct field *drop_on;      /* an icon field a drag is over */
    char status[256];
    ui_notebook_set_fn set;
    ui_notebook_close_fn closed;
    void *ctx;
    bool closing;
};

/* ------------------------------------------------------------ values */

static void value_copy(ui_value *d, const ui_value *s)
{
    *d = *s;
    d->text = s->text ? strdup(s->text) : NULL;
}

static void value_free(ui_value *v)
{
    free(v->text);
    v->text = NULL;
}

static bool value_same(const ui_value *a, const ui_value *b)
{
    return a->number == b->number && a->checked == b->checked &&
           !strcmp(a->text ? a->text : "", b->text ? b->text : "");
}

static void free_pages(struct page *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        for (size_t k = 0; k < p[i].n; k++) {
            struct field *f = &p[i].f[k];
            free(f->key);
            free(f->label);
            value_free(&f->v);
            value_free(&f->orig);
            for (size_t c = 0; c < f->nchoices; c++) free(f->choices[c]);
            free(f->choices);
            ui_entry_free(f->entry);
            if (f->img) cairo_surface_destroy(f->img);
            free(f->img_path);
        }
        free(p[i].f);
        free(p[i].title);
    }
    free(p);
}

static void entry_from_value(struct field *f)
{
    if (!f->entry) return;
    char buf[64];
    if (f->kind == UI_FIELD_NUMBER) {
        snprintf(buf, sizeof buf, "%lld", (long long)f->v.number);
        ui_entry_set_text(f->entry, buf);
    } else {
        ui_entry_set_text(f->entry, f->v.text ? f->v.text : "");
    }
}

static struct page *copy_pages(const ui_page_desc *src, size_t n)
{
    struct page *p = calloc(n ? n : 1, sizeof *p);
    for (size_t i = 0; i < n; i++) {
        p[i].title = strdup(src[i].title ? src[i].title : "");
        p[i].n = src[i].nfields;
        p[i].f = calloc(p[i].n ? p[i].n : 1, sizeof *p[i].f);
        for (size_t k = 0; k < p[i].n; k++) {
            const ui_field_desc *d = &src[i].fields[k];
            struct field *f = &p[i].f[k];
            f->key = strdup(d->key);
            f->label = strdup(d->label ? d->label : d->key);
            f->kind = d->kind;
            value_copy(&f->v, &d->value);
            value_copy(&f->orig, &d->value);
            f->nchoices = d->nchoices;
            f->choices = calloc(f->nchoices ? f->nchoices : 1, sizeof *f->choices);
            for (size_t c = 0; c < f->nchoices; c++) f->choices[c] = strdup(d->choices[c]);
            if (f->kind == UI_FIELD_TEXT || f->kind == UI_FIELD_NUMBER || f->kind == UI_FIELD_FILE || f->kind == UI_FIELD_COLOR) {
                f->entry = ui_entry_new("");
                entry_from_value(f);
            }
        }
    }
    return p;
}

/* ------------------------------------------------------------ geometry */

static ui_rect tab_rect(ui_notebook *nb, size_t i)
{
    ui_rect c = ui_frame_client(nb->win);
    double x = c.x + MARGIN;
    for (size_t k = 0; k < i; k++) x += ui_text_width(UI_FONT_REGULAR, nb->pages[k].title) + 28;
    return (ui_rect){ x, c.y + 8, ui_text_width(UI_FONT_REGULAR, nb->pages[i].title) + 24, TAB_H };
}

static ui_rect page_rect(ui_notebook *nb)
{
    ui_rect c = ui_frame_client(nb->win);
    return (ui_rect){ c.x + MARGIN - 4, c.y + 8 + TAB_H, c.w - 2 * (MARGIN - 4), c.h - 8 - TAB_H - BUTTON_H - 22 - STATUS_H };
}

#define ICON_BOX 76           /* an icon field's picture */
#define ICON_ROW (ICON_BOX + 12)

static double row_h(const struct field *f) { return f->kind == UI_FIELD_ICON ? ICON_ROW : ROW_H; }

static ui_rect control_rect(ui_notebook *nb, size_t k)
{
    ui_rect p = page_rect(nb);
    double x = p.x + MARGIN + LABEL_W, w = p.w - 2 * MARGIN - LABEL_W;
    struct page *pg = &nb->pages[nb->page];
    struct field *f = &pg->f[k];
    double y = p.y + MARGIN;
    for (size_t i = 0; i < k; i++) y += row_h(&pg->f[i]);
    if (f->kind == UI_FIELD_CHECKBOX) w = FIELD_H;
    else if (f->kind == UI_FIELD_NUMBER) w = fmin(w, 140);
    else if (f->kind == UI_FIELD_COLOR) w = fmin(w, FIELD_H + 6 + 100);
    else if (f->kind == UI_FIELD_CHOICE) w = fmin(w, 220);
    return (ui_rect){ x, y, w, f->kind == UI_FIELD_ICON ? ICON_BOX : FIELD_H };
}

/* An icon field: the picture, and its two buttons (0 Choose..., 1 Default). */
static ui_rect icon_box(ui_rect r) { return (ui_rect){ r.x, r.y, ICON_BOX, ICON_BOX }; }
static ui_rect icon_button(ui_rect r, int i) { return (ui_rect){ r.x + ICON_BOX + 14, r.y + i * 30, 110, 24 }; }

static ui_rect spin_rect(ui_rect r, bool up)
{
    return (ui_rect){ r.x + r.w - 16, up ? r.y : r.y + r.h / 2, 16, r.h / 2 };
}

/* A file field: its path, and Browse... after it; a colour field: its
 * swatch, then its "#rrggbb". */
#define BROWSE_W 80
static ui_rect file_button(ui_rect r) { return (ui_rect){ r.x + r.w - BROWSE_W, r.y, BROWSE_W, r.h }; }
static ui_rect color_swatch(ui_rect r) { return (ui_rect){ r.x, r.y, FIELD_H, FIELD_H }; }

/* Where a field's text is typed (number: before its arrows). */
static ui_rect entry_rect(const struct field *f, ui_rect r)
{
    if (f->kind == UI_FIELD_NUMBER) r.w -= 16;
    else if (f->kind == UI_FIELD_FILE) r.w -= BROWSE_W + 6;
    else if (f->kind == UI_FIELD_COLOR) { r.x += FIELD_H + 6; r.w -= FIELD_H + 6; }
    return r;
}

/* The button of an icon field (i: 0 Choose..., 1 Default) or file field (Browse...). */
static ui_rect field_button(const struct field *f, ui_rect r, int i)
{
    return f->kind == UI_FIELD_FILE ? file_button(r) : icon_button(r, i);
}

bool ui_parse_color(const char *t, double rgb[3])
{
    if (!t) return false;
    if (*t == '#') t++;
    size_t n = strlen(t);
    if (n != 6 && n != 3) return false;
    for (size_t i = 0; i < n; i++)
        if (!strchr("0123456789abcdefABCDEF", t[i])) return false;
    for (int i = 0; i < 3; i++) {
        char hx[3] = { t[n == 6 ? 2 * i : i], t[n == 6 ? 2 * i + 1 : i], 0 };
        rgb[i] = strtol(hx, NULL, 16) / 255.0;
    }
    return true;
}

static ui_rect button_rect(ui_notebook *nb, int i)
{
    ui_rect c = ui_frame_client(nb->win);
    double y = c.y + c.h - STATUS_H - BUTTON_H - 10;
    return (ui_rect){ c.x + MARGIN + i * (BUTTON_W + 8), y, BUTTON_W, BUTTON_H };
}

static ui_rect status_rect(ui_notebook *nb)
{
    ui_rect c = ui_frame_client(nb->win);
    return (ui_rect){ c.x, c.y + c.h - STATUS_H, c.w, STATUS_H };
}

static void a11y_nb(ui_notebook *nb);
static void damage(ui_notebook *nb)
{
    ui_surface_damage(nb->win, ui_frame_client(nb->win));
    a11y_nb(nb);
}

/* ------------------------------------------------------------ drawing */

static const char *button_labels[3] = { "Undo", "Default", "Help" };

static void draw_check(cairo_t *cr, ui_rect r, bool on)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_rect b = { r.x + 4, r.y + 4, r.h - 8, r.h - 8 };
    ui_fill(cr, b, k->client_bg);
    ui_bevel(cr, b, false);
    if (!on) return;
    ui_set_color(cr, k->text);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, b.x + 3, b.y + b.h / 2);
    cairo_line_to(cr, b.x + b.w / 2 - 1, b.y + b.h - 4);
    cairo_line_to(cr, b.x + b.w - 3, b.y + 3);
    cairo_stroke(cr);
}

static void focus_ring(cairo_t *cr, ui_rect r);

/* An icon field: its picture (the object's own icon, or a note that it
 * has its kind's), Choose... and Default beside it, and the hint that an
 * image can be dropped on it. */
static void draw_icon_field(ui_notebook *nb, struct field *f, cairo_t *cr, ui_rect r, bool focused)
{
    (void)nb;
    const ui_scheme *k = &ui_scheme_now;
    const char *want = f->v.text ? f->v.text : "";
    if (!f->img_path || strcmp(f->img_path, want)) {
        if (f->img) cairo_surface_destroy(f->img);
        f->img = NULL;
        free(f->img_path);
        f->img_path = strdup(want);
        if (*want) {
            f->img = cairo_image_surface_create_from_png(want);
            if (cairo_surface_status(f->img) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(f->img); f->img = NULL; }
        }
    }
    ui_rect b = icon_box(r);
    ui_fill(cr, b, nb->drop_on == f ? k->selection : k->client_bg);
    ui_bevel(cr, b, false);
    if (f->img) {
        double iw = cairo_image_surface_get_width(f->img), ih = cairo_image_surface_get_height(f->img);
        double sz = ICON_BOX - 12, sc = sz / (iw > ih ? iw : ih);
        cairo_save(cr);
        cairo_translate(cr, b.x + (ICON_BOX - iw * sc) / 2, b.y + (ICON_BOX - ih * sc) / 2);
        cairo_scale(cr, sc, sc);
        cairo_set_source_surface(cr, f->img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        ui_set_color(cr, k->disabled_text);
        const char *t = "(its kind's)";
        double tw = ui_text_width(UI_FONT_REGULAR, t);
        ui_text_draw(cr, UI_FONT_REGULAR, b.x + (ICON_BOX - tw) / 2, b.y + ICON_BOX / 2 + 4, t, ICON_BOX - 4);
    }
    static const char *const labels[2] = { "Choose...", "Default" };
    for (int i = 0; i < 2; i++) {
        ui_rect bt = icon_button(r, i);
        bool down = nb->icon_pressed == i && nb->icon_field == f;
        ui_fill(cr, bt, k->face);
        ui_bevel(cr, bt, !down);
        ui_set_color(cr, i == 1 && !*want ? k->disabled_text : k->text);
        double tw = ui_text_width(UI_FONT_REGULAR, labels[i]);
        ui_text_draw(cr, UI_FONT_REGULAR, bt.x + (bt.w - tw) / 2 + down, bt.y + bt.h / 2 + 5 + down, labels[i], bt.w - 4);
    }
    ui_set_color(cr, k->disabled_text);
    ui_text_draw(cr, UI_FONT_REGULAR, icon_button(r, 0).x, r.y + ICON_BOX - 6, "or drop an image on the picture", r.w - ICON_BOX - 14);
    if (focused) focus_ring(cr, b);
}

static void draw_arrow(cairo_t *cr, ui_rect r, bool up)
{
    double cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (up) { cairo_move_to(cr, cx - 3, cy + 2); cairo_line_to(cr, cx + 3, cy + 2); cairo_line_to(cr, cx, cy - 2); }
    else { cairo_move_to(cr, cx - 3, cy - 2); cairo_line_to(cr, cx + 3, cy - 2); cairo_line_to(cr, cx, cy + 2); }
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void focus_ring(cairo_t *cr, ui_rect r)
{
    ui_set_color(cr, ui_scheme_now.cursor);
    double dash = 1;
    cairo_set_dash(cr, &dash, 1, 0);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, r.x - 2.5, r.y - 2.5, r.w + 5, r.h + 5);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);
}

static void nb_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    ui_notebook *nb = ui_surface_data(s);
    const ui_scheme *k = &ui_scheme_now;
    ui_frame_draw(s, cr, clip, nb->title);
    ui_rect c = ui_frame_client(s);
    if (!ui_rect_meets(clip, c)) return;
    ui_fill(cr, c, k->face);
    double base_off = (FIELD_H + ui_text_ascent()) / 2 - 1;
    /* Tabs: the shown one raised and joined to the page. */
    for (size_t i = 0; i < nb->npages; i++) {
        ui_rect t = tab_rect(nb, i);
        bool on = i == nb->page;
        if (!on) { t.y += 2; t.h -= 2; }
        ui_fill(cr, t, on ? k->face : (ui_color){ k->face.r * 0.9, k->face.g * 0.9, k->face.b * 0.9, 1 });
        ui_bevel(cr, (ui_rect){ t.x, t.y, t.w, t.h + (on ? 2 : 0) }, true);
        ui_set_color(cr, k->text);
        ui_text_draw(cr, on ? UI_FONT_BOLD : UI_FONT_REGULAR, t.x + 12, t.y + (t.h + ui_text_ascent()) / 2 - 1,
                     nb->pages[i].title, 0);
    }
    ui_rect p = page_rect(nb);
    ui_bevel(cr, p, true);
    if (nb->npages) {
        ui_rect t = tab_rect(nb, nb->page);
        ui_fill(cr, (ui_rect){ t.x + 1, p.y, t.w - 2, 1 }, k->face);   /* the tab opens into the page */
    }
    struct page *pg = nb->npages ? &nb->pages[nb->page] : NULL;
    for (size_t i = 0; pg && i < pg->n; i++) {
        struct field *f = &pg->f[i];
        ui_rect r = control_rect(nb, i);
        bool focused = nb->focus == (int)i;
        ui_set_color(cr, k->text);
        ui_text_draw(cr, UI_FONT_REGULAR, p.x + MARGIN, r.y + base_off, f->label, LABEL_W - 8);
        switch (f->kind) {
        case UI_FIELD_TEXT:
            ui_entry_draw(f->entry, cr, r, focused);
            break;
        case UI_FIELD_INFO:
            ui_set_color(cr, k->text);
            ui_text_draw(cr, UI_FONT_REGULAR, r.x, r.y + base_off, f->v.text ? f->v.text : "", r.w);
            break;
        case UI_FIELD_ICON:
            draw_icon_field(nb, f, cr, r, focused);
            break;
        case UI_FIELD_FILE: {
            ui_rect er = entry_rect(f, r);
            if (nb->drop_on == f) { ui_fill(cr, er, k->selection); ui_bevel(cr, er, false); }
            else ui_entry_draw(f->entry, cr, er, focused);
            ui_rect b = file_button(r);
            bool down = nb->icon_field == f;
            ui_fill(cr, b, k->face);
            ui_bevel(cr, b, !down);
            ui_set_color(cr, k->text);
            double tw = ui_text_width(UI_FONT_REGULAR, "Browse...");
            ui_text_draw(cr, UI_FONT_REGULAR, b.x + (b.w - tw) / 2 + down, b.y + base_off + down, "Browse...", b.w - 4);
            break;
        }
        case UI_FIELD_COLOR: {
            ui_rect sw = color_swatch(r);
            double rgb[3];
            bool ok = ui_parse_color(f->v.text, rgb);
            if (ok) {
                cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
                cairo_rectangle(cr, sw.x, sw.y, sw.w, sw.h);
                cairo_fill(cr);
            } else {
                ui_fill(cr, sw, k->client_bg);
            }
            ui_bevel(cr, sw, false);
            ui_entry_draw(f->entry, cr, entry_rect(f, r), focused);
            break;
        }
        case UI_FIELD_NUMBER: {
            ui_rect er = { r.x, r.y, r.w - 16, r.h };
            ui_entry_draw(f->entry, cr, er, focused);
            for (int up = 0; up < 2; up++) {
                ui_rect a = spin_rect(r, up);
                ui_fill(cr, a, k->face);
                ui_bevel(cr, a, true);
                ui_set_color(cr, k->text);
                draw_arrow(cr, a, up);
            }
            break;
        }
        case UI_FIELD_CHECKBOX:
            draw_check(cr, r, f->v.checked);
            if (focused) focus_ring(cr, r);
            break;
        case UI_FIELD_CHOICE: {
            ui_fill(cr, r, k->client_bg);
            ui_bevel(cr, r, false);
            const char *t = f->v.number >= 0 && (size_t)f->v.number < f->nchoices ? f->choices[f->v.number] : "";
            ui_set_color(cr, k->text);
            ui_text_draw(cr, UI_FONT_REGULAR, r.x + 6, r.y + base_off, t, r.w - 30);
            ui_rect a = { r.x + r.w - 20, r.y + 2, 18, r.h - 4 };
            ui_fill(cr, a, k->face);
            ui_bevel(cr, a, true);
            ui_set_color(cr, k->text);
            draw_arrow(cr, a, false);
            if (focused) focus_ring(cr, r);
            break;
        }
        }
    }
    int nf = pg ? (int)pg->n : 0;
    for (int i = 0; i < 3; i++) {
        ui_rect b = button_rect(nb, i);
        bool enabled = i == 0;   /* Default and Help: later */
        bool down = nb->pressed_button == i;
        ui_fill(cr, b, k->face);
        ui_bevel(cr, b, !down);
        ui_set_color(cr, enabled ? k->text : k->disabled_text);
        double tw = ui_text_width(UI_FONT_REGULAR, button_labels[i]);
        ui_text_draw(cr, UI_FONT_REGULAR, b.x + (b.w - tw) / 2 + (down ? 1 : 0), b.y + (b.h + ui_text_ascent()) / 2 - 1 + (down ? 1 : 0),
                     button_labels[i], 0);
        if (nb->focus == nf + i) focus_ring(cr, (ui_rect){ b.x + 3, b.y + 3, b.w - 6, b.h - 6 });
    }
    ui_rect st = status_rect(nb);
    ui_fill(cr, st, k->face);
    ui_bevel(cr, (ui_rect){ st.x + 1, st.y + 1, st.w - 2, st.h - 2 }, false);
    ui_set_color(cr, k->text);
    ui_text_draw(cr, UI_FONT_REGULAR, st.x + 6, st.y + (STATUS_H + ui_text_ascent()) / 2 - 1, nb->status, st.w - 12);
}

/* ------------------------------------------------------------ behaviour */

static struct field *focused_field(ui_notebook *nb)
{
    if (!nb->npages) return NULL;
    struct page *pg = &nb->pages[nb->page];
    return nb->focus >= 0 && (size_t)nb->focus < pg->n ? &pg->f[nb->focus] : NULL;
}

static void apply(ui_notebook *nb, struct field *f)
{
    if (nb->set) nb->set(nb->ctx, f->key, f->kind, &f->v);
}

/* A text or number field's typing becomes its value. */
static void commit_entry(ui_notebook *nb, struct field *f)
{
    if (!f || !f->entry) return;
    const char *t = ui_entry_text(f->entry);
    if (f->kind == UI_FIELD_NUMBER) {
        char *end;
        long long v = strtoll(t, &end, 10);
        if (end == t || *end) { entry_from_value(f); damage(nb); return; }   /* not a number: put it back */
        if (v == f->v.number) return;
        f->v.number = v;
    } else if (f->kind == UI_FIELD_COLOR) {
        double rgb[3];
        char norm[8];
        if (!ui_parse_color(t, rgb)) { entry_from_value(f); damage(nb); return; }   /* not a colour: put it back */
        snprintf(norm, sizeof norm, "#%02x%02x%02x", (int)lround(rgb[0] * 255), (int)lround(rgb[1] * 255), (int)lround(rgb[2] * 255));
        if (!strcmp(norm, f->v.text ? f->v.text : "")) { entry_from_value(f); return; }
        free(f->v.text);
        f->v.text = strdup(norm);
        entry_from_value(f);
    } else {
        if (!strcmp(t, f->v.text ? f->v.text : "")) return;
        free(f->v.text);
        f->v.text = strdup(t);
    }
    apply(nb, f);
}

static void set_focus(ui_notebook *nb, int i)
{
    if (i == nb->focus) return;
    commit_entry(nb, focused_field(nb));
    nb->focus = i;
    struct field *f = focused_field(nb);
    if (f && f->entry) ui_entry_select_all(f->entry);
    /* an input method for text fields (not numbers) */
    bool text = f && f->entry && (f->kind == UI_FIELD_TEXT || f->kind == UI_FIELD_FILE);
    ui_text_input_focus(nb->win, text ? f->entry : NULL, text ? entry_rect(f, control_rect(nb, (size_t)i)) : (ui_rect){ 0, 0, 0, 0 });
    damage(nb);
}

static void show_page(ui_notebook *nb, size_t p)
{
    if (p >= nb->npages || p == nb->page) return;
    commit_entry(nb, focused_field(nb));
    nb->page = p;
    nb->focus = -1;
    ui_text_input_focus(nb->win, NULL, (ui_rect){ 0, 0, 0, 0 });
    damage(nb);
}

/* For screen readers: the field with the keyboard (its label, its
 * value), Undo, or with nothing focused the page's tab. */
static void a11y_nb(ui_notebook *nb)
{
    struct page *pg = nb->npages ? &nb->pages[nb->page] : NULL;
    int nf = pg ? (int)pg->n : 0;
    struct field *f = focused_field(nb);
    ui_a11y_info i = { .index = nb->focus, .count = nf + 1 };
    char num[32];
    if (f) {
        i.name = f->label;
        i.at = control_rect(nb, (size_t)nb->focus);
        switch (f->kind) {
        case UI_FIELD_TEXT: i.role = UI_ROLE_ENTRY; i.text = ui_entry_text(f->entry); break;
        case UI_FIELD_FILE:
            i.role = UI_ROLE_ENTRY;
            i.text = ui_entry_text(f->entry);
            i.description = "a file; Alt+Down: Browse...";
            break;
        case UI_FIELD_COLOR:
            i.role = UI_ROLE_ENTRY;
            i.text = ui_entry_text(f->entry);
            i.description = "a colour, as #rrggbb";
            break;
        case UI_FIELD_INFO: i.role = UI_ROLE_ENTRY; i.text = f->v.text; break;   /* (read only) */
        case UI_FIELD_NUMBER:
            i.role = UI_ROLE_SPIN_BUTTON;
            snprintf(num, sizeof num, "%s", ui_entry_text(f->entry));
            i.description = num;
            break;
        case UI_FIELD_CHECKBOX: i.role = UI_ROLE_CHECK_BOX; i.checked = f->v.checked; break;
        case UI_FIELD_ICON:
            i.role = UI_ROLE_BUTTON;
            i.description = f->v.text && *f->v.text ? "its own icon; Enter: choose another, Delete: its kind's"
                                                    : "its kind's icon; Enter: choose one";
            break;
        case UI_FIELD_CHOICE:
            i.role = UI_ROLE_COMBO_BOX;
            i.description = f->v.number >= 0 && (size_t)f->v.number < f->nchoices ? f->choices[f->v.number] : "";
            break;
        }
    } else if (nb->focus == nf) {
        i.role = UI_ROLE_BUTTON;
        i.name = "Undo";
    } else if (pg) {
        i.role = UI_ROLE_PAGE_TAB;
        i.name = pg->title;
        i.index = (int)nb->page;
        i.count = (int)nb->npages;
    } else {
        return;
    }
    ui_a11y_focus(nb->win, &i);
}

static void step_number(ui_notebook *nb, struct field *f, int d)
{
    commit_entry(nb, f);
    f->v.number += d;
    entry_from_value(f);
    apply(nb, f);
    damage(nb);
}

static void set_choice(ui_notebook *nb, struct field *f, int64_t c)
{
    if (c < 0 || (size_t)c >= f->nchoices || c == f->v.number) return;
    f->v.number = c;
    apply(nb, f);
    damage(nb);
}

struct choice_ctx { ui_notebook *nb; size_t page, field; };

static void choice_chosen(void *arg, int64_t id)
{
    struct choice_ctx *cc = arg;
    ui_notebook *nb = cc->nb;
    if (id >= 0 && !nb->closing && cc->page < nb->npages && cc->field < nb->pages[cc->page].n)
        set_choice(nb, &nb->pages[cc->page].f[cc->field], id);
    free(cc);
}

static void open_choices(ui_notebook *nb, size_t k, uint32_t serial)
{
    struct field *f = &nb->pages[nb->page].f[k];
    ui_menu_item *items = calloc(f->nchoices ? f->nchoices : 1, sizeof *items);
    for (size_t i = 0; i < f->nchoices; i++)
        items[i] = (ui_menu_item){ (int64_t)i, f->choices[i], true, (int64_t)i == f->v.number, NULL, 0 };
    ui_rect r = control_rect(nb, k);
    struct choice_ctx *cc = malloc(sizeof *cc);
    cc->nb = nb;
    cc->page = nb->page;
    cc->field = k;
    ui_menu_popup(nb->win, items, f->nchoices, r.x, r.y + r.h, serial, choice_chosen, cc);
    free(items);
}

static void undo(ui_notebook *nb)
{
    commit_entry(nb, focused_field(nb));
    for (size_t p = 0; p < nb->npages; p++)
        for (size_t k = 0; k < nb->pages[p].n; k++) {
            struct field *f = &nb->pages[p].f[k];
            if (value_same(&f->v, &f->orig)) continue;
            value_free(&f->v);
            value_copy(&f->v, &f->orig);
            entry_from_value(f);
            apply(nb, f);
        }
    damage(nb);
}

/* Choose... (0) or Default (1) of an icon field, Browse... of a file
 * field: its owner does it. */
static void icon_action(ui_notebook *nb, struct field *f, int which)
{
    if (!nb->set) return;
    if (f->kind == UI_FIELD_FILE) {
        commit_entry(nb, f);
        ui_value v = { .text = NULL };
        nb->set(nb->ctx, f->key, UI_FIELD_FILE, &v);
        return;
    }
    if (which == 1 && !(f->v.text && *f->v.text)) return;   /* already the kind's */
    ui_value v = { .text = which == 1 ? "" : NULL };
    nb->set(nb->ctx, f->key, UI_FIELD_ICON, &v);
}

/* An image dropped on an icon field's picture (text/uri-list: a file). */
static struct field *icon_field_at(ui_notebook *nb, double x, double y)
{
    struct page *pg = nb->npages ? &nb->pages[nb->page] : NULL;
    for (size_t k = 0; pg && k < pg->n; k++) {
        if (pg->f[k].kind == UI_FIELD_ICON && ui_rect_has(icon_box(control_rect(nb, k)), x, y)) return &pg->f[k];
        if (pg->f[k].kind == UI_FIELD_FILE && ui_rect_has(control_rect(nb, k), x, y)) return &pg->f[k];
    }
    return NULL;
}

static uint32_t nb_drag_motion(ui_surface *s, double x, double y, const char *const *mimes, size_t n, uint32_t src_actions,
                               unsigned mods, const char **mime)
{
    (void)src_actions; (void)mods;
    ui_notebook *nb = ui_surface_data(s);
    struct field *f = NULL;
    for (size_t i = 0; i < n && !f; i++)
        if (!strcmp(mimes[i], "text/uri-list")) { f = icon_field_at(nb, x, y); if (f) *mime = mimes[i]; }
    if (f != nb->drop_on) { nb->drop_on = f; damage(nb); }
    return f ? UI_DND_COPY : 0;
}

static void nb_drag_leave(ui_surface *s)
{
    ui_notebook *nb = ui_surface_data(s);
    if (nb->drop_on) { nb->drop_on = NULL; damage(nb); }
}

static uint32_t nb_drop(ui_surface *s, double x, double y, const char *mime, const char *data, size_t len, uint32_t action,
                        uint32_t start_action, unsigned mods)
{
    (void)mime; (void)action; (void)start_action; (void)mods;
    ui_notebook *nb = ui_surface_data(s);
    nb->drop_on = NULL;
    damage(nb);
    struct field *f = icon_field_at(nb, x, y);
    if (!f || !nb->set) return 0;
    /* the first file:// line, unescaped */
    char path[4096];
    size_t o = 0;
    const char *p = data, *end = data + len;
    while (p < end && (*p == '#' || strncmp(p, "file://", 7))) {   /* comments and other schemes */
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        p = nl ? nl + 1 : end;
    }
    if (p >= end) return 0;
    p += 7;
    while (p < end && *p != '/' && *p != '\r' && *p != '\n') p++;   /* a host name */
    for (; p < end && *p != '\r' && *p != '\n' && o < sizeof path - 1; p++) {
        if (*p == '%' && end - p > 2) {
            char hx[3] = { p[1], p[2], 0 };
            path[o++] = (char)strtol(hx, NULL, 16);
            p += 2;
        } else {
            path[o++] = *p;
        }
    }
    path[o] = 0;
    if (!o) return 0;
    if (f->kind == UI_FIELD_FILE) {   /* the path, as if typed */
        ui_notebook_set_value(nb, f->key, path);
        return UI_DND_COPY;
    }
    ui_value v = { .text = path };
    nb->set(nb->ctx, f->key, UI_FIELD_ICON, &v);
    return UI_DND_COPY;
}

static void nb_pointer(ui_surface *s, const ui_pointer_event *e)
{
    ui_notebook *nb = ui_surface_data(s);
    if (e->kind != UI_PRESS || ui_frame_hit(s, e->x, e->y) != UI_HIT_CLIENT)
        if (ui_frame_pointer(s, e) && e->kind == UI_PRESS) return;
    struct field *ff = focused_field(nb);
    if (ff && ff->entry && (e->kind == UI_MOTION || e->kind == UI_RELEASE)) {
        ui_rect r = entry_rect(ff, control_rect(nb, (size_t)nb->focus));
        if (ui_entry_pointer(ff->entry, e, r) != UI_ENTRY_NONE) damage(nb);
    }
    if (e->kind == UI_RELEASE && nb->icon_field) {
        struct field *f = nb->icon_field;
        int b = nb->icon_pressed;
        nb->icon_field = NULL;
        damage(nb);
        struct page *pg = &nb->pages[nb->page];
        for (size_t k = 0; k < pg->n; k++)
            if (&pg->f[k] == f && ui_rect_has(field_button(f, control_rect(nb, k), b), e->x, e->y)) icon_action(nb, f, b);
        return;
    }
    if (e->kind == UI_RELEASE && nb->pressed_button >= 0) {
        int b = nb->pressed_button;
        nb->pressed_button = -1;
        damage(nb);
        if (b == 0 && ui_rect_has(button_rect(nb, 0), e->x, e->y)) undo(nb);
        return;
    }
    if (e->kind != UI_PRESS || e->button != ui_mouse_button(UI_ACT_SELECT)) return;
    for (size_t i = 0; i < nb->npages; i++)
        if (ui_rect_has(tab_rect(nb, i), e->x, e->y)) { show_page(nb, i); return; }
    struct page *pg = nb->npages ? &nb->pages[nb->page] : NULL;
    for (size_t k = 0; pg && k < pg->n; k++) {
        ui_rect r = control_rect(nb, k);
        if (pg->f[k].kind == UI_FIELD_ICON) {
            for (int i = 0; i < 2; i++)
                if (ui_rect_has(icon_button(r, i), e->x, e->y)) {
                    set_focus(nb, (int)k);
                    nb->icon_field = &pg->f[k];
                    nb->icon_pressed = i;
                    damage(nb);
                    return;
                }
            r = icon_box(r);
        }
        if (pg->f[k].kind == UI_FIELD_FILE && ui_rect_has(file_button(r), e->x, e->y)) {
            set_focus(nb, (int)k);
            nb->icon_field = &pg->f[k];
            nb->icon_pressed = 0;
            damage(nb);
            return;
        }
        if (!ui_rect_has(r, e->x, e->y)) continue;
        struct field *f = &pg->f[k];
        set_focus(nb, (int)k);
        switch (f->kind) {
        case UI_FIELD_CHECKBOX:
            f->v.checked = !f->v.checked;
            apply(nb, f);
            damage(nb);
            break;
        case UI_FIELD_CHOICE:
            open_choices(nb, k, e->serial);
            break;
        case UI_FIELD_ICON:
            break;   /* the picture: focused (Enter chooses) */
        case UI_FIELD_INFO:
            break;
        case UI_FIELD_NUMBER:
            if (ui_rect_has(spin_rect(r, true), e->x, e->y)) { step_number(nb, f, 1); break; }
            if (ui_rect_has(spin_rect(r, false), e->x, e->y)) { step_number(nb, f, -1); break; }
            /* fall through */
        case UI_FIELD_TEXT:
        case UI_FIELD_FILE:
        case UI_FIELD_COLOR:
            if (ui_rect_has(entry_rect(f, r), e->x, e->y) && ui_entry_pointer(f->entry, e, entry_rect(f, r)) != UI_ENTRY_NONE)
                damage(nb);
            break;
        }
        return;
    }
    int nf = pg ? (int)pg->n : 0;
    if (ui_rect_has(button_rect(nb, 0), e->x, e->y)) {
        set_focus(nb, nf);
        nb->pressed_button = 0;
        damage(nb);
        return;
    }
    set_focus(nb, -1);   /* a click on the page: nothing focused */
}

static void nb_key(ui_surface *s, const ui_key_event *e)
{
    ui_notebook *nb = ui_surface_data(s);
    struct page *pg = nb->npages ? &nb->pages[nb->page] : NULL;
    int nf = pg ? (int)pg->n : 0, total = nf + 1;   /* fields, then Undo */
    struct field *f = focused_field(nb);
    if (e->sym == XKB_KEY_Tab || e->sym == XKB_KEY_ISO_Left_Tab) {
        bool back = (e->mods & UI_MOD_SHIFT) || e->sym == XKB_KEY_ISO_Left_Tab;
        int i = nb->focus < 0 ? (back ? total - 1 : 0) : (nb->focus + (back ? total - 1 : 1)) % total;
        set_focus(nb, i);
        return;
    }
    if ((e->mods & UI_MOD_CTRL) && (e->sym == XKB_KEY_Page_Down || e->sym == XKB_KEY_Page_Up)) {
        size_t n = nb->npages;
        if (n) show_page(nb, (nb->page + (e->sym == XKB_KEY_Page_Down ? 1 : n - 1)) % n);
        return;
    }
    if (e->sym == XKB_KEY_Escape && !(f && f->entry)) {   /* Escape closes, as in OS/2 */
        ui_notebook_close(nb);
        return;
    }
    if (nb->focus == nf && (e->sym == XKB_KEY_Return || e->sym == XKB_KEY_space)) { undo(nb); return; }
    if (!f) return;
    switch (f->kind) {
    case UI_FIELD_CHECKBOX:
        if (e->sym == XKB_KEY_space) { f->v.checked = !f->v.checked; apply(nb, f); damage(nb); }
        return;
    case UI_FIELD_INFO:
        return;
    case UI_FIELD_ICON:   /* Enter or Space: Choose...; Delete: Default */
        if (e->sym == XKB_KEY_space || e->sym == XKB_KEY_Return || e->sym == XKB_KEY_KP_Enter) icon_action(nb, f, 0);
        else if (e->sym == XKB_KEY_Delete || e->sym == XKB_KEY_BackSpace) icon_action(nb, f, 1);
        return;
    case UI_FIELD_CHOICE:
        if (e->sym == XKB_KEY_Up) set_choice(nb, f, f->v.number - 1);
        else if (e->sym == XKB_KEY_Down && !(e->mods & UI_MOD_ALT)) set_choice(nb, f, f->v.number + 1);
        else if (e->sym == XKB_KEY_space || e->sym == XKB_KEY_Return || e->sym == XKB_KEY_Down)
            open_choices(nb, (size_t)nb->focus, 0);
        return;
    case UI_FIELD_NUMBER:
        if (e->sym == XKB_KEY_Up) { step_number(nb, f, 1); return; }
        if (e->sym == XKB_KEY_Down) { step_number(nb, f, -1); return; }
        /* fall through */
    case UI_FIELD_FILE:
        if (f->kind == UI_FIELD_FILE && e->sym == XKB_KEY_Down && (e->mods & UI_MOD_ALT)) { icon_action(nb, f, 0); return; }
        /* fall through */
    case UI_FIELD_COLOR:
    case UI_FIELD_TEXT:
        switch (ui_entry_key(f->entry, e)) {
        case UI_ENTRY_COMMIT: commit_entry(nb, f); ui_entry_select_all(f->entry); damage(nb); break;
        case UI_ENTRY_CANCEL: entry_from_value(f); ui_entry_select_all(f->entry); damage(nb); break;
        case UI_ENTRY_NONE: break;
        default: damage(nb); break;
        }
        return;
    }
}

static void nb_resized(ui_surface *s, double w, double h)
{
    (void)w; (void)h;
    ui_surface_damage_all(s);
}

static void nb_focus(ui_surface *s, bool on)
{
    ui_notebook *nb = ui_surface_data(s);
    if (!on) commit_entry(nb, focused_field(nb));   /* leaving the window applies typing */
    ui_frame_damage(s);
}

static void destroy_later(void *arg)
{
    ui_notebook *nb = arg;
    if (nb->closed) nb->closed(nb->ctx);
    free_pages(nb->pages, nb->npages);
    ui_surface_destroy(nb->win);
    free(nb);
}

void ui_notebook_close(ui_notebook *nb)
{
    if (nb->closing) return;
    commit_entry(nb, focused_field(nb));
    nb->closing = true;
    ui_app_post(ui_surface_app(nb->win), destroy_later, nb);
}

static void nb_close(ui_surface *s) { ui_notebook_close(ui_surface_data(s)); }

static const ui_surface_ops nb_ops = {
    .draw = nb_draw, .pointer = nb_pointer, .key = nb_key, .resized = nb_resized, .focus = nb_focus, .close = nb_close,
    .drag_motion = nb_drag_motion, .drag_leave = nb_drag_leave, .drop = nb_drop,
};

ui_notebook *ui_notebook_open(ui_app *a, const char *title, const ui_page_desc *pages, size_t n,
                              ui_notebook_set_fn set, ui_notebook_close_fn closed, void *ctx)
{
    ui_notebook *nb = calloc(1, sizeof *nb);
    snprintf(nb->title, sizeof nb->title, "%s", title);
    nb->pages = copy_pages(pages, n);
    nb->npages = n;
    nb->focus = -1;
    nb->pressed_button = -1;
    nb->set = set;
    nb->closed = closed;
    nb->ctx = ctx;
    double most = ROW_H;   /* the tallest page */
    for (size_t i = 0; i < n; i++) {
        double t = 0;
        for (size_t k = 0; k < pages[i].nfields; k++) t += pages[i].fields[k].kind == UI_FIELD_ICON ? ICON_ROW : ROW_H;
        if (t > most) most = t;
    }
    double h = ui_scheme_now.title_h + 2 * ui_scheme_now.border + 8 + TAB_H + 2 * MARGIN + most +
               BUTTON_H + 22 + STATUS_H + 20;
    nb->win = ui_window_new(a, &nb_ops, nb, "org.wps.notebook", 520, h < 280 ? 280 : h);
    ui_window_set_title(nb->win, title);
    return nb;
}

/* New values from the service (after a change elsewhere, or a refused
 * one): fields keep their place and focus. */
void ui_notebook_update(ui_notebook *nb, const ui_page_desc *pages, size_t n)
{
    for (size_t p = 0; p < n && p < nb->npages; p++)
        for (size_t k = 0; k < pages[p].nfields; k++) {
            const ui_field_desc *d = &pages[p].fields[k];
            for (size_t j = 0; j < nb->pages[p].n; j++) {
                struct field *f = &nb->pages[p].f[j];
                if (strcmp(f->key, d->key)) continue;
                value_free(&f->v);
                value_copy(&f->v, &d->value);
                entry_from_value(f);
            }
        }
    damage(nb);
}

void ui_notebook_set_status(ui_notebook *nb, const char *msg)
{
    snprintf(nb->status, sizeof nb->status, "%s", msg ? msg : "");
    ui_surface_damage(nb->win, status_rect(nb));
}

void ui_notebook_show_page(ui_notebook *nb, size_t page) { show_page(nb, page); }

ui_surface *ui_notebook_surface(ui_notebook *nb) { return nb->win; }

size_t ui_notebook_page(ui_notebook *nb) { return nb ? nb->page : 0; }

static struct field *field_by_key(ui_notebook *nb, const char *key)
{
    for (size_t p = 0; p < nb->npages; p++)
        for (size_t k = 0; k < nb->pages[p].n; k++)
            if (!strcmp(nb->pages[p].f[k].key, key)) return &nb->pages[p].f[k];
    return NULL;
}

bool ui_notebook_set_value(ui_notebook *nb, const char *key, const char *text)
{
    struct field *f = nb && !nb->closing ? field_by_key(nb, key) : NULL;
    if (!f || !f->entry || f->kind == UI_FIELD_NUMBER) return false;
    ui_entry_set_text(f->entry, text ? text : "");
    commit_entry(nb, f);   /* (a colour checked; unchanged: nothing applied) */
    damage(nb);
    return true;
}

const char *ui_notebook_field_hint(ui_notebook *nb, const char *key)
{
    struct field *f = nb ? field_by_key(nb, key) : NULL;
    return f && f->nchoices ? f->choices[0] : "";
}
