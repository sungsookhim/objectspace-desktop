/* Schemes, text (HarfBuzz + FreeType + fontconfig) and drawing helpers. */
#define _GNU_SOURCE
#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>
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

/* The window options (System object), kept apart from the colours so a
 * change of scheme keeps them. */
static struct {
    double corner_radius, border;
    bool buttons_on_hover, title_mirrored, compact;
    int shadows, translucency;
    double font_px;          /* the interface font's size (GNOME's), 0: the scheme's */
} opts = { 0, 4, false, false, false, 0, 0, 0 };

static void apply_opts(void)
{
    ui_scheme_now.corner_radius = opts.corner_radius;
    ui_scheme_now.border = opts.border;
    ui_scheme_now.buttons_on_hover = opts.buttons_on_hover;
    ui_scheme_now.title_mirrored = opts.title_mirrored;
    ui_scheme_now.shadows = opts.shadows;
    ui_scheme_now.translucency = opts.translucency;
    if (opts.font_px > 0) ui_scheme_now.font_px = opts.font_px;
    if (opts.compact) { ui_scheme_now.title_h = 17; ui_scheme_now.button_w = 15; ui_scheme_now.button_h = 13; }
}

bool ui_scheme_use(const char *name)
{
    const ui_scheme *s = !strcmp(name, "classic") ? &classic : !strcmp(name, "dark") ? &dark :
                         !strcmp(name, "high contrast") ? &high_contrast : NULL;
    if (!s) return false;
    ui_scheme_now = *s;   /* colours and metrics from the scheme ... */
    apply_opts();         /* ... the System object's choices on top */
    return true;
}

void ui_scheme_set_border(const char *mode)
{
    opts.border = !strcmp(mode, "none") ? 0 : !strcmp(mode, "thin") ? 1 : 4;
    ui_scheme_now.border = opts.border;
}

bool ui_scheme_set_option(const char *key, const char *v)
{
    if (!strcmp(key, "corner_radius")) { double r = atof(v); opts.corner_radius = r < 0 ? 0 : r > 16 ? 16 : r; }
    else if (!strcmp(key, "buttons")) opts.buttons_on_hover = !strcmp(v, "on hover") || !strcmp(v, "hover");
    else if (!strcmp(key, "border")) opts.border = !strcmp(v, "none") ? 0 : !strcmp(v, "thin") ? 1 : 4;
    else if (!strcmp(key, "shadows")) opts.shadows = !strcmp(v, "strong") ? 2 : !strcmp(v, "subtle") ? 1 : 0;
    else if (!strcmp(key, "translucency")) opts.translucency = !strcmp(v, "menus") ? 2 : !strcmp(v, "title bars") ? 1 : 0;
    else if (!strcmp(key, "title_layout")) { opts.title_mirrored = !strcmp(v, "mirrored"); opts.compact = !strcmp(v, "compact"); }
    else return false;
    /* metrics may change (compact): start from the scheme again */
    ui_scheme_use(ui_scheme_now.name);
    return true;
}

void ui_scheme_init(void)
{
    const char *n = getenv("WPS_UI_SCHEME");
    ui_scheme_now = n && !strcmp(n, "dark") ? dark : classic;
    const char *r = getenv("WPS_UI_CORNER_RADIUS"), *b = getenv("WPS_UI_BUTTONS"), *sh = getenv("WPS_UI_SHADOWS");
    if (r) ui_scheme_set_option("corner_radius", r);
    if (b) ui_scheme_set_option("buttons", b);
    if (sh) ui_scheme_set_option("shadows", sh);
    apply_opts();
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

/* Font slots: 0 and 1 the interface font (regular, bold: GNOME's, or the
 * scheme's), then fonts by name ("12.Helvetica Bold", OS/2's Font Palette
 * names), loaded once each. */
#define MAX_FONTS 32
struct face {
    FT_Face ft;              /* Cairo's: it sets its own sizes on it */
    FT_Face shaping;         /* HarfBuzz's, fixed at the slot's size */
    hb_font_t *hb;
    cairo_font_face_t *cf;
};
static struct slot {
    char spec[160];          /* "" for the interface font's */
    char family[128];
    bool bold, italic;
    double pt;               /* 0: the interface font's size */
    double px;
    struct face f;
    double ascent, descent, ellipsis_w;
    bool loaded;
} slots[MAX_FONTS];
static int nslots = 2;

/* Fallback fonts: for characters a slot's font does not have (CJK, and
 * others), the font fontconfig picks for each one, loaded once per size. */
#define MAX_FALLBACK 48
static struct fallback {
    struct face f;
    char file[512];
    int index;
    bool bold;
    double px;
} fb[MAX_FALLBACK];
static int nfb;
#define CP_SLOTS 4096
static struct { uint32_t key; short face; } cp_cache[CP_SLOTS];   /* key: cp << 5 | slot, +1 (0: empty) */

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
    struct face **face; /* the font of each glyph (the slot's, or a fallback) */
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

static size_t utf8_len(const char *p)
{
    unsigned char c = (unsigned char)*p;
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    for (size_t i = 1; i < n; i++) if (!p[i]) return i;
    return n;
}

static uint32_t utf8_cp(const char *p)
{
    const unsigned char *u = (const unsigned char *)p;
    if (u[0] < 0x80) return u[0];
    if (u[0] < 0xe0) return (uint32_t)(u[0] & 0x1f) << 6 | (u[1] & 0x3f);
    if (u[0] < 0xf0) return (uint32_t)(u[0] & 0x0f) << 12 | (uint32_t)(u[1] & 0x3f) << 6 | (u[2] & 0x3f);
    return (uint32_t)(u[0] & 0x07) << 18 | (uint32_t)(u[1] & 0x3f) << 12 | (uint32_t)(u[2] & 0x3f) << 6 | (u[3] & 0x3f);
}

static char family[256];

static bool load_pair(struct face *fc, const char *file, int index, double px)
{
    FT_Face f, sh;
    if (FT_New_Face(ft, file, index, &f)) return false;
    if (FT_New_Face(ft, file, index, &sh)) { FT_Done_Face(f); return false; }
    FT_Set_Char_Size(f, 0, (FT_F26Dot6)(px * 64), 72, 72);
    FT_Set_Char_Size(sh, 0, (FT_F26Dot6)(px * 64), 72, 72);
    fc->ft = f;
    fc->shaping = sh;
    fc->hb = hb_ft_font_create_referenced(sh);
    hb_ft_font_set_load_flags(fc->hb, FT_LOAD_NO_HINTING);
    fc->cf = cairo_ft_font_face_create_for_ft_face(f, FT_LOAD_NO_HINTING);
    return true;
}

static struct slot *slot_of(ui_font font) { return font >= 0 && font < nslots && slots[font].loaded ? &slots[font] : &slots[0]; }

static void slot_pattern(const struct slot *sl, char *out, size_t n)
{
    snprintf(out, n, "%s%s%s", sl->family, sl->bold ? ":bold" : "", sl->italic ? ":italic" : "");
}

/* The font for the character at s[at]: the slot's font when it has it (or
 * for marks and spaces, which go with what is around them), else a
 * fallback that has it, else the slot's (a box). */
static struct face *face_for(ui_font font, const char *s, size_t at, size_t len)
{
    (void)len;
    struct slot *sl = slot_of(font);
    struct face *main = &sl->f;
    uint32_t cp = utf8_cp(s + at);
    if (cp < 0x300 || FT_Get_Char_Index(main->shaping, cp)) return main;
    uint32_t key = (cp << 5 | (uint32_t)(sl - slots)) + 1;
    size_t slot = (key * 2654435761u) % CP_SLOTS;
    if (cp_cache[slot].key == key) return cp_cache[slot].face < 0 ? main : &fb[cp_cache[slot].face].f;
    short found = -1;
    char pat[300];
    slot_pattern(sl, pat, sizeof pat);
    FcPattern *p = FcNameParse((const FcChar8 *)pat);
    FcCharSet *cs = FcCharSetCreate();
    FcCharSetAddChar(cs, cp);
    FcPatternAddCharSet(p, FC_CHARSET, cs);
    FcConfigSubstitute(NULL, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult res;
    FcPattern *m = FcFontMatch(NULL, p, &res);
    FcChar8 *file = NULL;
    FcCharSet *has = NULL;
    int index = 0;
    if (m && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch &&
        FcPatternGetCharSet(m, FC_CHARSET, 0, &has) == FcResultMatch && FcCharSetHasChar(has, cp)) {
        FcPatternGetInteger(m, FC_INDEX, 0, &index);
        for (int i = 0; i < nfb && found < 0; i++)
            if (!strcmp(fb[i].file, (const char *)file) && fb[i].index == index && fb[i].bold == sl->bold && fb[i].px == sl->px)
                found = (short)i;
        if (found < 0 && nfb < MAX_FALLBACK && load_pair(&fb[nfb].f, (const char *)file, index, sl->px)) {
            snprintf(fb[nfb].file, sizeof fb[nfb].file, "%s", (const char *)file);
            fb[nfb].index = index;
            fb[nfb].bold = sl->bold;
            fb[nfb].px = sl->px;
            found = (short)nfb++;
        }
    }
    if (m) FcPatternDestroy(m);
    FcCharSetDestroy(cs);
    FcPatternDestroy(p);
    cp_cache[slot].key = key;
    cp_cache[slot].face = found;
    return found < 0 ? main : &fb[found].f;
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
    free(r->face);
    free(r->clusters);
    free(r->x);
    r->s = strdup(s);
    r->font = (int)font;
    r->n = 0;
    unsigned cap = 16;
    r->glyphs = malloc(cap * sizeof *r->glyphs);
    r->face = malloc(cap * sizeof *r->face);
    r->clusters = malloc(cap * sizeof *r->clusters);
    r->x = malloc((cap + 1) * sizeof *r->x);
    double pen = 0;
    /* Runs of characters with the same font, each shaped with it. */
    size_t len = strlen(s), at = 0;
    while (at < len) {
        struct face *fc = face_for(font, s, at, len);
        size_t end = at + utf8_len(s + at);
        while (end < len && face_for(font, s, end, len) == fc) end += utf8_len(s + end);
        hb_buffer_t *b = hb_buffer_create();
        hb_buffer_add_utf8(b, s, (int)len, (unsigned)at, (int)(end - at));
        hb_buffer_guess_segment_properties(b);
        hb_shape(fc->hb, b, NULL, 0);
        unsigned n;
        hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(b, &n);
        hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(b, &n);
        if (r->n + n + 1 > cap) {
            while (r->n + n + 1 > cap) cap *= 2;
            r->glyphs = realloc(r->glyphs, cap * sizeof *r->glyphs);
            r->face = realloc(r->face, cap * sizeof *r->face);
            r->clusters = realloc(r->clusters, cap * sizeof *r->clusters);
            r->x = realloc(r->x, (cap + 1) * sizeof *r->x);
        }
        for (unsigned i = 0; i < n; i++) {
            unsigned k = r->n++;
            r->glyphs[k] = gi[i].codepoint;
            r->face[k] = fc;
            r->clusters[k] = gi[i].cluster;
            r->x[k] = pen + gp[i].x_offset / 64.0;
            pen += gp[i].x_advance / 64.0;
        }
        hb_buffer_destroy(b);
        at = end;
    }
    r->x[r->n] = pen;
    r->used = ++tick;
    return r;
}

static bool load_slot(int i);

void ui_text_init(void)
{
    snprintf(family, sizeof family, "sans-serif");
    FT_Init_FreeType(&ft);
    slots[1].bold = true;
    if (!load_slot(0) || !load_slot(1)) { fprintf(stderr, "wps-ui: no font for %s\n", family); exit(1); }
}

/* The interface font (GNOME's, G3): a family and a size in pixels. All
 * text is shaped again, every slot loaded again. */
const char *ui_text_family(void) { return family; }

static void free_face(struct face *f)
{
    if (f->hb) hb_font_destroy(f->hb);
    if (f->cf) cairo_font_face_destroy(f->cf);   /* (f->ft belongs to the Cairo face: freed with it) */
    if (f->shaping) FT_Done_Face(f->shaping);
    memset(f, 0, sizeof *f);
}

static void reload_all(void)
{
    for (size_t i = 0; i < CACHE_SLOTS; i++) {
        free(cache[i].s); free(cache[i].glyphs); free(cache[i].face); free(cache[i].clusters); free(cache[i].x);
        memset(&cache[i], 0, sizeof cache[i]);
    }
    for (int i = 0; i < nfb; i++) free_face(&fb[i].f);
    memset(fb, 0, sizeof fb);
    nfb = 0;
    memset(cp_cache, 0, sizeof cp_cache);
    for (int i = 0; i < nslots; i++) {
        free_face(&slots[i].f);
        slots[i].loaded = false;
        load_slot(i);
    }
}

void ui_text_set_font(const char *fam, double px)
{
    if (!fam || !*fam || px < 6 || px > 72) return;
    if (!strcmp(fam, family) && px == ui_scheme_now.font_px) return;
    snprintf(family, sizeof family, "%s", fam);
    ui_scheme_now.font_px = opts.font_px = px;
    reload_all();
}

/* A slot's faces: the interface font's (slots 0, 1: the family now, at the
 * scheme's size) or its own. */
static bool load_slot(int i)
{
    struct slot *sl = &slots[i];
    if (i < 2) {
        snprintf(sl->family, sizeof sl->family, "%s", family);
        sl->px = ui_scheme_now.font_px;
    } else {
        sl->px = round(sl->pt * 96.0 / 72.0);
        if (sl->px < 6) sl->px = 6;
        if (sl->px > 96) sl->px = 96;
    }
    char pat[300];
    slot_pattern(sl, pat, sizeof pat);
    /* Two faces of the same font: Cairo changes its face's size to the
     * device size when it draws (2x on a 2x screen), and HarfBuzz would
     * follow that, shaping some strings at one size and some at the other.
     * Text positions must not depend on which drew last. */
    FT_Face f = load_face(pat, sl->px), sh = load_face(pat, sl->px);
    if ((!f || !sh) && i < 2 && strcmp(family, "sans-serif")) {   /* not installed: back to the default */
        if (f) FT_Done_Face(f);
        if (sh) FT_Done_Face(sh);
        snprintf(family, sizeof family, "sans-serif");
        return load_slot(i);
    }
    if (!f || !sh) {
        if (f) FT_Done_Face(f);
        if (sh) FT_Done_Face(sh);
        return false;
    }
    sl->f.ft = f;
    sl->f.shaping = sh;
    sl->f.hb = hb_ft_font_create_referenced(sh);
    hb_ft_font_set_load_flags(sl->f.hb, FT_LOAD_NO_HINTING);
    sl->f.cf = cairo_ft_font_face_create_for_ft_face(f, FT_LOAD_NO_HINTING);
    sl->ascent = sh->size->metrics.ascender / 64.0;
    sl->descent = -sh->size->metrics.descender / 64.0;
    sl->loaded = true;
    sl->ellipsis_w = shape((ui_font)i, "\xe2\x80\xa6")->x[1];
    return true;
}

/* "12.Helvetica Bold Italic": size in points, family, styles. */
static bool parse_spec(const char *spec, double *pt, char *fam, size_t fn, bool *bold, bool *italic)
{
    char *end;
    long v = strtol(spec, &end, 10);   /* (not strtod: "8.Sans" would be 8.0, the dot taken) */
    if (end == spec || *end != '.' || v < 4 || v > 72) return false;
    char f[160];
    snprintf(f, sizeof f, "%s", end + 1);
    *bold = *italic = false;
    for (;;) {
        size_t l = strlen(f);
        if (l > 5 && !strcasecmp(f + l - 5, " bold")) { *bold = true; f[l - 5] = 0; continue; }
        if (l > 7 && !strcasecmp(f + l - 7, " italic")) { *italic = true; f[l - 7] = 0; continue; }
        break;
    }
    if (!f[0]) return false;
    *pt = (double)v;
    snprintf(fam, fn, "%s", f);
    return true;
}

ui_font ui_font_named(const char *spec)
{
    if (!spec || !*spec) return UI_FONT_REGULAR;
    for (int i = 2; i < nslots; i++) if (!strcmp(slots[i].spec, spec)) return slots[i].loaded ? i : UI_FONT_REGULAR;
    double pt;
    char fam[128];
    bool bold, italic;
    if (!parse_spec(spec, &pt, fam, sizeof fam, &bold, &italic) || nslots >= MAX_FONTS) return UI_FONT_REGULAR;
    struct slot *sl = &slots[nslots];
    memset(sl, 0, sizeof *sl);
    snprintf(sl->spec, sizeof sl->spec, "%s", spec);
    snprintf(sl->family, sizeof sl->family, "%s", fam);
    sl->pt = pt;
    sl->bold = bold;
    sl->italic = italic;
    int i = nslots++;
    return load_slot(i) ? i : UI_FONT_REGULAR;   /* (a failed one stays, not loaded: asked again, the interface font) */
}

bool ui_font_valid(const char *spec)
{
    double pt;
    char fam[128];
    bool b, it;
    return spec && parse_spec(spec, &pt, fam, sizeof fam, &b, &it);
}

double ui_font_ascent(ui_font f) { return slot_of(f)->ascent; }
double ui_font_px(ui_font f) { return slot_of(f)->px; }
double ui_font_line_h(ui_font f)
{
    struct slot *sl = slot_of(f);
    double h = ceil(sl->ascent + sl->descent);
    return h > sl->px ? h : ceil(sl->px * 1.2);
}

double ui_text_ascent(void) { return slots[0].ascent; }

double ui_text_width(ui_font f, const char *s)
{
    struct run *r = shape(f, s);
    return r->x[r->n];
}

double ui_text_draw(cairo_t *cr, ui_font f, double x, double baseline, const char *s, double max_w)
{
    struct slot *sl = slot_of(f);
    struct run *r = shape(f, s);
    unsigned n = r->n;
    bool cut = false;
    if (max_w > 0 && r->x[n] > max_w) {
        double room = max_w - sl->ellipsis_w;
        while (n > 0 && r->x[n] > room) n--;
        cut = true;
    }
    cairo_glyph_t stack[256], *g = n <= 256 ? stack : malloc(n * sizeof *g);
    for (unsigned i = 0; i < n; i++) {
        g[i].index = r->glyphs[i];
        g[i].x = x + r->x[i];
        g[i].y = baseline;
    }
    for (unsigned i = 0; i < n;) {   /* a group of glyphs per font */
        unsigned j = i + 1;
        while (j < n && r->face[j] == r->face[i]) j++;
        cairo_set_font_face(cr, r->face[i]->cf);
        cairo_set_font_size(cr, sl->px);
        cairo_show_glyphs(cr, g + i, (int)(j - i));
        i = j;
    }
    double w = r->x[n];
    if (g != stack) free(g);
    if (cut) {
        struct run *e = shape(f, "\xe2\x80\xa6");
        cairo_glyph_t eg = { e->glyphs[0], x + w, baseline };
        cairo_set_font_face(cr, e->face[0]->cf);
        cairo_set_font_size(cr, sl->px);
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
