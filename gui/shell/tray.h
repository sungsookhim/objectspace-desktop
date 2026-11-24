/*
 * TheBar's tray (v0.2-design 10, v0.2-plan step 8): other
 * programs' status icons. The desktop process is the session's
 * StatusNotifierWatcher (org.kde.StatusNotifierWatcher on the session bus)
 * and its only host: programs register their StatusNotifierItems with it;
 * the tray reads each item's properties (IconName with IconThemePath, or
 * IconPixmap; Status, Title, ToolTip, Menu, ItemIsMenu) and follows its
 * New* signals, and forgets it when its program leaves the bus.
 *
 * Clicks: button 1 Activate (the menu for an ItemIsMenu item, or one that
 * has no Activate); button 2 the item's menu, read over dbusmenu
 * (com.canonical.dbusmenu GetLayout) and shown as an Objectspace menu, a choice
 * sent back as an Event "clicked" (no menu: ContextMenu); the middle
 * button SecondaryActivate. Passive items are not shown.
 *
 * Only when TheBar is open; not when another watcher has the name
 * (GNOME's AppIndicator extension).
 */
#ifndef OSD_TRAY_H
#define OSD_TRAY_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui.h"

/* changed: the items or their icons changed (the bar is drawn again). */
bool tray_init(ui_app *a, void (*changed)(void));
bool tray_running(void);

/* The items shown (not Passive), left to right. */
size_t tray_count(void);
cairo_surface_t *tray_icon(size_t i);   /* NULL: none (yet) */
const char *tray_title(size_t i);       /* its tooltip's title, else Title, else Id */

/* A press on item i (button UI_BTN1..3); a menu pops up from `on` at `at`. */
void tray_press(size_t i, int button, ui_surface *on, ui_rect at, uint32_t serial);

#endif
