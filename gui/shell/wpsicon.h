/*
 * Icon packages (roadmap 0.4, the owner's spec): NAME.wpsicon, a zip of
 * PNGs named by their size ("256.png", square, any size) and optionally
 * "icon.svg"; no manifest. The standard sizes are 256 (the master), 128,
 * 96, 64 and 32: WPS's own packages hold all five, and packing makes any
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
#ifndef WPS_WPSICON_H
#define WPS_WPSICON_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct wpsicon wpsicon;

enum { WPSICON_EXACT = 1, WPSICON_SVG, WPSICON_MULTIPLE, WPSICON_LARGER, WPSICON_SMALLER };

extern const int wpsicon_standard[5];   /* 256, 128, 96, 64, 32 */

/* A package (.wpsicon) or a plain PNG; NULL if neither. */
wpsicon *wpsicon_open(const char *path);
void wpsicon_free(wpsicon *w);
/* Its PNG sizes, smallest first; how many. */
size_t wpsicon_sizes(const wpsicon *w, int *out, size_t max);
bool wpsicon_has_svg(const wpsicon *w);
/* The icon at `px` (ARGB32, px x px), by the rules above; *how (if
 * given): which rule. NULL if it has no pictures. */
cairo_surface_t *wpsicon_render(wpsicon *w, int px, int *how);

/* Pack `inputs` (PNG files, an icon.svg, or folders holding them: any
 * name, the size read from the picture) into `out`, adding the standard
 * sizes missing (from the largest picture, by the rules above). 0, or -1
 * with the reason in err. */
int wpsicon_pack(const char *const *inputs, size_t n, const char *out, char *err, size_t errn);

#endif
