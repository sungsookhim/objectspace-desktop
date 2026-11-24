/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * Objectspace Terminal's core (docs/cli-design.md 1): libvterm's screen with a
 * scrollback, keys turned into the bytes a program reads, cells' colours,
 * and the text of a selection - no window, no pseudo-terminal (those are
 * osd-terminal.c's), so the unit tests drive it directly.
 *
 * Rows are numbered from the top of the screen: 0 .. rows-1 the screen,
 * -1 the newest line of the scrollback, -nsb the oldest.
 */
#ifndef OSD_TERM_CORE_H
#define OSD_TERM_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vterm.h>

#include "ui.h"

typedef struct term_core term_core;

/* What the program is sent (keys, answers to its queries). */
typedef void (*term_out_fn)(const char *bytes, size_t n, void *user);

term_core *term_core_new(int rows, int cols, size_t scrollback, term_out_fn out, void *user);
void term_core_free(term_core *t);

/* The program's output. Returns whether the screen changed. */
bool term_core_input(term_core *t, const char *bytes, size_t n);
void term_core_resize(term_core *t, int rows, int cols);
int term_core_rows(const term_core *t);
int term_core_cols(const term_core *t);
size_t term_core_scrollback(const term_core *t);   /* lines kept above the screen */

/* A key: an xkb keysym, its text, ui's modifiers. False if it sends
 * nothing (a modifier alone). */
bool term_core_key(term_core *t, uint32_t sym, const char *utf8, unsigned mods);
/* Text pasted or dropped (bracketed if the program asked for it). */
void term_core_paste(term_core *t, const char *text);

/* One cell: its characters as UTF-8 ("" empty), its width (1, 2; 0 the
 * second half of a wide one), its colours (default ones as given),
 * reversed already, and its look. */
typedef struct {
    char text[32];
    int width;
    ui_color fg, bg;
    bool bg_default;
    bool bold, italic, underline, strike;
} term_cell;
void term_core_cell(term_core *t, int row, int col, ui_color fg_default, ui_color bg_default, term_cell *out);

/* The cursor (screen row and column), and whether it is shown. */
void term_core_cursor(const term_core *t, int *row, int *col, bool *visible);
const char *term_core_title(const term_core *t);   /* set by the program ("" none) */
bool term_core_mouse(const term_core *t);           /* the program wants the mouse */
bool term_core_altscreen(const term_core *t);       /* full-screen programs' screen */
bool term_core_bell(term_core *t);                  /* rung since the last call */
void term_core_mouse_event(term_core *t, int row, int col, int button, bool pressed, bool move, unsigned mods);

/* The text from (r0, c0) to (r1, c1) inclusive, rows joined with "\n"
 * (each row's trailing blanks dropped). Caller frees. */
char *term_core_text(term_core *t, int r0, int c0, int r1, int c1);

/* A path as the shell takes it: in single quotes when it has to be. */
void term_shell_quote(const char *path, char *out, size_t n);

#endif
