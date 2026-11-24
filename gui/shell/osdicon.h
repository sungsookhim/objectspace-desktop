/*
 * Icon packages (roadmap 0.4, the owner's spec): NAME.osdicon, a zip of
 * PNGs named by their size ("256.png", square, any size) and optionally
 * "icon.svg"; no manifest. The standard sizes are 256 (the master), 128,
 * 96, 64 and 32: Objectspace's own packages hold all five, and packing makes any
 * of them missing from the largest picture given. A plain PNG is a
 * package of one.
 *
 * A size is chosen, in device pixels, best first:
 *   1. the exact size;
 *   2. the SVG (librsvg: not yet; skipped);
 *   3. a whole multiple of it (the smallest), averaged down n x n;
 *   4. the nearest larger, scaled down smoothly;
 *   5. the nearest smaller, scaled up: by a whole factor nearest-neighbour
 *      (pixel art stays sharp), else smoothly.
 * Never a fractional nearest-neighbour scale.
 */
#ifndef OSD_OSDICON_H
#define OSD_OSDICON_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct osdicon osdicon;

enum { OSDICON_EXACT = 1, OSDICON_SVG, OSDICON_MULTIPLE, OSDICON_LARGER, OSDICON_SMALLER };

extern const int osdicon_standard[5];   /* 256, 128, 96, 64, 32 */

/* A package (.osdicon) or a plain PNG; NULL if neither. */
osdicon *osdicon_open(const char *path);
void osdicon_free(osdicon *w);
/* Its PNG sizes, smallest first; how many. */
size_t osdicon_sizes(const osdicon *w, int *out, size_t max);
bool osdicon_has_svg(const osdicon *w);
/* The icon at `px` (ARGB32, px x px), by the rules above; *how (if
 * given): which rule. NULL if it has no pictures. */
cairo_surface_t *osdicon_render(osdicon *w, int px, int *how);

/* Pack `inputs` (PNG files, an icon.svg, or folders holding them: any
 * name, the size read from the picture) into `out`, adding the standard
 * sizes missing (from the largest picture, by the rules above). 0, or -1
 * with the reason in err. */
int osdicon_pack(const char *const *inputs, size_t n, const char *out, char *err, size_t errn);

#endif
