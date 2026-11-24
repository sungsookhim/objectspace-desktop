/* WPS Terminal's core (gui/terminal/term_core.c): the screen into cells,
 * keys into bytes, the scrollback, selections, paths quoted. */
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "check.h"
#include "term_core.h"

static char sent[256];
static size_t nsent;
static void out(const char *b, size_t n, void *u)
{
    (void)u;
    if (nsent + n < sizeof sent) { memcpy(sent + nsent, b, n); nsent += n; sent[nsent] = 0; }
}
static void clear_sent(void) { nsent = 0; sent[0] = 0; }

static const ui_color FG = { 0, 0, 0, 1 }, BG = { 1, 1, 1, 1 };

static void feed(term_core *t, const char *s) { term_core_input(t, s, strlen(s)); }

TEST(text_and_true_colours)
{
    term_core *t = term_core_new(5, 20, 100, out, NULL);
    feed(t, "ab\x1b[38;2;10;20;30;48;5;196mX\x1b[0m");
    term_cell c;
    term_core_cell(t, 0, 0, FG, BG, &c);
    CHECK_STR(c.text, "a");
    CHECK(c.bg_default && c.fg.r == 0);
    term_core_cell(t, 0, 2, FG, BG, &c);
    CHECK_STR(c.text, "X");
    CHECK((int)(c.fg.r * 255 + 0.5) == 10 && (int)(c.fg.g * 255 + 0.5) == 20 && (int)(c.fg.b * 255 + 0.5) == 30);
    CHECK(!c.bg_default && c.bg.r > 0.9 && c.bg.g < 0.1);   /* 196: red */
    int r, col;
    bool vis;
    term_core_cursor(t, &r, &col, &vis);
    CHECK(r == 0 && col == 3 && vis);
    feed(t, "\x1b[1;7mR");   /* bold, reversed */
    term_core_cell(t, 0, 3, FG, BG, &c);
    CHECK(c.bold && c.fg.r == 1 && c.bg.r == 0 && !c.bg_default);
    term_core_free(t);
}

TEST(wide_characters)
{
    term_core *t = term_core_new(5, 20, 100, out, NULL);
    feed(t, "\xea\xb0\x80" "b");   /* 가b */
    term_cell c;
    term_core_cell(t, 0, 0, FG, BG, &c);
    CHECK_STR(c.text, "\xea\xb0\x80");
    CHECK(c.width == 2);
    term_core_cell(t, 0, 1, FG, BG, &c);
    CHECK(c.width == 0);
    term_core_cell(t, 0, 2, FG, BG, &c);
    CHECK_STR(c.text, "b");
    char *s = term_core_text(t, 0, 0, 0, 19);
    CHECK_STR(s, "\xea\xb0\x80" "b");
    free(s);
    term_core_free(t);
}

TEST(keys_into_bytes)
{
    term_core *t = term_core_new(5, 20, 100, out, NULL);
    clear_sent(); term_core_key(t, XKB_KEY_Up, "", 0); CHECK_STR(sent, "\x1b[A");
    clear_sent(); term_core_key(t, 'c', "c", UI_MOD_CTRL); CHECK_STR(sent, "\x03");
    clear_sent(); term_core_key(t, XKB_KEY_Return, "\r", 0); CHECK_STR(sent, "\r");
    clear_sent(); term_core_key(t, XKB_KEY_BackSpace, "\b", 0); CHECK_STR(sent, "\x7f");
    clear_sent(); term_core_key(t, 'A', "A", UI_MOD_SHIFT); CHECK_STR(sent, "A");
    clear_sent(); term_core_key(t, 0, "\xed\x95\x9c", 0); CHECK_STR(sent, "\xed\x95\x9c");   /* 한, from an input method */
    clear_sent(); term_core_key(t, XKB_KEY_F1, "", 0); CHECK_STR(sent, "\x1bOP");
    clear_sent(); CHECK(!term_core_key(t, XKB_KEY_Shift_L, "", UI_MOD_SHIFT)); CHECK_STR(sent, "");
    feed(t, "\x1b[?1h");   /* application cursor keys (vim, less) */
    clear_sent(); term_core_key(t, XKB_KEY_Up, "", 0); CHECK_STR(sent, "\x1bOA");
    feed(t, "\x1b[?2004h");   /* bracketed paste */
    clear_sent(); term_core_paste(t, "ls"); CHECK_STR(sent, "\x1b[200~ls\x1b[201~");
    term_core_free(t);
}

TEST(scrollback_and_the_alternate_screen)
{
    term_core *t = term_core_new(3, 10, 100, out, NULL);
    feed(t, "one\r\ntwo\r\nthree\r\nfour\r\nfive");
    CHECK(term_core_scrollback(t) == 2);
    char *s = term_core_text(t, -2, 0, -1, 9);
    CHECK_STR(s, "one\ntwo");
    free(s);
    feed(t, "\x1b[?1049h" "full");   /* a full-screen program */
    CHECK(term_core_altscreen(t));
    feed(t, "\x1b[?1049l");
    CHECK(!term_core_altscreen(t));
    s = term_core_text(t, 0, 0, 2, 9);
    CHECK_STR(s, "three\nfour\nfive");
    free(s);
    feed(t, "\x1b]0;my title\x07\x07");
    CHECK_STR(term_core_title(t), "my title");
    CHECK(term_core_bell(t));
    CHECK(!term_core_bell(t));
    term_core_free(t);
}

TEST(paths_quoted)
{
    char q[256];
    term_shell_quote("/home/me/notes.txt", q, sizeof q); CHECK_STR(q, "/home/me/notes.txt");
    term_shell_quote("/home/me/My Files", q, sizeof q); CHECK_STR(q, "'/home/me/My Files'");
    term_shell_quote("/tmp/it's", q, sizeof q); CHECK_STR(q, "'/tmp/it'\\''s'");
    term_shell_quote("/tmp/$HOME", q, sizeof q); CHECK_STR(q, "'/tmp/$HOME'");
}

int main(void)
{
    RUN(text_and_true_colours);
    RUN(wide_characters);
    RUN(keys_into_bytes);
    RUN(scrollback_and_the_alternate_screen);
    RUN(paths_quoted);
    return CHECK_SUMMARY();
}
