/*
 * The Colour Palette (OS/2's, v0.4-design 9): a window of colour samples.
 * Dragged (UI_MIME_COLOR, "#rrggbb") onto a folder window's or the
 * desktop's background, a sample sets its background colour (Ctrl: its
 * icons' titles; Shift: every folder's); onto a title bar, the scheme's
 * active title bar; onto TheBar or the Launcher, the scheme's
 * bar colour; onto a notebook's colour field, that field. A sample's menu
 * (or a double click): Edit colour... (hue and saturation, brightness,
 * #rrggbb).
 */
#ifndef OSD_COLORPAL_H
#define OSD_COLORPAL_H

#include <stdbool.h>
#include <stddef.h>

#include "ui.h"

/* what: "samples" (the palette's list, ";"). */
typedef void (*colorpal_apply_fn)(const char *what, const char *value);

void colorpal_init(ui_app *a, colorpal_apply_fn apply);
void colorpal_open(void);
/* The Colour Palette object's samples ("#ffffff;#c0c0c0;..."). */
void colorpal_set_samples(const char *list);

/* Self-tests. */
bool colorpal_is_open(void);
size_t colorpal_count(void);
const char *colorpal_sample(size_t i);
bool colorpal_edit(size_t i);                  /* Edit colour... on sample i */
bool colorpal_edit_is_open(void);
bool colorpal_edit_pick(double x, double y);   /* a click in the hue and saturation square (0..1 across, down) */
bool colorpal_edit_bright(double v);           /* the brightness (0..1) */
const char *colorpal_edit_hex(void);           /* what it shows now */
bool colorpal_edit_type(const char *hex);      /* typed into its #rrggbb field */
bool colorpal_edit_ok(void);
bool colorpal_snapshot(const char *png);
bool colorpal_edit_snapshot(const char *png);

#endif
