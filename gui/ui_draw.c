/* Schemes, text (HarfBuzz + FreeType + fontconfig) and drawing helpers. */
#define _GNU_SOURCE
#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

/* ---------------------------------------------------------------- schemes */

#define RGB(r, g, b) { (r) / 255.0, (g) / 255.0, (b) / 255.0, 1 }

/* Our own colours in the spirit of Warp 4: grey faces with 3D edges, a
 * dark blue active title bar, white client areas. */
static const ui_scheme classic = {
    .name = "classic",
    .face = RGB(204, 204, 204), .light = RGB(255, 255, 255), .shadow = RGB(128, 128, 128), .dark = RGB(64, 64, 64),
    .text = RGB(0, 0, 0), .disabled_text = RGB(128, 128, 128),
    .title_active = RGB(0, 0, 128), .title_inactive = RGB(150, 150, 150),
    .title_text = RGB(255, 255, 255), .title_text_inactive = RGB(220, 220, 220),
    .client_bg = RGB(255, 255, 255), .alt_row = RGB(242, 242, 247),
    .selection = RGB(0, 0, 128), .selection_text = RGB(255, 255, 255), .cursor = RGB(0, 0, 0),
    .menu_bg = RGB(204, 204, 204), .menu_text = RGB(0, 0, 0),
    .menu_hilite = RGB(0, 0, 128), .menu_hilite_text = RGB(255, 255, 255),
    .placeholder = RGB(225, 225, 225),
    .shadow_text = RGB(0, 90, 170),
    .font_px = 13, .border = 4, .title_h = 22, .button_w = 18, .button_h = 16,
    .corner_radius = 0, .buttons_on_hover = false,
};

static const ui_scheme dark = {
    .name = "dark",
    .face = RGB(60, 62, 66), .light = RGB(100, 102, 108), .shadow = RGB(30, 31, 34), .dark = RGB(15, 15, 17),
    .text = RGB(230, 230, 230), .disabled_text = RGB(130, 130, 130),
    .title_active = RGB(40, 70, 140), .title_inactive = RGB(70, 72, 76),
    .title_text = RGB(255, 255, 255), .title_text_inactive = RGB(170, 170, 170),
    .client_bg = RGB(36, 37, 40), .alt_row = RGB(42, 43, 47),
    .selection = RGB(40, 70, 140), .selection_text = RGB(255, 255, 255), .cursor = RGB(230, 230, 230),
    .menu_bg = RGB(60, 62, 66), .menu_text = RGB(230, 230, 230),
    .menu_hilite = RGB(40, 70, 140), .menu_hilite_text = RGB(255, 255, 255),
    .placeholder = RGB(55, 56, 60),
    .shadow_text = RGB(120, 180, 255),
    .font_px = 13, .border = 4, .title_h = 22, .button_w = 18, .button_h = 16,
    .corner_radius = 0, .buttons_on_hover = false,
};

/* High contrast: black and white, yellow emphasis, thicker frames. */
static const ui_scheme high_contrast = {
    .name = "high contrast",
    .face = RGB(0, 0, 0), .light = RGB(255, 255, 255), .shadow = RGB(255, 255, 255), .dark = RGB(255, 255, 255),
    .text = RGB(255, 255, 255), .disabled_text = RGB(0, 255, 0),
    .title_active = RGB(255, 255, 0), .title_inactive = RGB(0, 0, 0),
    .title_text = RGB(0, 0, 0), .title_text_inactive = RGB(255, 255, 255),
    .client_bg = RGB(0, 0, 0), .alt_row = RGB(20, 20, 20),
    .selection = RGB(255, 255, 0), .selection_text = RGB(0, 0, 0), .cursor = RGB(255, 255, 255),
    .menu_bg = RGB(0, 0, 0), .menu_text = RGB(255, 255, 255),
    .menu_hilite = RGB(255, 255, 0), .menu_hilite_text = RGB(0, 0, 0),
    .placeholder = RGB(60, 60, 60),
    .shadow_text = RGB(0, 255, 255),
    .font_px = 13, .border = 4, .title_h = 22, .button_w = 18, .button_h = 16,
    .corner_radius = 0, .buttons_on_hover = false,
};

ui_scheme ui_scheme_now;

bool ui_scheme_use(const char *name)
{
    const ui_scheme *s = !strcmp(name, "classic") ? &classic : !strcmp(name, "dark") ? &dark :
                         !strcmp(name, "high contrast") ? &high_contrast : NULL;
    if (!s) return false;
    /* Colours from the scheme; the System object's choices stay. */
    double r = ui_scheme_now.corner_radius, b = ui_scheme_now.border;
    bool h = ui_scheme_now.buttons_on_hover;
    ui_scheme_now = *s;
    ui_scheme_now.corner_radius = r;
    ui_scheme_now.buttons_on_hover = h;
    if (b >= 0) ui_scheme_now.border = b;
    return true;
}

void ui_scheme_set_border(const char *mode)
{
    ui_scheme_now.border = !strcmp(mode, "none") ? 0 : !strcmp(mode, "thin") ? 1 : 4;
}

void ui_scheme_init(void)
{
    const char *n = getenv("WPS_UI_SCHEME");
    ui_scheme_now = n && !strcmp(n, "dark") ? dark : classic;
    const char *r = getenv("WPS_UI_CORNER_RADIUS");
    if (r) {
        double v = atof(r);
        ui_scheme_now.corner_radius = v < 0 ? 0 : v > 16 ? 16 : v;
    }
    const char *b = getenv("WPS_UI_BUTTONS");
    if (b && !strcmp(b, "hover")) ui_scheme_now.buttons_on_hover = true;
}

void ui_set_color(cairo_t *cr, ui_color c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

void ui_fill(cairo_t *cr, ui_rect r, ui_color c)
{
    ui_set_color(cr, c);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
}

void ui_bevel(cairo_t *cr, ui_rect r, bool raised)
{
    const ui_scheme *s = &ui_scheme_now;
    cairo_set_line_width(cr, 1);
    ui_set_color(cr, raised ? s->light : s->shadow);
    cairo_move_to(cr, r.x + 0.5, r.y + r.h - 0.5);
    cairo_line_to(cr, r.x + 0.5, r.y + 0.5);
    cairo_line_to(cr, r.x + r.w - 0.5, r.y + 0.5);
    cairo_stroke(cr);
    ui_set_color(cr, raised ? s->shadow : s->light);
    cairo_move_to(cr, r.x + r.w - 0.5, r.y + 0.5);
    cairo_line_to(cr, r.x + r.w - 0.5, r.y + r.h - 0.5);
    cairo_line_to(cr, r.x + 0.5, r.y + r.h - 0.5);
    cairo_stroke(cr);
}

/* ------------------------------------------------------------------- text */

static FT_Library ft;
static struct face {
    FT_Face ft;              /* Cairo's: it sets its own sizes on it */
    FT_Face shaping;         /* HarfBuzz's, fixed at the scheme's size */
    hb_font_t *hb;
    cairo_font_face_t *cf;
    double ellipsis_w;
} faces[2];
static double ascent;

static FT_Face load_face(const char *pattern, double px)
{
    FcPattern *p = FcNameParse((const FcChar8 *)pattern);
    FcConfigSubstitute(NULL, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult res;
    FcPattern *m = FcFontMatch(NULL, p, &res);
    FcChar8 *file = NULL;
    int index = 0;
    FT_Face f = NULL;
    if (m && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
        FcPatternGetInteger(m, FC_INDEX, 0, &index);
        if (FT_New_Face(ft, (const char *)file, index, &f)) f = NULL;
    }
    if (m) FcPatternDestroy(m);
    FcPatternDestroy(p);
    if (f) FT_Set_Char_Size(f, 0, (FT_F26Dot6)(px * 64), 72, 72);
    return f;
}

/* Shaped runs, cached by (font, string): a folder view redraws the same
 * names as it scrolls. */
#define CACHE_SLOTS 8192
struct run {
    char *s;
    int font;
    unsigned n;
    unsigned *glyphs;
    unsigned *clusters; /* byte offset in the string where each glyph starts */
    double *x;          /* pen position of each glyph, then the total width */
    uint64_t used;
};
static struct run cache[CACHE_SLOTS];
static uint64_t tick;

static uint64_t hash(int font, const char *s)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t)font;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

static struct run *shape(ui_font font, const char *s)
{
    uint64_t h = hash(font, s);
    /* Four-way set: the least recently used of four slots is replaced. */
    size_t base = (size_t)(h % (CACHE_SLOTS / 4)) * 4, victim = base;
    for (size_t i = base; i < base + 4; i++) {
        struct run *r = &cache[i];
        if (r->s && r->font == (int)font && !strcmp(r->s, s)) { r->used = ++tick; return r; }
    }
    for (size_t i = base; i < base + 4; i++) {
        if (!cache[i].s) { victim = i; break; }
        if (cache[i].used < cache[victim].used) victim = i;
    }
    struct run *r = &cache[victim];
    free(r->s);
    free(r->glyphs);
    free(r->clusters);
    free(r->x);
    hb_buffer_t *b = hb_buffer_create();
    hb_buffer_add_utf8(b, s, -1, 0, -1);
    hb_buffer_guess_segment_properties(b);
    hb_shape(faces[font].hb, b, NULL, 0);
    unsigned n;
    hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(b, &n);
    hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(b, &n);
    r->s = strdup(s);
    r->font = (int)font;
    r->n = n;
    r->glyphs = malloc((n ? n : 1) * sizeof *r->glyphs);
    r->clusters = malloc((n ? n : 1) * sizeof *r->clusters);
    r->x = malloc((n + 1) * sizeof *r->x);
    double pen = 0;
    for (unsigned i = 0; i < n; i++) {
        r->glyphs[i] = gi[i].codepoint;
        r->clusters[i] = gi[i].cluster;
        r->x[i] = pen + gp[i].x_offset / 64.0;
        pen += gp[i].x_advance / 64.0;
    }
    r->x[n] = pen;
    r->used = ++tick;
    hb_buffer_destroy(b);
    return r;
}

void ui_text_init(void)
{
    FT_Init_FreeType(&ft);
    const char *pat[2] = { "sans-serif", "sans-serif:bold" };
    for (int i = 0; i < 2; i++) {
        /* Two faces of the same font: Cairo changes its face's size to the
         * device size when it draws (2x on a 2x screen), and HarfBuzz would
         * follow that, shaping some strings at one size and some at the
         * other. Text positions must not depend on which drew last. */
        FT_Face f = load_face(pat[i], ui_scheme_now.font_px), sh = load_face(pat[i], ui_scheme_now.font_px);
        if (!f || !sh) { fprintf(stderr, "wps-ui: no font for %s\n", pat[i]); exit(1); }
        faces[i].ft = f;
        faces[i].shaping = sh;
        faces[i].hb = hb_ft_font_create_referenced(sh);
        hb_ft_font_set_load_flags(faces[i].hb, FT_LOAD_NO_HINTING);
        faces[i].cf = cairo_ft_font_face_create_for_ft_face(f, FT_LOAD_NO_HINTING);
    }
    ascent = faces[0].shaping->size->metrics.ascender / 64.0;
    for (int i = 0; i < 2; i++) faces[i].ellipsis_w = shape((ui_font)i, "\xe2\x80\xa6")->x[1];
}

double ui_text_ascent(void) { return ascent; }

double ui_text_width(ui_font f, const char *s)
{
    struct run *r = shape(f, s);
    return r->x[r->n];
}

double ui_text_draw(cairo_t *cr, ui_font f, double x, double baseline, const char *s, double max_w)
{
    struct run *r = shape(f, s);
    unsigned n = r->n;
    bool cut = false;
    if (max_w > 0 && r->x[n] > max_w) {
        double room = max_w - faces[f].ellipsis_w;
        while (n > 0 && r->x[n] > room) n--;
        cut = true;
    }
    cairo_glyph_t stack[256], *g = n <= 256 ? stack : malloc(n * sizeof *g);
    for (unsigned i = 0; i < n; i++) {
        g[i].index = r->glyphs[i];
        g[i].x = x + r->x[i];
        g[i].y = baseline;
    }
    cairo_set_font_face(cr, faces[f].cf);
    cairo_set_font_size(cr, ui_scheme_now.font_px);
    cairo_show_glyphs(cr, g, (int)n);
    double w = r->x[n];
    if (g != stack) free(g);
    if (cut) {
        struct run *e = shape(f, "\xe2\x80\xa6");
        cairo_glyph_t eg = { e->glyphs[0], x + w, baseline };
        cairo_show_glyphs(cr, &eg, 1);
        w += e->x[1];
    }
    return w;
}

/* Caret and hit testing (left-to-right text): the x of the caret before
 * byte `at`, and the byte offset nearest to x. */
double ui_text_caret_x(ui_font f, const char *s, size_t at)
{
    struct run *r = shape(f, s);
    for (unsigned i = 0; i < r->n; i++)
        if (r->clusters[i] >= at) return r->x[i];
    return r->x[r->n];
}

size_t ui_text_hit(ui_font f, const char *s, double x)
{
    struct run *r = shape(f, s);
    for (unsigned i = 0; i < r->n; i++) {
        double mid = (r->x[i] + r->x[i + 1]) / 2;
        if (x < mid) return r->clusters[i];
    }
    return strlen(s);
}
