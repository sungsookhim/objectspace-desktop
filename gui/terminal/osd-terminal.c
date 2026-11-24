/*
 * Objectspace Terminal (docs/cli-design.md 1): a program on a pseudo-terminal in a
 * Objectspace window. libvterm keeps the screen (term_core.c); this draws it with
 * Objectspace's text code, and passes keys, the mouse, pastes and drops on.
 *
 *   osd-terminal [--title TITLE] [--hold] [--font SPEC] [--cwd FOLDER]
 *                [--size COLSxROWS] [-- COMMAND [ARG...]]
 *
 * No command: the user's shell. --hold: the window stays when the program
 * ends ("Ended (status N). Press Enter to close"), else it closes with it.
 * One process per window: the one Objectspace started stands for the program
 * (in use, a work area's group).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "term_core.h"
#include "term_setup.h"
#include "ui.h"

#define PAD 3
#define SB_W 14   /* the scroll bar */

static struct term {
    ui_app *app;
    ui_surface *s;
    term_core *core;
    int fd;
    pid_t pid;
    ui_font font, bold;
    double cw, ch, asc;
    char title[256];      /* --title, else the command's name */
    bool hold, ended, focused;
    int status;
    int view;             /* lines scrolled back (0: the bottom) */
    bool selecting, has_sel;
    int sel_r0, sel_c0, sel_r1, sel_c1;   /* core rows (negative: scrollback) */
    bool dragging_sb;
    ui_entry *ime;        /* (an input method's target: what it composes is drawn at the cursor) */
} T = { .fd = -1 };

static ui_color fg_default(void) { return ui_scheme_now.text; }
static ui_color bg_default(void) { return ui_scheme_now.client_bg; }

static ui_rect text_area(void)
{
    ui_rect c = ui_frame_client(T.s);
    return (ui_rect){ c.x + PAD, c.y + PAD, c.w - SB_W - 2 * PAD, c.h - 2 * PAD };
}

static ui_rect sb_rect(void)
{
    ui_rect c = ui_frame_client(T.s);
    return (ui_rect){ c.x + c.w - SB_W, c.y, SB_W, c.h };
}

/* ------------------------------------------------------------ the program */

static void send_bytes(const char *b, size_t n, void *user)
{
    (void)user;
    while (n && T.fd >= 0) {
        ssize_t w = write(T.fd, b, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { struct pollfd p = { T.fd, POLLOUT, 0 }; poll(&p, 1, 100); continue; }
            return;
        }
        b += w;
        n -= (size_t)w;
    }
}

static void update_title(void)
{
    const char *p = term_core_title(T.core);
    char t[400];
    if (T.ended) snprintf(t, sizeof t, "%s (ended)", *p ? p : T.title);
    else snprintf(t, sizeof t, "%s", *p ? p : T.title);
    if (strcmp(t, ui_window_title(T.s) ? ui_window_title(T.s) : "")) ui_window_set_title(T.s, t);
}

static void ended(void)
{
    if (T.fd >= 0) { ui_app_unwatch_fd(T.app, T.fd); close(T.fd); T.fd = -1; }
    int st = 0;
    if (T.pid > 0 && waitpid(T.pid, &st, 0) == T.pid) T.status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    T.pid = -1;
    if (!T.hold) { ui_app_quit(T.app); return; }
    T.ended = true;
    char msg[96];
    snprintf(msg, sizeof msg, "\r\n\x1b[0;7m Ended (status %d). Press Enter to close. \x1b[0m", T.status);
    term_core_input(T.core, msg, strlen(msg));
    update_title();
    ui_surface_damage_all(T.s);
}

static void on_pty(void *arg, int fd, short rev)
{
    (void)arg;
    char buf[65536];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
        if (term_core_input(T.core, buf, (size_t)n)) {
            if (T.view) T.view = (int)fmin(T.view, (double)term_core_scrollback(T.core));
            ui_surface_damage_all(T.s);
        }
        if (term_core_bell(T.core)) ui_action("terminal.bell", T.s, (ui_rect){ 0, 0, 0, 0 });
        update_title();
        return;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n == 0 || (rev & (POLLHUP | POLLERR)) || n < 0) ended();   /* (EIO: the program and its children are gone) */
}

static bool start(char **argv, const char *cwd, int rows, int cols)
{
    struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols };
    int fd;
    pid_t pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid < 0) { perror("osd-terminal: forkpty"); return false; }
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "osd-terminal", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        if (cwd && chdir(cwd) != 0) fprintf(stderr, "osd-terminal: %s: %s\n", cwd, strerror(errno));
        signal(SIGPIPE, SIG_DFL);
        execvp(argv[0], argv);
        fprintf(stderr, "osd-terminal: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    T.fd = fd;
    T.pid = pid;
    ui_app_watch_fd(T.app, fd, POLLIN, on_pty, NULL);
    return true;
}

/* The window's size in cells, after a resize. */
static void fit(void)
{
    ui_rect a = text_area();
    int cols = (int)fmax(2, floor(a.w / T.cw)), rows = (int)fmax(1, floor(a.h / T.ch));
    if (cols == term_core_cols(T.core) && rows == term_core_rows(T.core)) return;
    term_core_resize(T.core, rows, cols);
    if (T.fd >= 0) {
        struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols };
        ioctl(T.fd, TIOCSWINSZ, &ws);   /* (the program gets SIGWINCH) */
    }
}

/* -------------------------------------------------------------- drawing */

static bool selected(int row, int col)
{
    if (!T.has_sel) return false;
    int r0 = T.sel_r0, c0 = T.sel_c0, r1 = T.sel_r1, c1 = T.sel_c1;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) { int r = r0, c = c0; r0 = r1; c0 = c1; r1 = r; c1 = c; }
    if (row < r0 || row > r1) return false;
    if (row == r0 && col < c0) return false;
    if (row == r1 && col > c1) return false;
    return true;
}

/* Block elements, box drawing and braille, drawn here to fill the cell
 * edge to edge on whole pixels (a font's glyphs leave gaps between cells:
 * btop's meters and graphs). False: the font draws it. */
static uint32_t cp_of(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] >= 0xe0 && u[0] < 0xf0 && u[1] && u[2] && !u[3])
        return (uint32_t)(u[0] & 0x0f) << 12 | (uint32_t)(u[1] & 0x3f) << 6 | (u[2] & 0x3f);
    return 0;
}

/* Box drawing: each arm (up, right, down, left): 0 none, 1 light, 2 heavy, 3 double. */
static const struct { uint16_t cp; unsigned char u, r, d, l; } boxes[] = {
    { 0x2500, 0, 1, 0, 1 }, { 0x2501, 0, 2, 0, 2 }, { 0x2502, 1, 0, 1, 0 }, { 0x2503, 2, 0, 2, 0 },
    { 0x2504, 0, 1, 0, 1 }, { 0x2505, 0, 2, 0, 2 }, { 0x2506, 1, 0, 1, 0 }, { 0x2507, 2, 0, 2, 0 },
    { 0x2508, 0, 1, 0, 1 }, { 0x2509, 0, 2, 0, 2 }, { 0x250a, 1, 0, 1, 0 }, { 0x250b, 2, 0, 2, 0 },
    { 0x250c, 0, 1, 1, 0 }, { 0x250f, 0, 2, 2, 0 }, { 0x2510, 0, 0, 1, 1 }, { 0x2513, 0, 0, 2, 2 },
    { 0x2514, 1, 1, 0, 0 }, { 0x2517, 2, 2, 0, 0 }, { 0x2518, 1, 0, 0, 1 }, { 0x251b, 2, 0, 0, 2 },
    { 0x251c, 1, 1, 1, 0 }, { 0x2523, 2, 2, 2, 0 }, { 0x2524, 1, 0, 1, 1 }, { 0x252b, 2, 0, 2, 2 },
    { 0x252c, 0, 1, 1, 1 }, { 0x2533, 0, 2, 2, 2 }, { 0x2534, 1, 1, 0, 1 }, { 0x253b, 2, 2, 0, 2 },
    { 0x253c, 1, 1, 1, 1 }, { 0x254b, 2, 2, 2, 2 }, { 0x254c, 0, 1, 0, 1 }, { 0x254d, 0, 2, 0, 2 },
    { 0x254e, 1, 0, 1, 0 }, { 0x254f, 2, 0, 2, 0 },
    { 0x2550, 0, 3, 0, 3 }, { 0x2551, 3, 0, 3, 0 }, { 0x2554, 0, 3, 3, 0 }, { 0x2557, 0, 0, 3, 3 },
    { 0x255a, 3, 3, 0, 0 }, { 0x255d, 3, 0, 0, 3 }, { 0x2560, 3, 3, 3, 0 }, { 0x2563, 3, 0, 3, 3 },
    { 0x2566, 0, 3, 3, 3 }, { 0x2569, 3, 3, 0, 3 }, { 0x256c, 3, 3, 3, 3 },
    { 0x256d, 0, 1, 1, 0 }, { 0x256e, 0, 0, 1, 1 }, { 0x256f, 1, 0, 0, 1 }, { 0x2570, 1, 1, 0, 0 },   /* (rounded: square) */
    { 0x2574, 0, 0, 0, 1 }, { 0x2575, 1, 0, 0, 0 }, { 0x2576, 0, 1, 0, 0 }, { 0x2577, 0, 0, 1, 0 },
    { 0x2578, 0, 0, 0, 2 }, { 0x2579, 2, 0, 0, 0 }, { 0x257a, 0, 2, 0, 0 }, { 0x257b, 0, 0, 2, 0 },
    { 0x257c, 0, 2, 0, 1 }, { 0x257d, 1, 0, 2, 0 }, { 0x257e, 0, 1, 0, 2 }, { 0x257f, 2, 0, 1, 0 },
};

static bool draw_special(cairo_t *cr, uint32_t cp, double x0, double y0, double x1, double y1, ui_color fg)
{
    double w = x1 - x0, h = y1 - y0;
    if (cp >= 0x2580 && cp <= 0x259f) {   /* block elements */
        double ex = round(w / 8), ey = round(h / 8);
        ui_rect r[4];
        int n = 0;
        double mx = round(x0 + w / 2), my = round(y0 + h / 2);
        if (cp == 0x2580) r[n++] = (ui_rect){ x0, y0, w, my - y0 };
        else if (cp >= 0x2581 && cp <= 0x2588) { double t = round(h * (double)(cp - 0x2580) / 8); r[n++] = (ui_rect){ x0, y1 - t, w, t }; }
        else if (cp >= 0x2589 && cp <= 0x258f) { double t = round(w * (double)(0x2590 - cp) / 8); r[n++] = (ui_rect){ x0, y0, t, h }; }
        else if (cp == 0x2590) r[n++] = (ui_rect){ mx, y0, x1 - mx, h };
        else if (cp >= 0x2591 && cp <= 0x2593) {   /* shades: the colour at 25, 50, 75% */
            ui_color c = fg;
            c.a = (double)(cp - 0x2590) / 4;
            ui_fill(cr, (ui_rect){ x0, y0, w, h }, c);
            return true;
        }
        else if (cp == 0x2594) r[n++] = (ui_rect){ x0, y0, w, ey ? ey : 1 };
        else if (cp == 0x2595) r[n++] = (ui_rect){ x1 - (ex ? ex : 1), y0, ex ? ex : 1, h };
        else {   /* quadrants: bits upper left 1, upper right 2, lower left 4, lower right 8 */
            static const unsigned char q[] = { 4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8 };
            unsigned m = q[cp - 0x2596];
            if (m & 1) r[n++] = (ui_rect){ x0, y0, mx - x0, my - y0 };
            if (m & 2) r[n++] = (ui_rect){ mx, y0, x1 - mx, my - y0 };
            if (m & 4) r[n++] = (ui_rect){ x0, my, mx - x0, y1 - my };
            if (m & 8) r[n++] = (ui_rect){ mx, my, x1 - mx, y1 - my };
        }
        for (int i = 0; i < n; i++) ui_fill(cr, r[i], fg);
        return true;
    }
    if (cp >= 0x2800 && cp <= 0x28ff) {   /* braille: a 2 x 4 grid of dots */
        static const unsigned char bit[2][4] = { { 0, 1, 2, 6 }, { 3, 4, 5, 7 } };
        unsigned m = cp - 0x2800;
        double dw = fmax(1, round(w / 4)), dh = fmax(1, round(h / 8));
        for (int c = 0; c < 2; c++)
            for (int r = 0; r < 4; r++)
                if (m & (1u << bit[c][r])) {
                    double cx = round(x0 + w * (c * 2 + 1) / 4 - dw / 2), cy = round(y0 + h * (r * 2 + 1) / 8 - dh / 2);
                    ui_fill(cr, (ui_rect){ cx, cy, dw, dh }, fg);
                }
        return true;
    }
    if (cp < 0x2500 || cp > 0x257f) return false;
    for (size_t i = 0; i < sizeof boxes / sizeof *boxes; i++) {
        if (boxes[i].cp != cp) continue;
        double lt = fmax(1, round(w / 8)), hv = lt * 2;
        double mx = round(x0 + w / 2), my = round(y0 + h / 2);
        unsigned arm[4] = { boxes[i].u, boxes[i].r, boxes[i].d, boxes[i].l };
        for (int k = 0; k < 4; k++) {
            if (!arm[k]) continue;
            double t = arm[k] == 2 ? hv : lt;
            /* (a double arm: two lines a line apart) */
            int lines = arm[k] == 3 ? 2 : 1;
            for (int L = 0; L < lines; L++) {
                double off = lines == 2 ? (L ? lt : -lt) * 1.5 : 0;
                double cxl = round(mx - t / 2 + off), cyl = round(my - t / 2 + off);
                if (k == 0) ui_fill(cr, (ui_rect){ cxl, y0, t, my + t / 2 - y0 }, fg);
                else if (k == 2) ui_fill(cr, (ui_rect){ cxl, round(my - t / 2), t, y1 - round(my - t / 2) }, fg);
                else if (k == 1) ui_fill(cr, (ui_rect){ round(mx - t / 2), cyl, x1 - round(mx - t / 2), t }, fg);
                else ui_fill(cr, (ui_rect){ x0, cyl, mx + t / 2 - x0, t }, fg);
            }
        }
        return true;
    }
    return false;
}

static void draw_row(cairo_t *cr, ui_rect a, int vr)
{
    int row = vr - T.view, cols = term_core_cols(T.core);
    double y = a.y + vr * T.ch;
    const ui_scheme *k = &ui_scheme_now;
    for (int col = 0; col < cols; col++) {
        term_cell c;
        term_core_cell(T.core, row, col, fg_default(), bg_default(), &c);
        if (!c.width) continue;
        double x = a.x + col * T.cw, w = T.cw * c.width;
        ui_color fg = c.fg, bg = c.bg;
        bool bgd = c.bg_default;
        if (selected(row, col)) { fg = k->selection_text; bg = k->selection; bgd = false; }
        /* whole pixels: neighbouring cells' backgrounds and blocks meet */
        double px0 = round(x), px1 = round(a.x + (col + c.width) * T.cw);
        if (!bgd) ui_fill(cr, (ui_rect){ px0, y, px1 - px0, T.ch }, bg);
        uint32_t cp = cp_of(c.text);
        if (cp && draw_special(cr, cp, px0, y, px1, y + T.ch, fg)) {
            /* (drawn: a block, a box line, braille) */
        } else if (c.text[0] && strcmp(c.text, " ")) {
            ui_set_color(cr, fg);
            ui_font f = c.bold ? T.bold : T.font;
            if (c.italic) {   /* (slanted: a monospace font's italic may not exist) */
                cairo_save(cr);
                cairo_matrix_t m = { 1, 0, -0.2, 1, 0.2 * (y + T.asc), 0 };
                cairo_transform(cr, &m);
                ui_text_draw(cr, f, x, y + T.asc, c.text, 0);
                cairo_restore(cr);
            } else {
                ui_text_draw(cr, f, x, y + T.asc, c.text, 0);
            }
        }
        if (c.underline) ui_fill(cr, (ui_rect){ x, y + T.asc + 2, w, 1 }, fg);
        if (c.strike) ui_fill(cr, (ui_rect){ x, y + T.asc * 0.6, w, 1 }, fg);
    }
}

static void draw_scrollbar(cairo_t *cr)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_rect r = sb_rect();
    ui_fill(cr, r, k->placeholder);   /* the track, lighter than the thumb */
    ui_bevel(cr, r, false);
    size_t n = term_core_scrollback(T.core);
    int rows = term_core_rows(T.core);
    double total = (double)n + rows, th = fmax(16, r.h * rows / total);
    double ty = r.y + (r.h - th) * (total > rows ? (double)(n - (size_t)T.view) / (double)n : 1);
    if (!n) ty = r.y;
    ui_rect thumb = { r.x + 1, ty, r.w - 2, n ? th : r.h };
    ui_fill(cr, thumb, k->face);
    ui_bevel(cr, thumb, true);
}

static void t_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    ui_frame_draw(s, cr, clip, ui_window_title(s));
    ui_rect c = ui_frame_client(s), a = text_area();
    ui_fill(cr, (ui_rect){ c.x, c.y, c.w - SB_W, c.h }, bg_default());
    cairo_save(cr);
    cairo_rectangle(cr, c.x, c.y, c.w - SB_W, c.h);
    cairo_clip(cr);
    int rows = term_core_rows(T.core);
    for (int vr = 0; vr < rows; vr++) draw_row(cr, a, vr);
    /* the cursor: a block (an outline when the window is not active) */
    int cr_, cc;
    bool vis;
    term_core_cursor(T.core, &cr_, &cc, &vis);
    int vr = cr_ + T.view;
    if (vis && !T.ended && vr >= 0 && vr < rows) {
        ui_rect b = { a.x + cc * T.cw, a.y + vr * T.ch, T.cw, T.ch };
        const char *pre = T.ime ? ui_entry_preedit(T.ime) : NULL;
        if (pre && *pre) {   /* being composed (an input method): shown at the cursor, underlined */
            double w = ui_text_width(T.font, pre);
            ui_fill(cr, (ui_rect){ b.x, b.y, w, T.ch }, bg_default());
            ui_set_color(cr, fg_default());
            ui_text_draw(cr, T.font, b.x, b.y + T.asc, pre, 0);
            ui_fill(cr, (ui_rect){ b.x, b.y + T.ch - 2, w, 1 }, fg_default());
        } else if (T.focused) {
            term_cell cell;
            term_core_cell(T.core, cr_, cc, fg_default(), bg_default(), &cell);
            if (cell.width == 2) b.w *= 2;
            ui_fill(cr, b, ui_scheme_now.cursor);
            ui_set_color(cr, bg_default());
            if (cell.text[0]) ui_text_draw(cr, cell.bold ? T.bold : T.font, b.x, b.y + T.asc, cell.text, 0);
        } else {
            ui_set_color(cr, ui_scheme_now.cursor);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
    draw_scrollbar(cr);
}

/* ---------------------------------------------------------------- input */

static void scroll_view(int lines)
{
    int n = (int)term_core_scrollback(T.core), v = T.view + lines;
    v = v < 0 ? 0 : v > n ? n : v;
    if (v != T.view) { T.view = v; ui_surface_damage_all(T.s); }
}

static void copy_selection(void)
{
    if (!T.has_sel) return;
    char *s = term_core_text(T.core, T.sel_r0, T.sel_c0, T.sel_r1, T.sel_c1);
    if (s && *s) ui_clipboard_set(s);
    free(s);
}

static void paste(const char *text)
{
    if (!text || !*text || T.ended) return;
    T.view = 0;
    term_core_paste(T.core, text);
}

static void cell_at(double x, double y, int *row, int *col)
{
    ui_rect a = text_area();
    int vr = (int)floor((y - a.y) / T.ch), rows = term_core_rows(T.core), cols = term_core_cols(T.core);
    vr = vr < 0 ? 0 : vr >= rows ? rows - 1 : vr;
    int c = (int)floor((x - a.x) / T.cw);
    *col = c < 0 ? 0 : c >= cols ? cols - 1 : c;
    *row = vr - T.view;
}

static void menu_chosen(void *arg, int64_t id)
{
    (void)arg;
    if (id == 1) copy_selection();
    else if (id == 2) paste(ui_clipboard_text());
}

static void t_key(ui_surface *s, const ui_key_event *e)
{
    (void)s;
    bool cs = (e->mods & (UI_MOD_CTRL | UI_MOD_SHIFT)) == (UI_MOD_CTRL | UI_MOD_SHIFT);
    if (cs && (e->sym == XKB_KEY_C || e->sym == XKB_KEY_c)) { copy_selection(); return; }
    if ((cs && (e->sym == XKB_KEY_V || e->sym == XKB_KEY_v)) || (e->mods == UI_MOD_SHIFT && e->sym == XKB_KEY_Insert)) {
        paste(ui_clipboard_text());
        return;
    }
    if (e->mods == UI_MOD_SHIFT && !term_core_altscreen(T.core)) {   /* the scrollback */
        int page = term_core_rows(T.core) - 1;
        if (e->sym == XKB_KEY_Page_Up) { scroll_view(page); return; }
        if (e->sym == XKB_KEY_Page_Down) { scroll_view(-page); return; }
        if (e->sym == XKB_KEY_Home) { scroll_view(1 << 30); return; }
        if (e->sym == XKB_KEY_End) { scroll_view(-(1 << 30)); return; }
    }
    if (T.ended) {
        if (e->sym == XKB_KEY_Return || e->sym == XKB_KEY_KP_Enter || e->sym == XKB_KEY_Escape) ui_app_quit(T.app);
        return;
    }
    if (term_core_key(T.core, e->sym, e->utf8, e->mods)) {
        if (T.view) { T.view = 0; ui_surface_damage_all(T.s); }
        if (T.has_sel) { T.has_sel = false; ui_surface_damage_all(T.s); }
    }
}

static void t_pointer(ui_surface *s, const ui_pointer_event *e)
{
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    if (e->kind != UI_PRESS || h != UI_HIT_CLIENT) ui_frame_pointer(s, e);   /* (its frame: buttons, moving, sizing) */
    if (h != UI_HIT_CLIENT && !T.selecting && !T.dragging_sb) return;
    ui_rect sb = sb_rect();
    int row, col;
    cell_at(e->x, e->y, &row, &col);
    /* the scroll bar: a press or a drag puts the view there */
    if ((e->kind == UI_PRESS && ui_rect_has(sb, e->x, e->y)) || (T.dragging_sb && e->kind == UI_MOTION)) {
        T.dragging_sb = true;
        int n = (int)term_core_scrollback(T.core);
        double f = (e->y - sb.y) / fmax(1, sb.h);
        int v = (int)lround((1 - f) * n);
        scroll_view(v - T.view);
        return;
    }
    if (e->kind == UI_RELEASE && T.dragging_sb) { T.dragging_sb = false; return; }
    /* the program's mouse (htop, vim with mouse on), unless Shift is held */
    if (term_core_mouse(T.core) && !(e->mods & UI_MOD_SHIFT) && !T.ended && row >= 0) {
        int b = e->button == UI_BTN1 ? 1 : e->button == UI_BTN3 ? 2 : 3;
        if (e->kind == UI_PRESS || e->kind == UI_RELEASE) term_core_mouse_event(T.core, row, col, b, e->kind == UI_PRESS, false, e->mods);
        else if (e->kind == UI_MOTION) term_core_mouse_event(T.core, row, col, 0, false, true, e->mods);
        else if (e->kind == UI_WHEEL) term_core_mouse_event(T.core, row, col, e->wheel > 0 ? 5 : 4, true, false, e->mods);
        return;
    }
    if (e->kind == UI_WHEEL) {
        int lines = (int)lround(e->wheel / T.ch);
        if (!lines) lines = e->wheel > 0 ? 1 : -1;
        if (term_core_altscreen(T.core)) {   /* (a full-screen program without the mouse: arrows, as other terminals do) */
            for (int i = 0; i < abs(lines) && !T.ended; i++) term_core_key(T.core, lines > 0 ? XKB_KEY_Down : XKB_KEY_Up, "", 0);
        } else {
            scroll_view(-lines * 3);
        }
        return;
    }
    if (e->kind == UI_PRESS && e->button == UI_BTN1) {
        T.selecting = true;
        T.has_sel = false;
        T.sel_r0 = T.sel_r1 = row;
        T.sel_c0 = T.sel_c1 = col;
        ui_surface_damage_all(s);
    } else if (e->kind == UI_MOTION && T.selecting) {
        T.sel_r1 = row;
        T.sel_c1 = col;
        T.has_sel = T.sel_r1 != T.sel_r0 || T.sel_c1 != T.sel_c0;
        ui_surface_damage_all(s);
    } else if (e->kind == UI_RELEASE && e->button == UI_BTN1) {
        T.selecting = false;
    } else if (e->kind == UI_RELEASE && e->button == ui_mouse_button(UI_ACT_POPUP)) {
        ui_menu_item items[] = { { .id = 1, .label = (char *)"Copy\tCtrl+Shift+C", .enabled = T.has_sel },
                                 { .id = 2, .label = (char *)"Paste\tCtrl+Shift+V", .enabled = !T.ended } };
        ui_menu_popup(s, items, 2, e->x, e->y, e->serial, menu_chosen, NULL);
    }
}

static void t_resized(ui_surface *s, double w, double h)
{
    (void)w; (void)h;
    fit();
    ui_surface_damage_all(s);
}

static void t_focus(ui_surface *s, bool on)
{
    T.focused = on;
    if (on && T.ime) {
        int r, c;
        bool v;
        term_core_cursor(T.core, &r, &c, &v);
        ui_rect a = text_area();
        ui_text_input_focus(s, T.ime, (ui_rect){ a.x + c * T.cw, a.y + r * T.ch, T.cw, T.ch });
    }
    ui_surface_damage_all(s);
}

static void t_close(ui_surface *s)
{
    (void)s;
    if (T.pid > 0) kill(-T.pid, SIGHUP);   /* the program's group, as other terminals do */
    ui_app_quit(T.app);
}

/* Objects dropped: their paths, quoted, typed in. */
static uint32_t t_drag_motion(ui_surface *s, double x, double y, const char *const *mimes, size_t n, uint32_t src_actions,
                              unsigned mods, const char **mime)
{
    (void)s; (void)x; (void)y; (void)src_actions; (void)mods;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(mimes[i], "text/uri-list") && !T.ended) { *mime = mimes[i]; return UI_DND_COPY; }
    for (size_t i = 0; i < n; i++)
        if ((!strcmp(mimes[i], "text/plain;charset=utf-8") || !strcmp(mimes[i], "text/plain")) && !T.ended) { *mime = mimes[i]; return UI_DND_COPY; }
    return 0;
}

static uint32_t t_drop(ui_surface *s, double x, double y, const char *mime, const char *data, size_t len, uint32_t action,
                       uint32_t start_action, unsigned mods)
{
    (void)s; (void)x; (void)y; (void)action; (void)start_action; (void)mods;
    if (strcmp(mime, "text/uri-list")) {
        char *t = strndup(data, len);
        paste(t);
        free(t);
        return UI_DND_COPY;
    }
    size_t cap = len * 4 + 16, o = 0;
    char *out = malloc(cap);
    if (!out) return 0;
    out[0] = 0;
    for (const char *p = data, *end = data + len; p < end;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (l && p[l - 1] == '\r') l--;
        if (l > 7 && !strncmp(p, "file://", 7)) {
            const char *q = p + 7, *qe = p + l;
            while (q < qe && *q != '/') q++;   /* (a host name) */
            char path[4096];
            size_t k = 0;
            for (; q < qe && k + 1 < sizeof path; q++) {
                if (*q == '%' && qe - q > 2) { char hx[3] = { q[1], q[2], 0 }; path[k++] = (char)strtol(hx, NULL, 16); q += 2; }
                else path[k++] = *q;
            }
            path[k] = 0;
            char quoted[8300];
            term_shell_quote(path, quoted, sizeof quoted);
            o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? " " : "", quoted);
        }
        p = nl ? nl + 1 : end;
    }
    if (o) { strcat(out, " "); paste(out); }
    free(out);
    return o ? UI_DND_COPY : 0;
}

static const ui_surface_ops t_ops = {
    .draw = t_draw, .pointer = t_pointer, .key = t_key, .resized = t_resized, .focus = t_focus, .close = t_close,
    .drag_motion = t_drag_motion, .drop = t_drop,
};

/* ----------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr, "usage: osd-terminal [--title TITLE] [--hold] [--font SPEC] [--cwd FOLDER] [--size COLSxROWS] [-- COMMAND [ARG...]]\n");
}

int main(int argc, char **argv)
{
    const char *title = NULL, *font = getenv("OSD_TERMINAL_FONT"), *cwd = NULL;
    int cols = 80, rows = 24;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "--version")) { printf("osd-terminal %s\n", OSD_VERSION); return 0; }
        if (!strcmp(argv[i], "--hold")) T.hold = true;
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font = argv[++i];
        else if (!strcmp(argv[i], "--cwd") && i + 1 < argc) cwd = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &cols, &rows) != 2) { usage(); return 2; } }
        else if (argv[i][0] == '-') { usage(); return 2; }
        else break;   /* the command, without "--" */
    }
    /* the command, or the user's shell */
    char *shell_argv[2] = { NULL, NULL };
    char **cmd = argv + i;
    if (i >= argc) {
        const char *sh = getenv("SHELL");
        struct passwd *pw = getpwuid(getuid());
        if ((!sh || !*sh) && pw) sh = pw->pw_shell;
        shell_argv[0] = (char *)(sh && *sh ? sh : "/bin/sh");
        cmd = shell_argv;
    }
    const char *base = strrchr(cmd[0], '/');
    snprintf(T.title, sizeof T.title, "%s", title ? title : base ? base + 1 : cmd[0]);
    T.app = ui_app_new();
    if (!T.app) return 1;
    /* the scheme in use and the System's Terminal font (--font first) */
    char sys_font[200] = "";
    term_setup_read(sys_font, sizeof sys_font);
    if (!font && sys_font[0]) font = sys_font;
    T.font = ui_font_named(font && ui_font_valid(font) ? font : "11.Monospace");
    char bspec[200];
    snprintf(bspec, sizeof bspec, "%s Bold", font && ui_font_valid(font) ? font : "11.Monospace");
    T.bold = ui_font_named(bspec);
    T.cw = ui_text_width(T.font, "M");
    T.ch = ceil(ui_font_line_h(T.font));
    T.asc = ui_font_ascent(T.font);
    if (cols < 10) cols = 10;
    if (rows < 3) rows = 3;
    T.core = term_core_new(rows, cols, 5000, send_bytes, NULL);
    double w = cols * T.cw + 2 * PAD + SB_W + 2 * ui_scheme_now.border, h = rows * T.ch + 2 * PAD + 2 * ui_scheme_now.border + ui_scheme_now.title_h;
    T.s = ui_window_new(T.app, &t_ops, NULL, "io.github.sungsookhim.Objectspace.Terminal", ceil(w), ceil(h));
    ui_window_set_title(T.s, T.title);
    T.ime = ui_entry_new("");
    if (!start(cmd, cwd, rows, cols)) return 1;
    ui_app_run(T.app);
    if (T.pid > 0) kill(-T.pid, SIGHUP);
    return T.ended ? 0 : T.status;
}
