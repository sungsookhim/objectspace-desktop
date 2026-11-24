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
#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "ui.h"

/* ---------------------------------------------------------------- schemes */

#define RGB(r, g, b) { (r) / 255.0, (g) / 255.0, (b) / 255.0, 1 }
#define METERS .meter_cpu = RGB(51, 179, 77), .meter_memory = RGB(64, 115, 217), .meter_disk = RGB(217, 140, 38), \
               .meter_network = RGB(153, 89, 204)

/* Our own colours in the spirit of Warp 4: grey faces with 3D edges, a
 * dark blue active title bar, white client areas. */
static const ui_scheme classic = {
    .name = "classic",
    .face = RGB(204, 204, 204), .bar = RGB(204, 204, 204), .bar_text = RGB(0, 0, 0), .light = RGB(255, 255, 255), .shadow = RGB(128, 128, 128), .dark = RGB(64, 64, 64),
    .text = RGB(0, 0, 0), .disabled_text = RGB(128, 128, 128),
    .title_active = RGB(0, 0, 128), .title_inactive = RGB(150, 150, 150),
    .title_text = RGB(255, 255, 255), .title_text_inactive = RGB(220, 220, 220),
    .client_bg = RGB(255, 255, 255), .alt_row = RGB(242, 242, 247),
    .selection = RGB(0, 0, 128), .selection_text = RGB(255, 255, 255), .cursor = RGB(0, 0, 0),
    .menu_bg = RGB(204, 204, 204), .menu_text = RGB(0, 0, 0),
    .menu_hilite = RGB(0, 0, 128), .menu_hilite_text = RGB(255, 255, 255),
    .placeholder = RGB(225, 225, 225),
    .shadow_text = RGB(0, 90, 170),
    .desktop = { 0, 0.5, 0.5, 1 }, .desktop_title = RGB(255, 255, 255), .desktop_shadow_title = { 0.75, 0.85, 1.0, 1 },
    .tooltip_bg = { 1.0, 1.0, 0.88, 1 }, .tooltip_text = RGB(0, 0, 0),
    .urgent_bg = { 1.0, 0.93, 0.85, 1 }, .urgent_text = { 0.7, 0, 0, 1 },
    METERS,
    .font_px = 13, .border = 4, .title_h = 22, .button_w = 18, .button_h = 16,
    .corner_radius = 0, .buttons_on_hover = false,
};

static const ui_scheme dark = {
    .name = "dark",
    .face = RGB(60, 62, 66), .bar = RGB(60, 62, 66), .bar_text = RGB(230, 230, 230), .light = RGB(100, 102, 108), .shadow = RGB(30, 31, 34), .dark = RGB(15, 15, 17),
    .text = RGB(230, 230, 230), .disabled_text = RGB(130, 130, 130),
    .title_active = RGB(40, 70, 140), .title_inactive = RGB(70, 72, 76),
    .title_text = RGB(255, 255, 255), .title_text_inactive = RGB(170, 170, 170),
    .client_bg = RGB(36, 37, 40), .alt_row = RGB(42, 43, 47),
    .selection = RGB(40, 70, 140), .selection_text = RGB(255, 255, 255), .cursor = RGB(230, 230, 230),
    .menu_bg = RGB(60, 62, 66), .menu_text = RGB(230, 230, 230),
    .menu_hilite = RGB(40, 70, 140), .menu_hilite_text = RGB(255, 255, 255),
    .placeholder = RGB(55, 56, 60),
    .shadow_text = RGB(120, 180, 255),
    .desktop = { 0, 0.5, 0.5, 1 }, .desktop_title = RGB(255, 255, 255), .desktop_shadow_title = { 0.75, 0.85, 1.0, 1 },
    .tooltip_bg = { 1.0, 1.0, 0.88, 1 }, .tooltip_text = RGB(0, 0, 0),
    .urgent_bg = { 1.0, 0.93, 0.85, 1 }, .urgent_text = { 0.7, 0, 0, 1 },
    METERS,
    .font_px = 13, .border = 4, .title_h = 22, .button_w = 18, .button_h = 16,
    .corner_radius = 0, .buttons_on_hover = false,
};

/* High contrast: black and white, yellow emphasis, thicker frames. */
static const ui_scheme high_contrast = {
    .name = "high contrast",
    .face = RGB(0, 0, 0), .bar = RGB(0, 0, 0), .bar_text = RGB(255, 255, 255), .light = RGB(255, 255, 255), .shadow = RGB(255, 255, 255), .dark = RGB(255, 255, 255),
    .text = RGB(255, 255, 255), .disabled_text = RGB(0, 255, 0),
    .title_active = RGB(255, 255, 0), .title_inactive = RGB(0, 0, 0),
    .title_text = RGB(0, 0, 0), .title_text_inactive = RGB(255, 255, 255),
    .client_bg = RGB(0, 0, 0), .alt_row = RGB(20, 20, 20),
    .selection = RGB(255, 255, 0), .selection_text = RGB(0, 0, 0), .cursor = RGB(255, 255, 255),
    .menu_bg = RGB(0, 0, 0), .menu_text = RGB(255, 255, 255),
    .menu_hilite = RGB(255, 255, 0), .menu_hilite_text = RGB(0, 0, 0),
    .placeholder = RGB(60, 60, 60),
    .shadow_text = RGB(0, 255, 255),
    .desktop = { 0, 0.5, 0.5, 1 }, .desktop_title = RGB(255, 255, 255), .desktop_shadow_title = { 0.75, 0.85, 1.0, 1 },
    .tooltip_bg = { 1.0, 1.0, 0.88, 1 }, .tooltip_text = RGB(0, 0, 0),
    .urgent_bg = { 1.0, 0.93, 0.85, 1 }, .urgent_text = { 0.7, 0, 0, 1 },
    METERS,
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

/* ---- schemes defined at run time (from scheme objects) */

#define SCHEME_COLOR(f) { #f, offsetof(ui_scheme, f), 'c' }
#define SCHEME_NUMBER(f) { #f, offsetof(ui_scheme, f), 'n' }
#define SCHEME_TEXT(f) { #f, offsetof(ui_scheme, f), 's' }
static const struct { const char *key; size_t off; char type; } scheme_keys[] = {
    SCHEME_COLOR(face), SCHEME_COLOR(light), SCHEME_COLOR(shadow), SCHEME_COLOR(dark), SCHEME_COLOR(text),
    SCHEME_COLOR(disabled_text), SCHEME_COLOR(title_active), SCHEME_COLOR(title_inactive), SCHEME_COLOR(title_text),
    SCHEME_COLOR(title_text_inactive), SCHEME_COLOR(client_bg), SCHEME_COLOR(alt_row), SCHEME_COLOR(selection),
    SCHEME_COLOR(selection_text), SCHEME_COLOR(cursor), SCHEME_COLOR(menu_bg), SCHEME_COLOR(menu_text),
    SCHEME_COLOR(menu_hilite), SCHEME_COLOR(menu_hilite_text), SCHEME_COLOR(placeholder), SCHEME_COLOR(shadow_text),
    SCHEME_COLOR(desktop), SCHEME_COLOR(desktop_title), SCHEME_COLOR(desktop_shadow_title),
    SCHEME_COLOR(tooltip_bg), SCHEME_COLOR(tooltip_text), SCHEME_COLOR(urgent_bg), SCHEME_COLOR(urgent_text),
    SCHEME_COLOR(bar), SCHEME_COLOR(bar_text), SCHEME_COLOR(meter_cpu), SCHEME_COLOR(meter_memory), SCHEME_COLOR(meter_disk),
    SCHEME_COLOR(meter_network),
    SCHEME_NUMBER(font_px), SCHEME_NUMBER(border), SCHEME_NUMBER(title_h), SCHEME_NUMBER(button_w), SCHEME_NUMBER(button_h),
    SCHEME_TEXT(font_dialogs),
};

#define MAX_DEFINED 32
static struct { char name[64]; ui_scheme s; } defined[MAX_DEFINED];
static size_t ndefined;

static const ui_scheme *builtin(const char *name)
{
    return !strcasecmp(name, "classic") ? &classic : !strcasecmp(name, "dark") ? &dark :
           !strcasecmp(name, "high contrast") ? &high_contrast : NULL;
}

static const ui_scheme *find_scheme(const char *name)
{
    for (size_t i = 0; name && i < ndefined; i++)
        if (!strcasecmp(defined[i].name, name)) return &defined[i].s;
    return name ? builtin(name) : NULL;
}

static bool hex_color(const char *v, size_t n, ui_color *c)
{
    if (n != 7 || v[0] != '#') return false;
    unsigned x[3];
    char b[8];
    memcpy(b, v, 7);
    b[7] = 0;
    if (sscanf(b + 1, "%2x%2x%2x", &x[0], &x[1], &x[2]) != 3) return false;
    *c = (ui_color){ x[0] / 255.0, x[1] / 255.0, x[2] / 255.0, 1 };
    return true;
}

bool ui_scheme_define(const char *name, const char *base, const char *settings)
{
    if (!name || !*name) return false;
    const ui_scheme *b = base ? find_scheme(base) : NULL;
    ui_scheme s = b ? *b : classic;
    size_t k = 0;
    while (k < ndefined && strcasecmp(defined[k].name, name)) k++;
    if (k == ndefined) {
        if (ndefined == MAX_DEFINED) return false;
        ndefined++;
    }
    snprintf(defined[k].name, sizeof defined[k].name, "%s", name);
    s.name = defined[k].name;
    bool bar = false, bar_text = false;   /* (not given: the face's and text's) */
    for (const char *p = settings; p && *p;) {
        const char *e = strchr(p, ';'), *eq = memchr(p, '=', e ? (size_t)(e - p) : strlen(p));
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (eq) {
            size_t kl = (size_t)(eq - p), vl = len - kl - 1;
            for (size_t i = 0; i < sizeof scheme_keys / sizeof *scheme_keys; i++) {
                if (strlen(scheme_keys[i].key) != kl || strncmp(scheme_keys[i].key, p, kl)) continue;
                char *at = (char *)&s + scheme_keys[i].off;
                if (scheme_keys[i].type == 'c') {
                    bool ok = hex_color(eq + 1, vl, (ui_color *)at);
                    if (ok && !strcmp(scheme_keys[i].key, "bar")) bar = true;
                    if (ok && !strcmp(scheme_keys[i].key, "bar_text")) bar_text = true;
                }
                else if (scheme_keys[i].type == 's') snprintf(at, UI_SCHEME_TEXT_MAX, "%.*s", (int)vl, eq + 1);
                else if (vl) *(double *)at = atof(eq + 1);
            }
        }
        p = e ? e + 1 : NULL;
    }
    if (!bar) s.bar = s.face;
    if (!bar_text) s.bar_text = s.text;
    defined[k].s = s;
    return true;
}

char *ui_scheme_describe(const char *name)
{
    const ui_scheme *s = find_scheme(name);
    if (!s) return NULL;
    size_t cap = 2048, n = 0;
    char *out = malloc(cap);
    out[0] = 0;
    for (size_t i = 0; i < sizeof scheme_keys / sizeof *scheme_keys; i++) {
        const char *at = (const char *)s + scheme_keys[i].off;
        if (scheme_keys[i].type == 'c') {
            const ui_color *c = (const ui_color *)at;
            n += (size_t)snprintf(out + n, cap - n, "%s=#%02x%02x%02x;", scheme_keys[i].key, (int)(c->r * 255 + 0.5),
                                  (int)(c->g * 255 + 0.5), (int)(c->b * 255 + 0.5));
        } else if (scheme_keys[i].type == 's') {
            n += (size_t)snprintf(out + n, cap - n, "%s=%s;", scheme_keys[i].key, at);
        } else {
            n += (size_t)snprintf(out + n, cap - n, "%s=%g;", scheme_keys[i].key, *(const double *)at);
        }
    }
    return out;
}

bool ui_scheme_use(const char *name)
{
    const ui_scheme *s = find_scheme(name);
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
    char style[64];          /* fontconfig's style ("SemiBold"), or "" and bold, italic */
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
static double text_scale = 1;

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

/* The Dialogs font (notebooks, dialogs, Find, the window list, the
 * palettes): an alias of a slot, the interface font's unless set. */
static ui_font dialog_font = UI_FONT_REGULAR, dialog_bold = UI_FONT_BOLD;
static ui_font real_font(ui_font f) { return f == UI_FONT_DIALOG ? dialog_font : f == UI_FONT_DIALOG_BOLD ? dialog_bold : f; }

static struct slot *slot_of(ui_font font)
{
    font = real_font(font);
    return font >= 0 && font < nslots && slots[font].loaded ? &slots[font] : &slots[0];
}

static void slot_pattern(const struct slot *sl, char *out, size_t n)
{
    if (sl->style[0]) snprintf(out, n, "%s:style=%s", sl->family, sl->style);
    else snprintf(out, n, "%s%s%s", sl->family, sl->bold ? ":bold" : "", sl->italic ? ":italic" : "");
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
    font = real_font(font);
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

/* GNOME's text scaling factor: the Font Palette's sizes (points) are
 * times it, as the interface font's are. */
void ui_text_set_scale(double scale)
{
    if (scale < 0.5 || scale > 3) scale = 1;
    if (scale == text_scale) return;
    text_scale = scale;
    reload_all();
}
double ui_text_scale(void) { return text_scale; }

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
        sl->px = round(sl->pt * 96.0 / 72.0 * text_scale);
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

/* A Font Palette name's family and style: "Noto Sans SemiBold Italic" is
 * the family "Noto Sans" in its style "SemiBold Italic" when the family
 * has it; else " Bold" and " Italic" at the end are the styles (made by
 * fontconfig if the family has none), and the rest the family ("Sans",
 * "Helvetica": names fontconfig finds a font for). */
bool ui_font_split(const char *name, bool (*has)(const char *family, const char *style),
                   char *fam, size_t fn, char *style, size_t sn, bool *bold, bool *italic)
{
    char f[160];
    snprintf(f, sizeof f, "%s", name);
    *bold = *italic = false;
    style[0] = 0;
    /* the longest family the words start with, the rest its style */
    for (char *sp = strrchr(f, ' '); sp && has; ) {
        *sp = 0;
        const char *st = sp + 1;
        if (*f && has(f, NULL)) {
            if (!strcasecmp(st, "Regular")) { snprintf(fam, fn, "%s", f); return true; }
            if (!strcasecmp(st, "Bold") || !strcasecmp(st, "Italic") || !strcasecmp(st, "Bold Italic") || !strcasecmp(st, "Oblique")) {
                *bold = strncasecmp(st, "Bold", 4) == 0;
                *italic = !*bold || strcasecmp(st, "Bold") != 0;
                snprintf(fam, fn, "%s", f);
                return true;
            }
            if (has(f, st)) { snprintf(fam, fn, "%s", f); snprintf(style, sn, "%s", st); return true; }
        }
        char *prev = strrchr(f, ' ');
        *sp = ' ';
        sp = prev;
    }
    for (;;) {
        size_t l = strlen(f);
        if (l >= 5 && !strcasecmp(f + l - 5, " bold")) { *bold = true; f[l - 5] = 0; continue; }
        if (l >= 7 && !strcasecmp(f + l - 7, " italic")) { *italic = true; f[l - 7] = 0; continue; }
        break;
    }
    if (!f[0] || f[0] == ' ') return false;
    snprintf(fam, fn, "%s", f);
    return true;
}

/* fontconfig: is there a font of this family (and style)? */
static bool fc_has(const char *family, const char *style)
{
    FcPattern *p = FcPatternCreate();
    FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)family);
    if (style) FcPatternAddString(p, FC_STYLE, (const FcChar8 *)style);
    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, NULL);
    FcFontSet *fs = FcFontList(NULL, p, os);
    bool yes = fs && fs->nfont > 0;
    if (fs) FcFontSetDestroy(fs);
    FcObjectSetDestroy(os);
    FcPatternDestroy(p);
    return yes;
}

/* A family's styles, as fontconfig lists them ("Regular", "Bold", "Light
 * Italic", ...), sorted, at most max; returns how many. */
size_t ui_font_styles(const char *family, char (*out)[64], size_t max)
{
    FcPattern *p = FcPatternCreate();
    FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)family);
    FcObjectSet *os = FcObjectSetBuild(FC_STYLE, FC_WEIGHT, FC_SLANT, NULL);
    FcFontSet *fs = FcFontList(NULL, p, os);
    size_t n = 0;
    int key[256];
    for (int i = 0; fs && i < fs->nfont && n < max && n < 256; i++) {
        FcChar8 *st;
        if (FcPatternGetString(fs->fonts[i], FC_STYLE, 0, &st) != FcResultMatch) continue;
        bool dup = false;
        for (size_t k = 0; k < n; k++) if (!strcasecmp(out[k], (const char *)st)) dup = true;
        if (dup) continue;
        int w = 80, sl = 0;
        FcPatternGetInteger(fs->fonts[i], FC_WEIGHT, 0, &w);
        FcPatternGetInteger(fs->fonts[i], FC_SLANT, 0, &sl);
        int kk = w * 2 + (sl ? 1 : 0);   /* by weight, upright first */
        size_t at = n;
        while (at > 0 && key[at - 1] > kk) { memcpy(out[at], out[at - 1], 64); key[at] = key[at - 1]; at--; }
        snprintf(out[at], 64, "%s", (const char *)st);
        key[at] = kk;
        n++;
    }
    if (fs) FcFontSetDestroy(fs);
    FcObjectSetDestroy(os);
    FcPatternDestroy(p);
    return n;
}

/* "12.Helvetica Bold Italic", "10.Noto Sans SemiBold": size in points,
 * family, style. */
static bool parse_spec(const char *spec, double *pt, char *fam, size_t fn, char *style, size_t sn, bool *bold, bool *italic)
{
    char *end;
    long v = strtol(spec, &end, 10);   /* (not strtod: "8.Sans" would be 8.0, the dot taken) */
    if (end == spec || *end != '.' || v < 4 || v > 72) return false;
    if (!ui_font_split(end + 1, ft ? fc_has : NULL, fam, fn, style, sn, bold, italic)) return false;
    *pt = (double)v;
    return true;
}

ui_font ui_font_named(const char *spec)
{
    if (!spec || !*spec) return UI_FONT_REGULAR;
    for (int i = 2; i < nslots; i++) if (!strcmp(slots[i].spec, spec)) return slots[i].loaded ? i : UI_FONT_REGULAR;
    double pt;
    char fam[128], style[64];
    bool bold, italic;
    if (!parse_spec(spec, &pt, fam, sizeof fam, style, sizeof style, &bold, &italic) || nslots >= MAX_FONTS) return UI_FONT_REGULAR;
    struct slot *sl = &slots[nslots];
    memset(sl, 0, sizeof *sl);
    snprintf(sl->spec, sizeof sl->spec, "%s", spec);
    snprintf(sl->family, sizeof sl->family, "%s", fam);
    snprintf(sl->style, sizeof sl->style, "%s", style);
    sl->pt = pt;
    sl->bold = bold;
    sl->italic = italic;
    int i = nslots++;
    return load_slot(i) ? i : UI_FONT_REGULAR;   /* (a failed one stays, not loaded: asked again, the interface font) */
}

bool ui_font_valid(const char *spec)
{
    double pt;
    char fam[128], style[64];
    bool b, it;
    return spec && parse_spec(spec, &pt, fam, sizeof fam, style, sizeof style, &b, &it);
}

/* The Dialogs font: a Font Palette name, "" the interface font. Its bold
 * (a notebook's open tab) is the same family's. */
void ui_set_dialog_font(const char *spec)
{
    ui_font f = spec && *spec ? ui_font_named(spec) : UI_FONT_REGULAR;
    ui_font b = UI_FONT_BOLD;
    if (f != UI_FONT_REGULAR) {
        char bs[200];
        snprintf(bs, sizeof bs, "%g.%s Bold", slots[f].pt, slots[f].family);
        b = slots[f].bold && !slots[f].style[0] ? f : ui_font_named(bs);
    }
    dialog_font = f;
    dialog_bold = b;
}
bool ui_dialog_font_is_set(void) { return dialog_font != UI_FONT_REGULAR; }
bool ui_font_is_interface(ui_font f) { return real_font(f) == UI_FONT_REGULAR; }

/* Dialogs' rows and widths, made for a 13-pixel font (a line of 18): how
 * much taller and wider the Dialogs font makes them. */
double ui_dialog_grow(void) { return fmax(0, ui_font_line_h(UI_FONT_DIALOG) - 18); }
double ui_dialog_wider(void) { return fmax(1, ui_font_px(UI_FONT_DIALOG) / 13); }

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
