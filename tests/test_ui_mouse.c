/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Mouse mappings (as OS/2's Mouse object): parsing, matching, loading. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "ui.h"

static ui_pointer_event press(int button, unsigned mods, int clicks)
{
    ui_pointer_event e = { .kind = UI_PRESS, .button = button, .mods = mods, .clicks = clicks };
    return e;
}

TEST(defaults_are_os2s)
{
    ui_pointer_event b1 = press(UI_BTN1, 0, 1), b1d = press(UI_BTN1, 0, 2), b2 = press(UI_BTN2, 0, 1),
                     alt1 = press(UI_BTN1, UI_MOD_ALT, 1), ctrl1 = press(UI_BTN1, UI_MOD_CTRL, 1);
    CHECK(ui_mouse_is(UI_ACT_SELECT, &b1));
    CHECK(ui_mouse_is(UI_ACT_SELECT, &ctrl1));            /* Ctrl changes a selection, still select */
    CHECK(!ui_mouse_is(UI_ACT_OPEN, &b1));
    CHECK(ui_mouse_is(UI_ACT_OPEN, &b1d));
    CHECK(ui_mouse_is(UI_ACT_POPUP, &b2));
    CHECK(ui_mouse_is(UI_ACT_EDIT_TITLE, &alt1));
    CHECK(!ui_mouse_is(UI_ACT_SELECT, &alt1));            /* Alt+button 1 is edit title, not select */
    CHECK(ui_mouse_is(UI_ACT_DRAG, &b2));
}

TEST(chords_parse)
{
    ui_chord c;
    CHECK(ui_mouse_parse_chord("ctrl+shift+button3 double", &c) && c.button == 3 &&
          c.mods == (UI_MOD_CTRL | UI_MOD_SHIFT) && c.clicks == 2);
    CHECK(ui_mouse_parse_chord("Alt + Button1", &c) && c.button == 1 && c.mods == UI_MOD_ALT && c.clicks == 1);
    CHECK(!ui_mouse_parse_chord("button4", &c));
    CHECK(!ui_mouse_parse_chord("alt", &c));
    CHECK(!ui_mouse_parse_chord("hyper+button1", &c));
}

TEST(a_config_file_remaps)
{
    char dir[] = "/tmp/osd-mouse-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[256];
    snprintf(path, sizeof path, "%s/mouse.conf", dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "# a left-hander who wants menus on the middle button\n"
               "popup_menu = button3\nedit_title = shift+button1   # as in some OS/2 setups\n"
               "left_handed = yes\ndouble_click_ms = 600\nwheel_speed = 5\n");
    fclose(f);
    setenv("OSD_CONFIG_DIR", dir, 1);
    ui_mouse_load();
    ui_pointer_event b3 = press(UI_BTN3, 0, 1), b2 = press(UI_BTN2, 0, 1), sh1 = press(UI_BTN1, UI_MOD_SHIFT, 1);
    CHECK(ui_mouse_is(UI_ACT_POPUP, &b3));
    CHECK(!ui_mouse_is(UI_ACT_POPUP, &b2));
    CHECK(ui_mouse_is(UI_ACT_EDIT_TITLE, &sh1));
    CHECK(ui_mouse.left_handed && ui_mouse.double_click_ms == 600 && ui_mouse.wheel_speed == 5);
    unlink(path);
    rmdir(dir);
}

/* A two-button chord (the owner's: edit title on button 1 + button 2, the
 * popup menu on button 2): button 2 alone is the popup menu, never edit
 * title; button 2 with button 1 held (or 1 with 2 held) is edit title,
 * not the popup menu. */
TEST(two_button_chords)
{
    ui_chord c;
    CHECK(ui_mouse_parse_chord("button1+button2", &c) && c.button == 2 && c.with == 1);
    CHECK(ui_mouse_set("edit_title", "button1+button2"));
    CHECK(ui_mouse_set("popup_menu", "button2"));
    ui_pointer_event b2 = press(UI_BTN2, 0, 1), b2_with1 = press(UI_BTN2, 0, 1), b1_with2 = press(UI_BTN1, 0, 1);
    b2_with1.held = 1u << UI_BTN1;
    b1_with2.held = 1u << UI_BTN2;
    CHECK(ui_mouse_is(UI_ACT_POPUP, &b2));
    CHECK(!ui_mouse_is(UI_ACT_EDIT_TITLE, &b2));
    CHECK(ui_mouse_is(UI_ACT_EDIT_TITLE, &b2_with1));
    CHECK(!ui_mouse_is(UI_ACT_POPUP, &b2_with1));
    CHECK(ui_mouse_is(UI_ACT_EDIT_TITLE, &b1_with2));
    CHECK(!ui_mouse_is(UI_ACT_SELECT, &b1_with2));
    ui_mouse_set("edit_title", "alt+button1");
}

int main(void)
{
    RUN(two_button_chords);
    RUN(defaults_are_os2s);
    RUN(chords_parse);
    RUN(a_config_file_remaps);
    return CHECK_SUMMARY();
}
