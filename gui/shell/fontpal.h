/*
 * The Font Palette (OS/2's, v0.3-design 10): a window of font samples
 * ("12.Helvetica"), each drawn in its font. Dragged onto a folder window
 * or the desktop, a sample sets that folder's icon font (with Shift:
 * every folder's); onto the WarpCenter, its font; onto a title bar, all
 * title bars'. A sample's menu: Edit font... (family, size, bold, italic),
 * and Use for menus, title bars, every folder, the WarpCenter.
 */
#ifndef OSD_FONTPAL_H
#define OSD_FONTPAL_H

#include <stdbool.h>
#include <stddef.h>

#include "ui.h"

#define MIME_FONT "application/x-wps-font"

/* what: "menus", "titles", "folders", "warpcenter" (a font for them; ""
 * the interface font again), or "samples" (the palette's list, ";"). */
typedef void (*fontpal_apply_fn)(const char *what, const char *spec);

void fontpal_init(ui_app *a, fontpal_apply_fn apply);
void fontpal_open(void);
/* The Font Palette object's samples ("8.Sans;10.Sans Bold;..."). */
void fontpal_set_samples(const char *list);

/* Self-tests. */
bool fontpal_is_open(void);
size_t fontpal_count(void);
const char *fontpal_sample(size_t i);
bool fontpal_edit(size_t i);                       /* Edit font... on sample i */
bool fontpal_edit_choose(const char *family, int size, const char *style);   /* its OK */
bool fontpal_edit_family(const char *family);    /* a family chosen in its list */
size_t fontpal_edit_styles(char (*out)[64], size_t max);   /* the style list now */
bool fontpal_edit_is_open(void);
size_t fontpal_families(void);
bool fontpal_snapshot(const char *png);

#endif
