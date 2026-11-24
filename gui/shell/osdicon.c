/* Icon packages (osdicon.h): a small zip reader and writer (stored and
 * deflated entries, through zlib), and the size rules with cairo. */
#define _GNU_SOURCE
#include "osdicon.h"

#include <librsvg/rsvg.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>

const int osdicon_standard[5] = { 256, 128, 96, 64, 32 };

#define MAX_PICS 32

struct pic { int size; unsigned char *png; size_t len; };

struct osdicon {
    struct pic pics[MAX_PICS];   /* by size, smallest first */
    size_t n;
    unsigned char *svg;
    size_t svglen;
};

/* ------------------------------------------------------------ bytes */

static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const unsigned char *p) { return rd16(p) | rd16(p + 2) << 16; }

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size < 0 || st.st_size > (64 << 20)) { fclose(f); return NULL; }
    unsigned char *b = malloc((size_t)st.st_size + 1);
    size_t r = b ? fread(b, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!b || r != (size_t)st.st_size) { free(b); return NULL; }
    *len = r;
    return b;
}

/* A PNG's width and height from its header (0 if it is not one). */
static void png_dims(const unsigned char *b, size_t len, int *w, int *h)
{
    *w = *h = 0;
    if (len < 24 || memcmp(b, "\x89PNG\r\n\x1a\n", 8) || memcmp(b + 12, "IHDR", 4)) return;
    *w = (int)((uint32_t)b[16] << 24 | (uint32_t)b[17] << 16 | (uint32_t)b[18] << 8 | b[19]);
    *h = (int)((uint32_t)b[20] << 24 | (uint32_t)b[21] << 16 | (uint32_t)b[22] << 8 | b[23]);
}

static bool add_pic(osdicon *w, int size, unsigned char *png, size_t len)
{
    for (size_t i = 0; i < w->n; i++)
        if (w->pics[i].size == size) { free(w->pics[i].png); w->pics[i].png = png; w->pics[i].len = len; return true; }
    if (w->n == MAX_PICS || size < 1 || size > 4096) return false;
    size_t at = 0;
    while (at < w->n && w->pics[at].size < size) at++;
    memmove(&w->pics[at + 1], &w->pics[at], (w->n - at) * sizeof *w->pics);
    w->pics[at] = (struct pic){ size, png, len };
    w->n++;
    return true;
}

/* -------------------------------------------------------- the zip file */

static unsigned char *inflate_raw(const unsigned char *in, size_t inlen, size_t outlen)
{
    unsigned char *out = malloc(outlen ? outlen : 1);
    if (!out) return NULL;
    z_stream z = { 0 };
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) { free(out); return NULL; }
    z.next_in = (unsigned char *)in;
    z.avail_in = (uInt)inlen;
    z.next_out = out;
    z.avail_out = (uInt)outlen;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (r != Z_STREAM_END || z.total_out != outlen) { free(out); return NULL; }
    return out;
}

static bool read_zip(osdicon *w, const unsigned char *b, size_t len)
{
    /* the end of central directory record, from the end */
    if (len < 22) return false;
    size_t e = len - 22;
    for (;;) {
        if (rd32(b + e) == 0x06054b50) break;
        if (e == 0 || len - e > 22 + 65535) return false;
        e--;
    }
    size_t count = rd16(b + e + 10), cd = rd32(b + e + 16);
    for (size_t k = 0; k < count; k++) {
        if (cd + 46 > len || rd32(b + cd) != 0x02014b50) return false;
        uint32_t method = rd16(b + cd + 10), csize = rd32(b + cd + 20), usize = rd32(b + cd + 24);
        size_t nl = rd16(b + cd + 28), xl = rd16(b + cd + 30), cl = rd16(b + cd + 32), lho = rd32(b + cd + 42);
        if (cd + 46 + nl > len) return false;
        char name[64] = "";
        if (nl < sizeof name) { memcpy(name, b + cd + 46, nl); name[nl] = 0; }
        cd += 46 + nl + xl + cl;
        if (lho + 30 > len || rd32(b + lho) != 0x04034b50) return false;
        size_t data = lho + 30 + rd16(b + lho + 26) + rd16(b + lho + 28);
        if (data + csize > len || usize > (32u << 20)) return false;
        bool svg = !strcasecmp(name, "icon.svg");
        char *end;
        long size = strtol(name, &end, 10);
        bool png = end != name && !strcasecmp(end, ".png");
        if (!svg && !png) continue;   /* (anything else: not ours, left alone) */
        unsigned char *u;
        if (method == 0 && csize == usize) { u = malloc(usize ? usize : 1); if (u) memcpy(u, b + data, usize); }
        else if (method == 8) u = inflate_raw(b + data, csize, usize);
        else continue;
        if (!u) return false;
        if (svg) { free(w->svg); w->svg = u; w->svglen = usize; continue; }
        int pw, ph;
        png_dims(u, usize, &pw, &ph);
        if (pw != size || ph != size || !add_pic(w, (int)size, u, usize)) free(u);   /* named by its size, and square */
    }
    return true;
}

osdicon *osdicon_open(const char *path)
{
    size_t len;
    unsigned char *b = read_file(path, &len);
    if (!b) return NULL;
    osdicon *w = calloc(1, sizeof *w);
    int pw, ph;
    png_dims(b, len, &pw, &ph);
    if (pw > 0 && pw == ph) {   /* a plain picture: a package of one */
        if (add_pic(w, pw, b, len)) return w;
    } else if (pw > 0) {        /* (not square: none) */
    } else if (read_zip(w, b, len) && (w->n || w->svg)) {
        free(b);
        return w;
    }
    free(b);
    osdicon_free(w);
    return NULL;
}

void osdicon_free(osdicon *w)
{
    if (!w) return;
    for (size_t i = 0; i < w->n; i++) free(w->pics[i].png);
    free(w->svg);
    free(w);
}

size_t osdicon_sizes(const osdicon *w, int *out, size_t max)
{
    size_t k = 0;
    for (size_t i = 0; i < w->n && k < max; i++) out[k++] = w->pics[i].size;
    return k;
}

bool osdicon_has_svg(const osdicon *w) { return w->svg != NULL; }

/* ------------------------------------------------------- the size rules */

struct rd { const unsigned char *p; size_t left; };
static cairo_status_t read_mem(void *c, unsigned char *data, unsigned int n)
{
    struct rd *r = c;
    if (n > r->left) return CAIRO_STATUS_READ_ERROR;
    memcpy(data, r->p, n);
    r->p += n;
    r->left -= n;
    return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *decode(const struct pic *p)
{
    struct rd r = { p->png, p->len };
    cairo_surface_t *s = cairo_image_surface_create_from_png_stream(read_mem, &r);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); return NULL; }
    return s;
}

/* n x n boxes averaged (premultiplied ARGB: averaging them is right). */
static cairo_surface_t *average_down(cairo_surface_t *src, int px, int n)
{
    cairo_surface_flush(src);
    cairo_surface_t *d = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    const unsigned char *sp = cairo_image_surface_get_data(src);
    unsigned char *dp = cairo_image_surface_get_data(d);
    int ss = cairo_image_surface_get_stride(src), ds = cairo_image_surface_get_stride(d);
    for (int y = 0; y < px; y++)
        for (int x = 0; x < px; x++) {
            uint32_t acc[4] = { 0 };
            for (int j = 0; j < n; j++) {
                const uint32_t *row = (const uint32_t *)(sp + (size_t)(y * n + j) * (size_t)ss);
                for (int i = 0; i < n; i++) {
                    uint32_t v = row[x * n + i];
                    acc[0] += v >> 24; acc[1] += (v >> 16) & 255; acc[2] += (v >> 8) & 255; acc[3] += v & 255;
                }
            }
            uint32_t nn = (uint32_t)(n * n), half = nn / 2;
            ((uint32_t *)(dp + (size_t)y * (size_t)ds))[x] = ((acc[0] + half) / nn) << 24 | ((acc[1] + half) / nn) << 16 |
                                                            ((acc[2] + half) / nn) << 8 | ((acc[3] + half) / nn);
        }
    cairo_surface_mark_dirty(d);
    return d;
}

static cairo_surface_t *scale(cairo_surface_t *src, int from, int px, cairo_filter_t filter)
{
    cairo_surface_t *d = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(d);
    cairo_scale(cr, (double)px / from, (double)px / from);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), filter);
    cairo_paint(cr);
    cairo_destroy(cr);
    return d;
}

/* The SVG drawn at px x px (librsvg), or NULL. */
static cairo_surface_t *render_svg(const unsigned char *svg, size_t len, int px)
{
    GError *err = NULL;
    RsvgHandle *h = rsvg_handle_new_from_data(svg, len, &err);
    if (!h) { if (err) g_error_free(err); return NULL; }
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(s);
    RsvgRectangle vp = { 0, 0, px, px };
    bool ok = rsvg_handle_render_document(h, cr, &vp, &err);
    cairo_destroy(cr);
    g_object_unref(h);
    if (err) g_error_free(err);
    if (!ok) { cairo_surface_destroy(s); return NULL; }
    return s;
}

/* The rules over a set of pictures (and the SVG, if there is one). */
static cairo_surface_t *render_from(const struct pic *pics, size_t n, const unsigned char *svg, size_t svglen, int px, int *how)
{
    if ((!n && !svg) || px < 1) return NULL;
    for (size_t i = 0; i < n; i++)
        if (pics[i].size == px) { if (how) *how = OSDICON_EXACT; return decode(&pics[i]); }
    if (svg && svglen < (8u << 20)) {   /* 2: the SVG at this size (a size limit: a huge one is not drawn) */
        cairo_surface_t *s = render_svg(svg, svglen, px);
        if (s) { if (how) *how = OSDICON_SVG; return s; }
    }
    if (!n) return NULL;
    for (size_t i = 0; i < n; i++)
        if (pics[i].size > px && pics[i].size % px == 0) {
            cairo_surface_t *s = decode(&pics[i]);
            if (!s) return NULL;
            cairo_surface_t *d = average_down(s, px, pics[i].size / px);
            cairo_surface_destroy(s);
            if (how) *how = OSDICON_MULTIPLE;
            return d;
        }
    for (size_t i = 0; i < n; i++)
        if (pics[i].size > px) {
            cairo_surface_t *s = decode(&pics[i]);
            if (!s) return NULL;
            cairo_surface_t *d = scale(s, pics[i].size, px, CAIRO_FILTER_GOOD);
            cairo_surface_destroy(s);
            if (how) *how = OSDICON_LARGER;
            return d;
        }
    const struct pic *p = &pics[n - 1];   /* all smaller: the largest */
    cairo_surface_t *s = decode(p);
    if (!s) return NULL;
    cairo_surface_t *d = scale(s, p->size, px, px % p->size == 0 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
    cairo_surface_destroy(s);
    if (how) *how = OSDICON_SMALLER;
    return d;
}

cairo_surface_t *osdicon_render(osdicon *w, int px, int *how)
{
    return render_from(w->pics, w->n, w->svg, w->svglen, px, how);
}

/* ----------------------------------------------------------- packing */

struct wbuf { unsigned char *b; size_t n, cap; bool oom; };

static void put(struct wbuf *w, const void *p, size_t n)
{
    if (w->oom) return;
    if (w->n + n > w->cap) {
        size_t c = w->cap ? w->cap * 2 : 65536;
        while (c < w->n + n) c *= 2;
        unsigned char *nb = realloc(w->b, c);
        if (!nb) { w->oom = true; return; }
        w->b = nb;
        w->cap = c;
    }
    memcpy(w->b + w->n, p, n);
    w->n += n;
}
static void put16(struct wbuf *w, uint32_t v) { unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) }; put(w, b, 2); }
static void put32(struct wbuf *w, uint32_t v) { put16(w, v & 0xffff); put16(w, v >> 16); }

static cairo_status_t write_mem(void *c, const unsigned char *data, unsigned int n) { put(c, data, n); return CAIRO_STATUS_SUCCESS; }

/* Stored entries (PNGs are compressed already); the time fixed, so the
 * same pictures make the same package. */
static int write_zip(const char *out, char names[][16], const unsigned char *const *data, const size_t *lens, size_t n)
{
    struct wbuf z = { 0 };
    uint32_t *off = calloc(n ? n : 1, sizeof *off), *crc = calloc(n ? n : 1, sizeof *crc);
    for (size_t i = 0; i < n; i++) {
        off[i] = (uint32_t)z.n;
        crc[i] = (uint32_t)crc32(0, data[i], (uInt)lens[i]);
        put32(&z, 0x04034b50); put16(&z, 20); put16(&z, 0); put16(&z, 0);
        put16(&z, 0); put16(&z, 0x21);   /* 1980-01-01 00:00 */
        put32(&z, crc[i]); put32(&z, (uint32_t)lens[i]); put32(&z, (uint32_t)lens[i]);
        put16(&z, (uint32_t)strlen(names[i])); put16(&z, 0);
        put(&z, names[i], strlen(names[i]));
        put(&z, data[i], lens[i]);
    }
    uint32_t cd = (uint32_t)z.n;
    for (size_t i = 0; i < n; i++) {
        put32(&z, 0x02014b50); put16(&z, 20); put16(&z, 20); put16(&z, 0); put16(&z, 0);
        put16(&z, 0); put16(&z, 0x21);
        put32(&z, crc[i]); put32(&z, (uint32_t)lens[i]); put32(&z, (uint32_t)lens[i]);
        put16(&z, (uint32_t)strlen(names[i])); put16(&z, 0); put16(&z, 0); put16(&z, 0); put16(&z, 0);
        put32(&z, 0); put32(&z, off[i]);
        put(&z, names[i], strlen(names[i]));
    }
    uint32_t cdlen = (uint32_t)z.n - cd;
    put32(&z, 0x06054b50); put16(&z, 0); put16(&z, 0); put16(&z, (uint32_t)n); put16(&z, (uint32_t)n);
    put32(&z, cdlen); put32(&z, cd); put16(&z, 0);
    free(off);
    free(crc);
    int r = -1;
    if (!z.oom) {
        char tmp[PATH_MAX + 8];
        snprintf(tmp, sizeof tmp, "%s.new", out);
        FILE *f = fopen(tmp, "wbe");
        if (f && fwrite(z.b, 1, z.n, f) == z.n && fclose(f) == 0) { f = NULL; r = rename(tmp, out); }
        if (f) fclose(f);
        if (r) remove(tmp);
    }
    free(z.b);
    return r;
}

static void add_input(osdicon *w, const char *path, char *err, size_t errn, bool *bad)
{
    size_t len;
    unsigned char *b = read_file(path, &len);
    if (!b) { snprintf(err, errn, "%s: cannot be read", path); *bad = true; return; }
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (!strcasecmp(base, "icon.svg") || (strlen(base) > 4 && !strcasecmp(base + strlen(base) - 4, ".svg"))) {
        free(w->svg);
        w->svg = b;
        w->svglen = len;
        return;
    }
    int pw, ph;
    png_dims(b, len, &pw, &ph);
    if (pw <= 0) { free(b); snprintf(err, errn, "%s: not a PNG", path); *bad = true; return; }
    if (pw != ph) { free(b); snprintf(err, errn, "%s: not square (%dx%d)", path, pw, ph); *bad = true; return; }
    if (!add_pic(w, pw, b, len)) { free(b); snprintf(err, errn, "%s: too many sizes, or one too large", path); *bad = true; }
}

int osdicon_pack(const char *const *inputs, size_t n, const char *out, char *err, size_t errn)
{
    osdicon *w = calloc(1, sizeof *w);
    bool bad = false;
    err[0] = 0;
    for (size_t i = 0; i < n && !bad; i++) {
        struct stat st;
        if (stat(inputs[i], &st) != 0) { snprintf(err, errn, "%s: %s", inputs[i], strerror(errno)); bad = true; break; }
        if (!S_ISDIR(st.st_mode)) { add_input(w, inputs[i], err, errn, &bad); continue; }
        DIR *d = opendir(inputs[i]);
        struct dirent *de;
        while (d && !bad && (de = readdir(d))) {
            size_t l = strlen(de->d_name);
            if (l < 5 || (strcasecmp(de->d_name + l - 4, ".png") && strcasecmp(de->d_name + l - 4, ".svg"))) continue;
            char p[PATH_MAX + NAME_MAX + 2];
            snprintf(p, sizeof p, "%s/%s", inputs[i], de->d_name);
            add_input(w, p, err, errn, &bad);
        }
        if (d) closedir(d);
    }
    if (!bad && !w->n && !w->svg) { snprintf(err, errn, "no pictures (PNG or SVG files) given"); bad = true; }
    if (bad) { osdicon_free(w); return -1; }
    /* the standard sizes missing: from the pictures given, by the rules */
    struct pic given[MAX_PICS];
    size_t ng = w->n;
    memcpy(given, w->pics, ng * sizeof *given);
    for (size_t k = 0; k < 5 && !bad; k++) {
        int sz = osdicon_standard[k];
        bool have = false;
        for (size_t i = 0; i < w->n; i++) have |= w->pics[i].size == sz;
        if (have) continue;
        int how = 0;
        cairo_surface_t *s = render_from(given, ng, w->svg, w->svglen, sz, &how);
        struct wbuf png = { 0 };
        if (!s || cairo_surface_write_to_png_stream(s, write_mem, &png) != CAIRO_STATUS_SUCCESS || png.oom) {
            snprintf(err, errn, "the %d px picture could not be made", sz);
            bad = true;
        } else if (!add_pic(w, sz, png.b, png.n)) {
            free(png.b);
        }
        if (s) cairo_surface_destroy(s);
    }
    int r = -1;
    if (!bad) {
        char (*names)[16] = calloc(w->n + 1, sizeof *names);
        const unsigned char **data = calloc(w->n + 1, sizeof *data);
        size_t *lens = calloc(w->n + 1, sizeof *lens), k = 0;
        for (size_t i = w->n; i-- > 0;) {   /* (largest first: the master leads) */
            snprintf(names[k], sizeof names[k], "%d.png", w->pics[i].size);
            data[k] = w->pics[i].png;
            lens[k++] = w->pics[i].len;
        }
        if (w->svg) { snprintf(names[k], sizeof names[k], "icon.svg"); data[k] = w->svg; lens[k++] = w->svglen; }
        r = write_zip(out, names, data, lens, k);
        if (r) snprintf(err, errn, "%s: cannot be written", out);
        free(names);
        free(data);
        free(lens);
    }
    osdicon_free(w);
    return r;
}
