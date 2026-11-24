/*
 * The WPS session's notification service (v0.2-design 9): the desktop
 * process owns org.freedesktop.Notifications on the session bus (sd-bus;
 * only in the WPS session, or with OSD_NOTIFY=1: GNOME has its own).
 *
 * Notify shows a pop-up near the WarpCenter (a window io.github.sungsookhim.Objectspace.Notice the
 * compositor puts in the usable area's corner by the bar): icon, summary,
 * body (simple markup taken out) and the actions as buttons. It stays the
 * notification's timeout (the Messages object's "Pop-ups stay", 6 s by
 * default; critical ones until closed). Every notification is also kept
 * in Messages (osdd), unless it is "transient". Capabilities: body,
 * body-markup, actions, persistence, icon-static. Its actions stay usable
 * from the Messages list until the program closes the notification or
 * leaves the bus. "Do not disturb" keeps the pop-ups away; the messages
 * are still kept and counted.
 */
#ifndef OSD_NOTIFY_H
#define OSD_NOTIFY_H

#include <stdbool.h>
#include <stdint.h>

#include "ui.h"

/* False when not wanted here, or the name is taken (another server). */
bool notify_init(ui_app *a);
bool notify_running(void);

void notify_set_dnd(bool on);
void notify_set_seconds(int seconds);

/* A pop-up of WPS's own (an error the desktop could not show elsewhere);
 * nothing outside the WPS session. */
void notify_local(const char *summary, const char *body, int urgency);

/* The program behind notification nid (from sender) can still take its
 * actions; invoke one (as a click on its button). */
bool notify_can_invoke(int64_t nid, const char *sender);
bool notify_invoke(int64_t nid, const char *sender, const char *key);

/* Self-tests: the pop-ups shown, a click on pop-up i's action k (-1: its
 * body, -2: its close box), its snapshot. */
size_t notify_shown(void);
const char *notify_shown_summary(size_t i);
bool notify_click(size_t i, int action);
bool notify_snapshot(const char *png);

/* Plain text from simple markup (<b>, <i>, <u>, <a>, <img>; &amp; ...). */
void notify_strip_markup(const char *in, char *out, size_t n);

#endif
