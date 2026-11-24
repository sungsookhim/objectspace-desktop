/*
 * G0 prototype: a folder window drawn with our own code (Cairo, HarfBuzz,
 * FreeType) on a plain Wayland surface, fed by osdd's streaming listing.
 *
 * Design (gui-design.md, Architecture): the input and drawing thread never
 * waits on osdd or the disk. A service thread fetches pages and stores
 * them in the model under a short lock, then wakes the drawing thread with
 * an eventfd. Rows not loaded yet are drawn as placeholders.
 *
 * usage: folderview FOLDER [--bench SECONDS]
 *   --bench: scroll by itself (a fling, then jumps as if dragging the
 *   scroll bar) and print frame statistics as JSON, then exit.
 */
#define _GNU_SOURCE
#include <cairo-ft.h>
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "cbor.h"
#include "xdg-shell-client-protocol.h"

/* ------------------------------------------------------------- metrics */

#define BORDER 4
#define TITLE_H 22
#define HEADER_H 20
#define ROW_H 20
#define STATUS_H 20
#define SCROLL_W 16
#define FONT_PX 13.0

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* --------------------------------------------------------------- model */

#define PAGE 256
enum { P_NONE, P_NAMES, P_WANT_DETAILS, P_DETAILED };

struct row {
    char *name, *title;
    char type;              /* d f l o */
    int64_t size, mtime;
};

struct page {
    int state;
    bool requested;
    struct row rows[PAGE];
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t want_cv = PTHREAD_COND_INITIALIZER;
static struct page **pages;
static size_t npages;
static size_t total;
static bool complete, quitting;
static size_t vis_first_page, vis_last_page;   /* what the window shows */
static char service_error[256];
static int wake_fd;
static double t_start, t_first_rows = -1, t_complete = -1;

static struct page *page_at(size_t p)   /* mu held */
{
    if (p >= npages) {
        size_t n = npages ? npages : 64;
        while (n <= p) n *= 2;
        pages = realloc(pages, n * sizeof *pages);
        memset(pages + npages, 0, (n - npages) * sizeof *pages);
        npages = n;
    }
    if (!pages[p]) pages[p] = calloc(1, sizeof **pages);
    return pages[p];
}

/* ------------------------------------------------------- service thread */

static const char *folder;
static int sock = -1;
static int64_t next_id = 1;

static int connect_service(void)
{
    char path[4096];
    const char *s = getenv("OSD_SOCKET"), *rt = getenv("OSD_RUNTIME_DIR"), *x = getenv("XDG_RUNTIME_DIR");
    if (s && *s) snprintf(path, sizeof path, "%s", s);
    else if (rt && *rt) snprintf(path, sizeof path, "%s/objectspace.sock", rt);
    else if (x && *x) snprintf(path, sizeof path, "%s/objectspace.sock", x);
    else snprintf(path, sizeof path, "/tmp/objectspace-%u/objectspace.sock", (unsigned)getuid());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof a.sun_path) {
        snprintf(service_error, sizeof service_error, "socket path too long: %s", path);
        close(fd);
        return -1;
    }
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        snprintf(service_error, sizeof service_error, "cannot reach osdd at %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static bool io_all(int fd, void *p, size_t n, bool wr)
{
    unsigned char *b = p;
    while (n) {
        ssize_t r = wr ? write(fd, b, n) : read(fd, b, n);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
        b += r;
        n -= (size_t)r;
    }
    return true;
}

/* One list request, answered in order (one connection, one at a time). */
static bool fetch(size_t offset, size_t count, bool details, cv *out)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 6);
    cb_text(&b, "id"); cb_int(&b, next_id++);
    cb_text(&b, "op"); cb_text(&b, "list");
    cb_text(&b, "target"); cb_text(&b, folder);
    cb_text(&b, "offset"); cb_uint(&b, offset);
    cb_text(&b, "count"); cb_uint(&b, count);
    cb_text(&b, "details"); cb_bool(&b, details);
    unsigned char hdr[4] = { (unsigned char)(b.n >> 24), (unsigned char)(b.n >> 16), (unsigned char)(b.n >> 8), (unsigned char)b.n };
    bool ok = io_all(sock, hdr, 4, true) && io_all(sock, b.d, b.n, true);
    cb_free(&b);
    if (!ok || !io_all(sock, hdr, 4, false)) return false;
    size_t len = (size_t)hdr[0] << 24 | (size_t)hdr[1] << 16 | (size_t)hdr[2] << 8 | hdr[3];
    unsigned char *buf = malloc(len);
    ok = io_all(sock, buf, len, false) && cv_decode(buf, len, out) == 0;
    free(buf);
    return ok;
}

static void store(const cv *reply, bool details)
{
    const cv *res = cv_get(reply, "result");
    if (!res) {
        const char *m = cv_text(reply, "message");
        pthread_mutex_lock(&mu);
        snprintf(service_error, sizeof service_error, "osdd: %s", m ? m : "error");
        pthread_mutex_unlock(&mu);
        return;
    }
    int64_t off = 0, tot = 0;
    cv_int(res, "offset", &off);
    cv_int(res, "total", &tot);
    const cv *cmp = cv_get(res, "complete"), *items = cv_get(res, "items");
    size_t n = items && items->k == CV_ARRAY ? items->n : 0;
    /* Build the rows outside the lock; swap them in under it. */
    struct row *rows = calloc(n ? n : 1, sizeof *rows);
    for (size_t i = 0; i < n; i++) {
        const cv *it = &items->items[i];
        const char *nm = cv_text(it, "name"), *ty = cv_text(it, "type"), *ti = cv_text(it, "title");
        rows[i].name = strdup(nm ? nm : "?");
        rows[i].type = ty ? ty[0] : 'o';
        if (ti && *ti) rows[i].title = strdup(ti);
        cv_int(it, "size", &rows[i].size);
        cv_int(it, "mtime", &rows[i].mtime);
    }
    pthread_mutex_lock(&mu);
    for (size_t i = 0; i < n; i++) {
        size_t r = (size_t)off + i;
        struct page *pg = page_at(r / PAGE);
        struct row *dst = &pg->rows[r % PAGE];
        free(dst->name);
        free(dst->title);
        *dst = rows[i];
    }
    for (size_t p = (size_t)off / PAGE; n && p <= ((size_t)off + n - 1) / PAGE; p++) {
        struct page *pg = page_at(p);
        if (details) pg->state = P_DETAILED;
        else if (pg->state == P_NONE) pg->state = P_NAMES;
        pg->requested = false;
    }
    if ((size_t)tot > total) total = (size_t)tot;
    complete = cmp && cmp->k == CV_BOOL && cmp->b;
    if (total && t_first_rows < 0) t_first_rows = now_ms() - t_start;
    if (complete && t_complete < 0) t_complete = now_ms() - t_start;
    pthread_mutex_unlock(&mu);
    free(rows);
    uint64_t one = 1;
    ssize_t w = write(wake_fd, &one, sizeof one);
    (void)w;
}

static void *service_main(void *arg)
{
    (void)arg;
    sock = connect_service();
    if (sock < 0) {
        uint64_t one = 1;
        ssize_t w = write(wake_fd, &one, sizeof one);
        (void)w;
        return NULL;
    }
    pthread_mutex_lock(&mu);
    while (!quitting) {
        /* 1. Details for the pages on screen (and one either side). */
        size_t want = SIZE_MAX;
        size_t lo = vis_first_page ? vis_first_page - 1 : 0, hi = vis_last_page + 1;
        for (size_t p = vis_first_page; p <= hi && want == SIZE_MAX; p++)
            if (p * PAGE < total && (p >= npages || !pages[p] || pages[p]->state != P_DETAILED)) want = p;
        for (size_t p = lo; p < vis_first_page && want == SIZE_MAX; p++)
            if (p * PAGE < total && (p >= npages || !pages[p] || pages[p]->state != P_DETAILED)) want = p;
        if (want != SIZE_MAX) {
            page_at(want)->requested = true;
            pthread_mutex_unlock(&mu);
            cv r;
            bool ok = fetch(want * PAGE, PAGE, true, &r);
            if (ok) { store(&r, true); cv_free(&r); }
            pthread_mutex_lock(&mu);
            if (!ok) break;
            continue;
        }
        /* 2. The rest of the folder, names only, as it streams in. */
        if (!complete) {
            size_t at = total, cnt = total ? 4096 : 128;
            pthread_mutex_unlock(&mu);
            cv r;
            bool ok = fetch(at, cnt, false, &r);
            if (ok) { store(&r, false); cv_free(&r); }
            pthread_mutex_lock(&mu);
            if (!ok) break;
            continue;
        }
        pthread_cond_wait(&want_cv, &mu);
    }
    if (!quitting && !service_error[0]) snprintf(service_error, sizeof service_error, "lost the connection to osdd");
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* ---------------------------------------------------------------- text */

static FT_Library ft;
static FT_Face face, face_bold;
static hb_font_t *hb_reg, *hb_bold;
static cairo_font_face_t *cf_reg, *cf_bold;

static FT_Face load_face(const char *pattern)
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
    if (f) FT_Set_Char_Size(f, 0, (FT_F26Dot6)(FONT_PX * 64), 72, 72);
    return f;
}

static void fonts_init(void)
{
    FT_Init_FreeType(&ft);
    face = load_face("sans-serif");
    face_bold = load_face("sans-serif:bold");
    if (!face || !face_bold) { fprintf(stderr, "folderview: no font\n"); exit(1); }
    hb_reg = hb_ft_font_create_referenced(face);
    hb_bold = hb_ft_font_create_referenced(face_bold);
    hb_ft_font_set_load_flags(hb_reg, FT_LOAD_NO_HINTING);
    hb_ft_font_set_load_flags(hb_bold, FT_LOAD_NO_HINTING);
    cf_reg = cairo_ft_font_face_create_for_ft_face(face, FT_LOAD_NO_HINTING);
    cf_bold = cairo_ft_font_face_create_for_ft_face(face_bold, FT_LOAD_NO_HINTING);
}

/* Shape with HarfBuzz and draw with Cairo; cut off at max_w with "...". */
static double draw_text(cairo_t *cr, double x, double baseline, const char *s, double max_w, bool bold)
{
    hb_buffer_t *b = hb_buffer_create();
    hb_buffer_add_utf8(b, s, -1, 0, -1);
    hb_buffer_guess_segment_properties(b);
    hb_shape(bold ? hb_bold : hb_reg, b, NULL, 0);
    unsigned n;
    hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(b, &n);
    hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(b, &n);
    cairo_glyph_t gl[512];
    double pen = 0;
    unsigned k = 0;
    bool cut = false;
    for (unsigned i = 0; i < n && k < 510; i++) {
        double adv = gp[i].x_advance / 64.0;
        if (max_w > 0 && pen + adv > max_w - 8) { cut = true; break; }
        gl[k].index = gi[i].codepoint;
        gl[k].x = x + pen + gp[i].x_offset / 64.0;
        gl[k].y = baseline - gp[i].y_offset / 64.0;
        k++;
        pen += adv;
    }
    cairo_set_font_face(cr, bold ? cf_bold : cf_reg);
    cairo_set_font_size(cr, FONT_PX);
    cairo_show_glyphs(cr, gl, (int)k);
    if (cut) {
        cairo_move_to(cr, x + pen, baseline);
        cairo_show_text(cr, "...");
    }
    hb_buffer_destroy(b);
    return pen;
}

/* ------------------------------------------------------------- wayland */

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm;
static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *kstate;
static int width = 760, height = 560, scale = 1, comp_version;
static bool configured, running = true, dirty = true, frame_pending, maximized, activated = true;

struct buffer {
    struct wl_buffer *wb;
    void *data;
    size_t size;
    int w, h;
    bool busy;
};
static struct buffer bufs[2];

static void buf_release(void *d, struct wl_buffer *wb) { (void)wb; ((struct buffer *)d)->busy = false; }
static const struct wl_buffer_listener buf_listener = { buf_release };

static struct buffer *get_buffer(int w, int h)
{
    for (int i = 0; i < 2; i++) {
        struct buffer *b = &bufs[i];
        if (b->busy) continue;
        if (b->wb && (b->w != w || b->h != h)) {
            wl_buffer_destroy(b->wb);
            munmap(b->data, b->size);
            b->wb = NULL;
        }
        if (!b->wb) {
            int stride = w * 4;
            b->size = (size_t)stride * h;
            int fd = memfd_create("wps-buffer", MFD_CLOEXEC);
            if (fd < 0 || ftruncate(fd, (off_t)b->size) < 0) return NULL;
            b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)b->size);
            b->wb = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
            wl_buffer_add_listener(b->wb, &buf_listener, b);
            wl_shm_pool_destroy(pool);
            close(fd);
            b->w = w;
            b->h = h;
        }
        return b;
    }
    return NULL;
}

/* --------------------------------------------------------------- view */

static double scroll_y;          /* in pixels */
static double ptr_x, ptr_y;
static uint32_t ptr_serial;
static bool dragging_thumb;
static double drag_off;

static double list_top(void) { return BORDER + TITLE_H + HEADER_H; }
static double list_h(void) { return height - BORDER * 2 - TITLE_H - HEADER_H - STATUS_H; }

static double max_scroll(void)
{
    double m = (double)total * ROW_H - list_h();
    return m > 0 ? m : 0;
}

static void clamp_scroll(void)
{
    if (scroll_y > max_scroll()) scroll_y = max_scroll();
    if (scroll_y < 0) scroll_y = 0;
}

/* A raised or sunken 3D edge, as on the OS/2 frame controls. */
static void bevel(cairo_t *cr, double x, double y, double w, double h, bool raised)
{
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, raised ? 1 : 0.5, raised ? 1 : 0.5, raised ? 1 : 0.5);
    cairo_move_to(cr, x + 0.5, y + h - 0.5);
    cairo_line_to(cr, x + 0.5, y + 0.5);
    cairo_line_to(cr, x + w - 0.5, y + 0.5);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, raised ? 0.5 : 1, raised ? 0.5 : 1, raised ? 0.5 : 1);
    cairo_move_to(cr, x + w - 0.5, y + 0.5);
    cairo_line_to(cr, x + w - 0.5, y + h - 0.5);
    cairo_line_to(cr, x + 0.5, y + h - 0.5);
    cairo_stroke(cr);
}

enum { HIT_NONE, HIT_TITLE, HIT_MIN, HIT_MAX, HIT_CLOSE, HIT_THUMB, HIT_TROUGH, HIT_BORDER };

static void button_rect(int which, double *x, double *y, double *w, double *h)
{
    *w = 18; *h = 16;
    *y = BORDER + 3;
    *x = width - BORDER - 3 - (3 - which) * 20;   /* 0 min, 1 max, 2 close */
}

static void thumb_rect(double *y, double *h)
{
    double th = list_h(), tot = (double)total * ROW_H;
    *h = tot > th ? fmax(20, th * th / tot) : th;
    *y = list_top() + (max_scroll() > 0 ? (th - *h) * scroll_y / max_scroll() : 0);
}

static int hit(double x, double y)
{
    if (x < BORDER || y < BORDER || x >= width - BORDER || y >= height - BORDER) return HIT_BORDER;
    for (int i = 0; i < 3; i++) {
        double bx, by, bw, bh;
        button_rect(i, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) return HIT_MIN + i;
    }
    if (y < BORDER + TITLE_H) return HIT_TITLE;
    if (x >= width - BORDER - SCROLL_W && y >= list_top() && y < list_top() + list_h()) {
        double ty, th;
        thumb_rect(&ty, &th);
        return y >= ty && y < ty + th ? HIT_THUMB : HIT_TROUGH;
    }
    return HIT_NONE;
}

/* Frame statistics (bench). */
static double *draw_ms, *interval_ms;
static int *placeholders;
static size_t nframes, cap_frames;
static double last_frame_t;

struct snap { char name[96], title[96], size[24], date[24]; char type; bool loaded, detailed; };
static int *undetailed;

static void draw(void)
{
    int bw = width * scale, bh = height * scale;
    struct buffer *b = get_buffer(bw, bh);
    if (!b) return;   /* both buffers still with the compositor: next frame */
    double t0 = now_ms();
    cairo_surface_t *cs = cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_ARGB32, bw, bh, bw * 4);
    cairo_surface_set_device_scale(cs, scale, scale);
    cairo_t *cr = cairo_create(cs);

    /* Snapshot what is on screen (strings copied), then draw unlocked. */
    clamp_scroll();
    size_t first = (size_t)(scroll_y / ROW_H);
    int nvis = (int)(list_h() / ROW_H) + 2;
    struct snap rows[128];
    if (nvis > 128) nvis = 128;
    size_t tot;
    bool cmp;
    char err[256];
    pthread_mutex_lock(&mu);
    tot = total;
    cmp = complete;
    snprintf(err, sizeof err, "%s", service_error);
    for (int i = 0; i < nvis; i++) {
        size_t r = first + (size_t)i;
        struct snap *s = &rows[i];
        s->loaded = s->detailed = false;
        if (r >= total) continue;
        struct page *pg = r / PAGE < npages ? pages[r / PAGE] : NULL;
        if (!pg || pg->state == P_NONE || !pg->rows[r % PAGE].name) continue;
        struct row *src = &pg->rows[r % PAGE];
        s->loaded = true;
        s->detailed = pg->state == P_DETAILED;
        s->type = src->type;
        snprintf(s->name, sizeof s->name, "%s", src->name);
        snprintf(s->title, sizeof s->title, "%s", src->title ? src->title : src->name);
        if (s->detailed) {
            snprintf(s->size, sizeof s->size, "%lld", (long long)src->size);
            struct tm tm;
            time_t t = (time_t)src->mtime;
            localtime_r(&t, &tm);
            strftime(s->date, sizeof s->date, "%Y-%m-%d %H:%M", &tm);
        }
    }
    size_t vf = first / PAGE, vl = (first + (size_t)nvis) / PAGE;
    if (vf != vis_first_page || vl != vis_last_page) {
        vis_first_page = vf;
        vis_last_page = vl;
        pthread_cond_signal(&want_cv);
    }
    pthread_mutex_unlock(&mu);

    /* Frame: grey with a raised edge; title bar dark blue when active. */
    cairo_set_source_rgb(cr, 0.80, 0.80, 0.80);
    cairo_paint(cr);
    bevel(cr, 0, 0, width, height, true);
    bevel(cr, BORDER - 1, BORDER - 1, width - 2 * BORDER + 2, height - 2 * BORDER + 2, false);
    if (activated) cairo_set_source_rgb(cr, 0.0, 0.0, 0.50);
    else cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
    cairo_rectangle(cr, BORDER, BORDER, width - 2 * BORDER, TITLE_H);
    cairo_fill(cr);
    char title[512];
    const char *base = strrchr(folder, '/') && strrchr(folder, '/')[1] ? strrchr(folder, '/') + 1 : folder;
    snprintf(title, sizeof title, "%s - Details View", base);
    cairo_set_source_rgb(cr, 1, 1, 1);
    draw_text(cr, BORDER + 8, BORDER + 16, title, width - 2 * BORDER - 80, true);
    for (int i = 0; i < 3; i++) {
        double x, y, w, h;
        button_rect(i, &x, &y, &w, &h);
        cairo_set_source_rgb(cr, 0.80, 0.80, 0.80);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
        bevel(cr, x, y, w, h, true);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_set_line_width(cr, 1.5);
        if (i == 0) { cairo_rectangle(cr, x + 7, y + 7, 4, 3); cairo_fill(cr); }
        else if (i == 1) { cairo_rectangle(cr, x + 4.5, y + 3.5, w - 9, h - 7); cairo_stroke(cr); }
        else {
            cairo_move_to(cr, x + 5, y + 4); cairo_line_to(cr, x + w - 5, y + h - 4);
            cairo_move_to(cr, x + w - 5, y + 4); cairo_line_to(cr, x + 5, y + h - 4);
            cairo_stroke(cr);
        }
    }

    /* Column headings. */
    double cx[5] = { BORDER + 6, BORDER + 30, BORDER + 300, 0, 0 };
    double lw = width - 2 * BORDER - SCROLL_W;
    cx[3] = BORDER + lw - 260;
    cx[4] = BORDER + lw - 150;
    if (cx[2] > cx[3] - 40) cx[2] = cx[3] - 40;
    double hy = BORDER + TITLE_H;
    cairo_set_source_rgb(cr, 0.80, 0.80, 0.80);
    cairo_rectangle(cr, BORDER, hy, lw, HEADER_H);
    cairo_fill(cr);
    static const char *heads[5] = { "", "Title", "Name", "Size", "Last write" };
    for (int c = 1; c < 5; c++) {
        double right = c < 4 ? cx[c + 1] : BORDER + lw;
        bevel(cr, cx[c] - 4, hy, right - cx[c] + 4, HEADER_H, true);
        cairo_set_source_rgb(cr, 0, 0, 0);
        draw_text(cr, cx[c], hy + 14, heads[c], right - cx[c], false);
    }

    /* Rows. */
    cairo_save(cr);
    cairo_rectangle(cr, BORDER, list_top(), lw, list_h());
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    int ph = 0, und = 0;
    double y0 = list_top() - fmod(scroll_y, ROW_H);
    for (int i = 0; i < nvis; i++) {
        double y = y0 + i * ROW_H;
        size_t r = first + (size_t)i;
        if (r >= tot) break;
        struct snap *s = &rows[i];
        if (r % 2) {
            cairo_set_source_rgb(cr, 0.95, 0.95, 0.97);
            cairo_rectangle(cr, BORDER, y, lw, ROW_H);
            cairo_fill(cr);
        }
        if (!s->loaded) {
            ph++;
            cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
            cairo_rectangle(cr, cx[1], y + 6, 160, 8);
            cairo_fill(cr);
            continue;
        }
        /* A small type mark stands in for the icon. */
        if (s->type == 'f') cairo_set_source_rgb(cr, 0.85, 0.70, 0.20);
        else if (s->type == 'l') cairo_set_source_rgb(cr, 0.30, 0.60, 0.30);
        else cairo_set_source_rgb(cr, 0.55, 0.60, 0.75);
        cairo_rectangle(cr, cx[0] + 2, y + 3, 14, 14);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0, 0, 0);
        draw_text(cr, cx[1], y + 14, s->title, cx[2] - cx[1], false);
        draw_text(cr, cx[2], y + 14, s->name, cx[3] - cx[2], false);
        if (s->detailed) {
            draw_text(cr, cx[3], y + 14, s->size, cx[4] - cx[3], false);
            draw_text(cr, cx[4], y + 14, s->date, BORDER + lw - cx[4], false);
        } else {
            und++;
            cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
            cairo_rectangle(cr, cx[3], y + 6, 60, 8);
            cairo_rectangle(cr, cx[4], y + 6, 100, 8);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);

    /* Scroll bar. */
    double sx = width - BORDER - SCROLL_W;
    cairo_set_source_rgb(cr, 0.72, 0.72, 0.72);
    cairo_rectangle(cr, sx, list_top(), SCROLL_W, list_h());
    cairo_fill(cr);
    double ty, th;
    thumb_rect(&ty, &th);
    cairo_set_source_rgb(cr, 0.80, 0.80, 0.80);
    cairo_rectangle(cr, sx, ty, SCROLL_W, th);
    cairo_fill(cr);
    bevel(cr, sx, ty, SCROLL_W, th, true);

    /* Status line. */
    double sy = height - BORDER - STATUS_H;
    cairo_set_source_rgb(cr, 0.80, 0.80, 0.80);
    cairo_rectangle(cr, BORDER, sy, width - 2 * BORDER, STATUS_H);
    cairo_fill(cr);
    bevel(cr, BORDER, sy, width - 2 * BORDER, STATUS_H, false);
    char status[400];
    if (err[0]) snprintf(status, sizeof status, "%s", err);
    else snprintf(status, sizeof status, "%zu objects%s", tot, cmp ? "" : " (still reading...)");
    cairo_set_source_rgb(cr, 0, 0, 0);
    draw_text(cr, BORDER + 6, sy + 14, status, width - 2 * BORDER - 12, false);

    cairo_destroy(cr);
    cairo_surface_flush(cs);
    cairo_surface_destroy(cs);
    double t1 = now_ms();

    wl_surface_set_buffer_scale(surf, scale);
    wl_surface_attach(surf, b->wb, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, bw, bh);
    b->busy = true;

    if (nframes == cap_frames) {
        cap_frames = cap_frames ? cap_frames * 2 : 4096;
        draw_ms = realloc(draw_ms, cap_frames * sizeof *draw_ms);
        interval_ms = realloc(interval_ms, cap_frames * sizeof *interval_ms);
        placeholders = realloc(placeholders, cap_frames * sizeof *placeholders);
        undetailed = realloc(undetailed, cap_frames * sizeof *undetailed);
    }
    undetailed[nframes] = und;
    draw_ms[nframes] = t1 - t0;
    interval_ms[nframes] = last_frame_t > 0 ? t0 - last_frame_t : 0;
    placeholders[nframes] = ph;
    nframes++;
    last_frame_t = t0;
}

static void frame_done(void *d, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_listener = { frame_done };

static double bench_secs, bench_start = -1, bench_next_jump;
static unsigned bench_seed = 12345;
static size_t bench_frames_start;

static void bench_step(void)
{
    double t = now_ms();
    if (bench_start < 0) {
        pthread_mutex_lock(&mu);
        bool ready = total > 0 || complete, failed = service_error[0] != 0;
        pthread_mutex_unlock(&mu);
        if (failed) { running = false; return; }
        if (!ready) return;
        bench_start = t;
        bench_next_jump = t + 3000;
        bench_frames_start = nframes;
    }
    if (t - bench_start > bench_secs * 1000) { running = false; return; }
    if (t < bench_next_jump) {
        scroll_y += 60;   /* a fling: 3 rows a frame */
    } else {
        /* A jump, as if the scroll bar thumb were dragged. */
        scroll_y = (double)(rand_r(&bench_seed) % 1000000) / 1000000.0 * max_scroll();
        bench_next_jump = t + 500;
    }
}

static void request_frame(void)
{
    if (frame_pending || !configured) return;
    if (bench_secs > 0) bench_step();
    draw();
    struct wl_callback *cb = wl_surface_frame(surf);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    wl_surface_commit(surf);
    frame_pending = true;
    dirty = false;
}

static void frame_done(void *d, struct wl_callback *cb, uint32_t t)
{
    (void)d; (void)t;
    wl_callback_destroy(cb);
    frame_pending = false;
    if (dirty || bench_secs > 0) request_frame();
}

static void redraw(void) { dirty = true; request_frame(); }

/* xdg-shell */
static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t serial) { (void)d; xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xs_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    (void)d;
    xdg_surface_ack_configure(s, serial);
    configured = true;
    redraw();
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
    (void)d; (void)t;
    if (w > 0) width = w;
    if (h > 0) height = h;
    maximized = false;
    activated = false;
    uint32_t *s;
    wl_array_for_each(s, states) {
        if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED) maximized = true;
        if (*s == XDG_TOPLEVEL_STATE_ACTIVATED) activated = true;
    }
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; running = false; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_bounds, top_caps };

/* Surface scale (wl_surface v6). */
static void surf_enter(void *d, struct wl_surface *s, struct wl_output *o) { (void)d; (void)s; (void)o; }
static void surf_leave(void *d, struct wl_surface *s, struct wl_output *o) { (void)d; (void)s; (void)o; }
static bool scale_forced;
static void surf_scale(void *d, struct wl_surface *s, int32_t f) { (void)d; (void)s; if (!scale_forced && f > 0 && f != scale) { scale = f; redraw(); } }
static void surf_transform(void *d, struct wl_surface *s, uint32_t t) { (void)d; (void)s; (void)t; }
static const struct wl_surface_listener surf_listener = { surf_enter, surf_leave, surf_scale, surf_transform };

/* Pointer: wheel scrolls; title bar moves; buttons; scroll bar; border resizes. */
static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)s; ptr_serial = serial; ptr_x = wl_fixed_to_double(x); ptr_y = wl_fixed_to_double(y); }
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)d; (void)p; (void)serial; (void)s; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)t;
    ptr_x = wl_fixed_to_double(x);
    ptr_y = wl_fixed_to_double(y);
    if (dragging_thumb) {
        double ty, th;
        thumb_rect(&ty, &th);
        double range = list_h() - th;
        if (range > 0) scroll_y = (ptr_y - drag_off - list_top()) / range * max_scroll();
        redraw();
    }
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state)
{
    (void)d; (void)p; (void)t;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) { dragging_thumb = false; return; }
    if (button != BTN_LEFT) return;
    int h = hit(ptr_x, ptr_y);
    if (h == HIT_TITLE) xdg_toplevel_move(top, seat, serial);
    else if (h == HIT_MIN) xdg_toplevel_set_minimized(top);
    else if (h == HIT_MAX) maximized ? xdg_toplevel_unset_maximized(top) : xdg_toplevel_set_maximized(top);
    else if (h == HIT_CLOSE) running = false;
    else if (h == HIT_THUMB) {
        double ty, th;
        thumb_rect(&ty, &th);
        dragging_thumb = true;
        drag_off = ptr_y - ty;
    } else if (h == HIT_TROUGH) {
        double ty, th;
        thumb_rect(&ty, &th);
        scroll_y += (ptr_y < ty ? -1 : 1) * list_h();
        redraw();
    } else if (h == HIT_BORDER) {
        uint32_t edge = 0;
        if (ptr_y < BORDER * 3) edge |= XDG_TOPLEVEL_RESIZE_EDGE_TOP;
        if (ptr_y >= height - BORDER * 3) edge |= XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
        if (ptr_x < BORDER * 3) edge |= XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
        if (ptr_x >= width - BORDER * 3) edge |= XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
        if (edge) xdg_toplevel_resize(top, seat, serial, edge);
    }
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v)
{
    (void)d; (void)p; (void)t;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    scroll_y += wl_fixed_to_double(v) * 2;
    redraw();
}
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void p_axis_v120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void p_axis_rel(void *d, struct wl_pointer *p, uint32_t a, uint32_t r) { (void)d; (void)p; (void)a; (void)r; }
static const struct wl_pointer_listener p_listener = {
    .enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis, .frame = p_frame,
    .axis_source = p_axis_source, .axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
    .axis_value120 = p_axis_v120, .axis_relative_direction = p_axis_rel,
};

/* Keyboard: Page Up/Down, Home, End, arrows; q or Escape closes. */
static void k_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
    (void)d; (void)k;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    if (keymap) xkb_keymap_unref(keymap);
    if (kstate) xkb_state_unref(kstate);
    keymap = xkb_keymap_new_from_string(xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    kstate = keymap ? xkb_state_new(keymap) : NULL;
}
static void k_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) { (void)d; (void)k; (void)s; (void)sf; (void)keys; }
static void k_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { (void)d; (void)k; (void)s; (void)sf; }
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state)
{
    (void)d; (void)k; (void)serial; (void)t;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !kstate) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(kstate, key + 8);
    switch (sym) {
    case XKB_KEY_Page_Down: scroll_y += list_h(); break;
    case XKB_KEY_Page_Up: scroll_y -= list_h(); break;
    case XKB_KEY_Down: scroll_y += ROW_H; break;
    case XKB_KEY_Up: scroll_y -= ROW_H; break;
    case XKB_KEY_Home: scroll_y = 0; break;
    case XKB_KEY_End: scroll_y = max_scroll(); break;
    case XKB_KEY_q: case XKB_KEY_Escape: running = false; return;
    default: return;
    }
    redraw();
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{ (void)d; (void)k; (void)s; if (kstate) xkb_state_update_mask(kstate, dep, lat, lock, 0, 0, grp); }
static void k_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) { (void)d; (void)k; (void)r; (void)dl; }
static const struct wl_keyboard_listener k_listener = { k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &p_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &k_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        comp_version = ver < 6 ? (int)ver : 6;
        comp = wl_registry_bind(r, name, &wl_compositor_interface, (uint32_t)comp_version);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
        xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 8 ? ver : 8);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* ------------------------------------------------------------- results */

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double pct(double *v, size_t n, double p)
{
    if (!n) return 0;
    qsort(v, n, sizeof *v, cmp_d);
    size_t i = (size_t)(p / 100.0 * (double)(n - 1) + 0.5);
    return v[i];
}

static void report(void)
{
    size_t s = bench_frames_start + 1, n = nframes > s ? nframes - s : 0;
    double *d = malloc((n ? n : 1) * sizeof *d), *iv = malloc((n ? n : 1) * sizeof *iv);
    size_t over = 0, ph_frames = 0, ph_rows = 0, und_frames = 0;
    for (size_t i = 0; i < n; i++) {
        d[i] = draw_ms[s + i];
        iv[i] = interval_ms[s + i];
        over += d[i] > 16.7;
        ph_frames += placeholders[s + i] > 0;
        ph_rows += (size_t)placeholders[s + i];
        und_frames += undetailed[s + i] > 0;
    }
    /* Each statistic sorts its array; take them one at a time. */
    double d50 = pct(d, n, 50), d99 = pct(d, n, 99), dmax = n ? d[n - 1] : 0;
    double i50 = pct(iv, n, 50), i99 = pct(iv, n, 99), imax = n ? iv[n - 1] : 0;
    double secs = n ? (last_frame_t - bench_start) / 1000.0 : 0;
    size_t missed = 0;
    for (size_t i = 0; i < n; i++) missed += iv[i] > i50 * 1.5;
    pthread_mutex_lock(&mu);
    printf("{\"folder\":\"%s\",\"total\":%zu,\"complete\":%s,\"first_rows_ms\":%.1f,\"complete_ms\":%.1f,"
           "\"frames\":%zu,\"fps\":%.1f,\"scale\":%d,\"size\":[%d,%d],"
           "\"draw_ms\":{\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f},\"draw_over_16ms\":%zu,"
           "\"interval_ms\":{\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f},\"late_frames\":%zu,"
           "\"frames_with_placeholders\":%zu,\"placeholder_rows\":%zu,\"frames_waiting_for_details\":%zu,\"error\":\"%s\"}\n",
           folder, total, complete ? "true" : "false", t_first_rows, t_complete, n, secs > 0 ? (double)n / secs : 0,
           scale, width, height, d50, d99, dmax, over, i50, i99, imax, missed, ph_frames, ph_rows, und_frames, service_error);
    pthread_mutex_unlock(&mu);
    free(d);
    free(iv);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: folderview FOLDER [--bench SECONDS]\n"); return 2; }
    folder = argv[1];
    if (argc == 4 && !strcmp(argv[2], "--bench")) bench_secs = atof(argv[3]);
    if (getenv("FOLDERVIEW_SCALE")) { scale = atoi(getenv("FOLDERVIEW_SCALE")); scale_forced = scale > 0; if (scale < 1) scale = 1; }
    if (getenv("FOLDERVIEW_SIZE")) sscanf(getenv("FOLDERVIEW_SIZE"), "%dx%d", &width, &height);
    t_start = now_ms();
    wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    pthread_t svc;
    pthread_create(&svc, NULL, service_main, NULL);

    fonts_init();
    xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "folderview: no Wayland display\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm) { fprintf(stderr, "folderview: compositor lacks xdg-shell or shm\n"); return 1; }
    surf = wl_compositor_create_surface(comp);
    if (comp_version >= 6) wl_surface_add_listener(surf, &surf_listener, NULL);
    xsurf = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xsurf, &xs_listener, NULL);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_title(top, folder);
    xdg_toplevel_set_app_id(top, "io.github.sungsookhim.Objectspace.folderview");
    wl_surface_commit(surf);

    int wfd = wl_display_get_fd(dpy);
    while (running) {
        while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        struct pollfd pf[2] = { { wfd, POLLIN, 0 }, { wake_fd, POLLIN, 0 } };
        int r = poll(pf, 2, bench_secs > 0 ? 100 : -1);
        if (r > 0 && (pf[0].revents & POLLIN)) wl_display_read_events(dpy);
        else wl_display_cancel_read(dpy);
        wl_display_dispatch_pending(dpy);
        if (pf[1].revents & POLLIN) {
            uint64_t x;
            ssize_t rd = read(wake_fd, &x, sizeof x);
            (void)rd;
            redraw();   /* new rows from the service thread */
        }
        if (bench_secs > 0 && !frame_pending) request_frame();
    }
    if (bench_secs > 0) {
        report();
        if (service_error[0]) fprintf(stderr, "folderview: %s\n", service_error);
    }

    pthread_mutex_lock(&mu);
    quitting = true;
    pthread_cond_signal(&want_cv);
    pthread_mutex_unlock(&mu);
    if (sock >= 0) shutdown(sock, SHUT_RDWR);
    pthread_join(svc, NULL);
    return 0;
}
