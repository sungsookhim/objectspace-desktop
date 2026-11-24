/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* The text entry's editing, without a display. */
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "check.h"
#include "ui.h"

static ui_entry_result key(ui_entry *e, unsigned sym, const char *utf8, unsigned mods)
{
    ui_key_event k = { .sym = sym, .mods = mods };
    if (utf8) snprintf(k.utf8, sizeof k.utf8, "%s", utf8);
    return ui_entry_key(e, &k);
}

static void type(ui_entry *e, const char *s)
{
    for (; *s; s++) { char c[2] = { *s, 0 }; key(e, (unsigned)*s, c, 0); }
}

TEST(typing_replaces_the_selected_stem)
{
    ui_entry *e = ui_entry_new("report.final.odt");
    ui_entry_select_stem(e);
    type(e, "notes");
    CHECK_STR(ui_entry_text(e), "notes.odt");
    ui_entry_free(e);
}

TEST(backspace_removes_whole_characters)
{
    ui_entry *e = ui_entry_new("caf\xc3\xa9");   /* café */
    CHECK(key(e, XKB_KEY_BackSpace, NULL, 0) == UI_ENTRY_CHANGED);
    CHECK_STR(ui_entry_text(e), "caf");
    ui_entry_set_text(e, "a\xe2\x82\xac" "b");   /* a€b */
    key(e, XKB_KEY_Left, NULL, 0);
    key(e, XKB_KEY_BackSpace, NULL, 0);
    CHECK_STR(ui_entry_text(e), "ab");
    ui_entry_free(e);
}

TEST(words_and_selection)
{
    ui_entry *e = ui_entry_new("one two three");
    key(e, XKB_KEY_Left, NULL, UI_MOD_CTRL);                 /* before "three" */
    key(e, XKB_KEY_Left, NULL, UI_MOD_CTRL | UI_MOD_SHIFT);  /* select "two " */
    type(e, "2 ");
    CHECK_STR(ui_entry_text(e), "one 2 three");
    key(e, XKB_KEY_BackSpace, NULL, UI_MOD_CTRL);            /* delete the word before the caret */
    CHECK_STR(ui_entry_text(e), "one three");
    key(e, XKB_KEY_a, "a", UI_MOD_CTRL);
    key(e, XKB_KEY_Delete, NULL, 0);
    CHECK_STR(ui_entry_text(e), "");
    ui_entry_free(e);
}

TEST(paste_drops_line_breaks)
{
    ui_entry *e = ui_entry_new("line one\nline two");   /* as if pasted text had a newline */
    ui_entry_select_all(e);
    key(e, XKB_KEY_c, "c", UI_MOD_CTRL);
    ui_entry_set_text(e, "");
    key(e, XKB_KEY_v, "v", UI_MOD_CTRL);
    CHECK_STR(ui_entry_text(e), "line oneline two");
    ui_entry_free(e);
}

TEST(enter_and_escape)
{
    ui_entry *e = ui_entry_new("x");
    CHECK(key(e, XKB_KEY_Return, NULL, 0) == UI_ENTRY_COMMIT);
    CHECK(key(e, XKB_KEY_Escape, NULL, 0) == UI_ENTRY_CANCEL);
    CHECK(key(e, XKB_KEY_Tab, NULL, 0) == UI_ENTRY_NONE);
    ui_entry_free(e);
}

int main(void)
{
    ui_scheme_init();
    ui_text_init();
    RUN(typing_replaces_the_selected_stem);
    RUN(backspace_removes_whole_characters);
    RUN(words_and_selection);
    RUN(paste_drops_line_breaks);
    RUN(enter_and_escape);
    return CHECK_SUMMARY();
}
