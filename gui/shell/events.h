/*
 * Action events (gui-design.md: Every action is an event): one bus in
 * wps-shell. Every user action raises a named event with the object, its
 * class and where on screen it happened; the sound scheme (and later
 * animations and GUI plug-ins) receive them.
 *
 * Names: window.open, window.close, window.minimize, window.maximize,
 * window.restore, menu.open, menu.select, object.drag, object.drop,
 * object.drop.refused, object.delete, object.trash, object.create,
 * object.open, job.done, job.failed, alert.info, alert.warning,
 * alert.error, shell.startup, shell.shutdown.
 *
 * The sound scheme maps them to sounds: by name in the sound theme GNOME
 * uses (org.gnome.desktop.sound theme-name, then freedesktop), or a file.
 * GNOME's "event sounds" switch turns them all off. Until the Sound
 * object (G3), changes come from <config>/sounds.conf:
 *     volume = 70
 *     enabled = yes          (overrides GNOME's switch)
 *     window.open = bell     (a theme sound)
 *     job.done = /path/to/done.wav
 *     alert.info = none
 */
#ifndef WPS_EVENTS_H
#define WPS_EVENTS_H

#include <stdbool.h>

#include "ui.h"

typedef struct {
    const char *name;
    const char *object;   /* its path or address, or "" */
    const char *cls;      /* "folder", "file", "program", "window", or "" */
    ui_rect at;           /* on screen, in its surface */
} wps_event;

typedef void (*wps_event_fn)(const wps_event *e, void *ctx);

/* Start the bus and the sound scheme (reads the settings). */
void events_init(void);
void events_shutdown(void);

void events_raise(const char *name, const char *object, const char *cls, ui_rect at);
void events_subscribe(wps_event_fn fn, void *ctx);

/* The sound file an event plays now, or NULL (self-tests, settings). */
const char *events_sound_for(const char *name);

/* Read the scheme again (settings changed). */
void events_reload(void);

/* The Sound object's settings (G3), replacing sounds.conf: begin, then
 * each key (enabled, volume, or an event name) with its value (a theme
 * sound, a file, or "" for none). GNOME's event-sounds switch still
 * silences everything. */
/* Play a Sound object's value now (a theme sound's name or a file): its
 * Play button. False if there is no such sound. */
bool events_play(const char *value);
void events_setup_begin(void);
void events_setup_set(const char *key, const char *value);

#endif
