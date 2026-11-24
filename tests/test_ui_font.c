/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Font Palette names with styles ("10.Noto Sans SemiBold"), against a
 * made-up font list (not the machine's fonts). */
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include "check.h"
#include "ui.h"

/* The fonts "installed": Noto Sans (Regular, Bold, SemiBold, Light,
 * Light Italic), Noto Sans Mono (Regular), Cantarell (Regular). */
static bool fake_has(const char *family, const char *style)
{
    static const char *fonts[][2] = {
        { "Noto Sans", "Regular" }, { "Noto Sans", "Bold" }, { "Noto Sans", "SemiBold" },
        { "Noto Sans", "Light" }, { "Noto Sans", "Light Italic" },
        { "Noto Sans Mono", "Regular" }, { "Cantarell", "Regular" },
    };
    for (size_t i = 0; i < sizeof fonts / sizeof fonts[0]; i++)
        if (!strcasecmp(fonts[i][0], family) && (!style || !strcasecmp(fonts[i][1], style))) return true;
    return false;
}

struct split { char fam[128], style[64]; bool bold, italic, ok; };

static struct split split(const char *name)
{
    struct split r;
    r.ok = ui_font_split(name, fake_has, r.fam, sizeof r.fam, r.style, sizeof r.style, &r.bold, &r.italic);
    return r;
}

TEST(a_family_alone)
{
    struct split r = split("Noto Sans");
    CHECK(r.ok);
    CHECK_STR(r.fam, "Noto Sans");
    CHECK_STR(r.style, "");
    CHECK(!r.bold && !r.italic);
    r = split("Noto Sans Mono");   /* (a family that starts with another's name) */
    CHECK_STR(r.fam, "Noto Sans Mono");
    CHECK_STR(r.style, "");
}

TEST(a_style_the_family_has)
{
    struct split r = split("Noto Sans SemiBold");
    CHECK_STR(r.fam, "Noto Sans");
    CHECK_STR(r.style, "SemiBold");
    r = split("Noto Sans Light Italic");   /* two words */
    CHECK_STR(r.fam, "Noto Sans");
    CHECK_STR(r.style, "Light Italic");
    r = split("noto sans light");   /* (fontconfig's names are not case sensitive) */
    CHECK_STR(r.style, "light");
}

TEST(bold_and_italic_as_before)
{
    struct split r = split("Noto Sans Bold");
    CHECK_STR(r.fam, "Noto Sans");
    CHECK_STR(r.style, "");
    CHECK(r.bold && !r.italic);
    r = split("Cantarell Bold Italic");   /* (no such font: made by fontconfig) */
    CHECK_STR(r.fam, "Cantarell");
    CHECK(r.bold && r.italic);
    r = split("Noto Sans Regular");
    CHECK_STR(r.fam, "Noto Sans");
    CHECK(!r.bold && !r.italic && !r.style[0]);
}

TEST(names_fontconfig_finds_a_font_for)
{
    /* "Sans", "Helvetica": not listed families, but fontconfig matches them */
    struct split r = split("Sans Bold");
    CHECK_STR(r.fam, "Sans");
    CHECK(r.bold);
    r = split("Helvetica Bold Italic");
    CHECK_STR(r.fam, "Helvetica");
    CHECK(r.bold && r.italic);
    r = split("Noto Sans Heavy");   /* a style it has not: the whole name, as before */
    CHECK_STR(r.fam, "Noto Sans Heavy");
    CHECK_STR(r.style, "");
    CHECK(!split("").ok);
    CHECK(!split(" Bold").ok);
}

TEST(sizes_follow_the_text_scale)
{
    ui_font f = ui_font_named("10.Sans");
    double px = ui_font_px(f);
    CHECK(px == 13);                       /* 10 points at 96 dpi */
    ui_text_set_scale(1.5);
    CHECK(ui_font_px(f) == 20);
    ui_text_set_scale(1);
    CHECK(ui_font_px(f) == 13);
}

TEST(the_dialogs_font)
{
    CHECK(ui_font_px(UI_FONT_DIALOG) == ui_font_px(UI_FONT_REGULAR));
    ui_set_dialog_font("18.Sans");
    CHECK(ui_font_px(UI_FONT_DIALOG) == 24);
    CHECK(ui_font_px(UI_FONT_DIALOG_BOLD) == 24);
    CHECK(ui_text_width(UI_FONT_DIALOG, "Settings") > ui_text_width(UI_FONT_REGULAR, "Settings"));
    ui_set_dialog_font("");
    CHECK(ui_text_width(UI_FONT_DIALOG, "Settings") == ui_text_width(UI_FONT_REGULAR, "Settings"));
}

int main(void)
{
    ui_scheme_init();
    ui_text_init();
    RUN(a_family_alone);
    RUN(a_style_the_family_has);
    RUN(bold_and_italic_as_before);
    RUN(names_fontconfig_finds_a_font_for);
    RUN(sizes_follow_the_text_scale);
    RUN(the_dialogs_font);
    return CHECK_SUMMARY();
}
