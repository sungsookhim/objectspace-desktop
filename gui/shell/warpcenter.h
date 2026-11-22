/*
 * The WarpCenter (v0.2-design 8; OS/2 Warp 4's bar): a WPS window
 * (org.wps.WarpCenter) of the desktop process, in the WPS session only.
 * The compositor puts it along the first screen's bottom (or top) edge,
 * full width, above other windows, and takes its height from the usable
 * screen area.
 *
 * From the left: the Desktop button (a menu of the desktop's objects,
 * folders as cascades, with Lockup, Shut down and the Window list at its
 * end), the Window list button; on the right the meters (CPU, memory,
 * disk, network), the Messages envelope (the unread count; click: the
 * Messages list), the keyboard (the input source; click: the next one,
 * as Super+Space) and the clock (click: a month's calendar).
 */
#ifndef WPS_WARPCENTER_H
#define WPS_WARPCENTER_H

#include <stdbool.h>

#include "ui.h"

/* desktop: the desktop folder (its menu); open_folder: a folder window. */
void warpcenter_open(ui_app *a, const char *desktop, bool top, void (*open_folder)(const char *path));
bool warpcenter_is_open(void);
/* Draw it again (an icon became ready). */
void warpcenter_damage(void);
void warpcenter_set_font(ui_font f);   /* the Font Palette's */
void warpcenter_on_font(void (*fn)(const char *spec));   /* a font dropped on it */

/* The WarpCenter object's settings (edge, meters, keyboard, messages,
 * clock, seconds, date), applied at once. */
void warpcenter_setting(const char *key, const char *val);
bool warpcenter_top(void);
/* The unread messages' count, and what a click on the envelope opens. */
void warpcenter_messages(size_t unread, void (*open_messages)(void));

/* The compositor's "I" line: the input source now. */
void warpcenter_input_source(int index, int count, bool ibus, const char *name);
/* "EN", "한", ... for an input source (exposed for tests). */
void warpcenter_source_label(bool ibus, const char *name, char *out, size_t n);

/* Self-tests: a click on a part ("desktop", "windows", "keyboard",
 * "clock"); the menu's labels at the top level (after it was read);
 * the keyboard part's label. */
bool warpcenter_press(const char *part);
size_t warpcenter_menu_labels(char *out, size_t n);
const char *warpcenter_keyboard_label(void);
const char *warpcenter_clock_text(void);
/* A part's place on the bar; false when it is not shown. */
bool warpcenter_part_rect(const char *part, double *x, double *y, double *w, double *h);

#endif
