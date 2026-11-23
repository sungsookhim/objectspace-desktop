/* Icon packages (gui/shell/wpsicon.h): packing (the standard sizes added),
 * reading back, and the size rules. */
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "wpsicon.h"

static char dir[256];

/* A square picture: a checkerboard of one-pixel squares (white and blue)
 * - averaged down 2x2 it is one even colour; scaled by a fraction, not. */
static void make_png(const char *name, int size)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    unsigned char *d = cairo_image_surface_get_data(s);
    int st = cairo_image_surface_get_stride(s);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) ((unsigned *)(d + y * st))[x] = (x + y) % 2 ? 0xff0000ffu : 0xffffffffu;
    cairo_surface_mark_dirty(s);
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    cairo_surface_write_to_png(s, p);
    cairo_surface_destroy(s);
}

static unsigned px_at(cairo_surface_t *s, int x, int y)
{
    cairo_surface_flush(s);
    return ((unsigned *)(cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s)))[x];
}

TEST(pack_adds_the_standard_sizes)
{
    make_png("big.png", 128);
    char in[512], out[512];
    snprintf(in, sizeof in, "%s/big.png", dir);
    snprintf(out, sizeof out, "%s/a.wpsicon", dir);
    const char *inputs[1] = { in };
    char err[256];
    CHECK(wpsicon_pack(inputs, 1, out, err, sizeof err) == 0);
    wpsicon *w = wpsicon_open(out);
    CHECK(w != NULL);
    int sz[16];
    size_t n = w ? wpsicon_sizes(w, sz, 16) : 0;
    CHECK(n == 5 && sz[0] == 32 && sz[1] == 64 && sz[2] == 96 && sz[3] == 128 && sz[4] == 256);
    wpsicon_free(w);
}

TEST(a_folder_and_other_sizes)
{
    char sub[512], out[512];
    snprintf(sub, sizeof sub, "%s/set", dir);
    mkdir(sub, 0755);
    make_png("set/x-48.png", 48);
    make_png("set/x-256.png", 256);
    snprintf(out, sizeof out, "%s/b.wpsicon", dir);
    const char *inputs[1] = { sub };
    char err[256];
    CHECK(wpsicon_pack(inputs, 1, out, err, sizeof err) == 0);
    wpsicon *w = wpsicon_open(out);
    int sz[16];
    size_t n = w ? wpsicon_sizes(w, sz, 16) : 0;
    CHECK(n == 6 && sz[1] == 48);   /* 32 48 64 96 128 256: its own 48 kept */
    int how = 0;
    cairo_surface_t *s = w ? wpsicon_render(w, 48, &how) : NULL;
    CHECK(s && how == WPSICON_EXACT && cairo_image_surface_get_width(s) == 48);
    if (s) cairo_surface_destroy(s);
    /* 24: 48 is a whole multiple (2x): averaged, the checkerboard one even colour */
    s = w ? wpsicon_render(w, 24, &how) : NULL;
    CHECK(s && how == WPSICON_MULTIPLE && px_at(s, 3, 3) == px_at(s, 4, 3));
    if (s) cairo_surface_destroy(s);
    /* 40: the nearest larger (48), scaled down */
    s = w ? wpsicon_render(w, 40, &how) : NULL;
    CHECK(s && how == WPSICON_LARGER && cairo_image_surface_get_width(s) == 40);
    if (s) cairo_surface_destroy(s);
    /* 512: the nearest smaller (256), by 2: nearest-neighbour, still sharp */
    s = w ? wpsicon_render(w, 512, &how) : NULL;
    CHECK(s && how == WPSICON_SMALLER && px_at(s, 0, 0) == px_at(s, 1, 1) && px_at(s, 0, 0) != px_at(s, 2, 0));
    if (s) cairo_surface_destroy(s);
    wpsicon_free(w);
}

TEST(refused)
{
    char out[512], bad[512];
    snprintf(out, sizeof out, "%s/c.wpsicon", dir);
    snprintf(bad, sizeof bad, "%s/notes.txt", dir);
    FILE *f = fopen(bad, "w");
    fputs("not a picture", f);
    fclose(f);
    const char *inputs[1] = { bad };
    char err[256];
    CHECK(wpsicon_pack(inputs, 1, out, err, sizeof err) != 0 && strstr(err, "not a PNG"));
    CHECK(wpsicon_open(bad) == NULL);
    /* a plain PNG is a package of one */
    char one[512];
    snprintf(one, sizeof one, "%s/big.png", dir);
    wpsicon *w = wpsicon_open(one);
    int sz[4];
    CHECK(w && wpsicon_sizes(w, sz, 4) == 1 && sz[0] == 128);
    wpsicon_free(w);
}

TEST(svg)
{
    /* a package of one SVG: drawn at any size asked, exactly (librsvg) */
    char svg[512], out[512];
    snprintf(svg, sizeof svg, "%s/square.svg", dir);
    FILE *f = fopen(svg, "w");
    fputs("<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' viewBox='0 0 10 10'>"
          "<rect width='10' height='10' fill='#ff0000'/></svg>", f);
    fclose(f);
    snprintf(out, sizeof out, "%s/svg.wpsicon", dir);
    const char *inputs[1] = { svg };
    char err[256];
    CHECK(wpsicon_pack(inputs, 1, out, err, sizeof err) == 0);
    wpsicon *w = wpsicon_open(out);
    CHECK(w && wpsicon_has_svg(w));
    int how = 0;
    cairo_surface_t *s = w ? wpsicon_render(w, 48, &how) : NULL;
    CHECK(s && how == WPSICON_SVG && cairo_image_surface_get_width(s) == 48);
    CHECK(s && px_at(s, 24, 24) == 0xffff0000u);   /* opaque red, drawn */
    if (s) cairo_surface_destroy(s);
    s = w ? wpsicon_render(w, 64, &how) : NULL;   /* a standard size made from it at packing: exact */
    CHECK(s && how == WPSICON_EXACT);
    if (s) cairo_surface_destroy(s);
    wpsicon_free(w);
}

int main(void)
{
    snprintf(dir, sizeof dir, "/tmp/wps-test-wpsicon-%d", (int)getpid());
    mkdir(dir, 0755);
    RUN(pack_adds_the_standard_sizes);
    RUN(a_folder_and_other_sizes);
    RUN(refused);
    RUN(svg);
    return CHECK_SUMMARY();
}
