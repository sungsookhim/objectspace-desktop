/*
 * Settings shared with GNOME (gui-design.md: Settings shared with GNOME).
 * Where WPS and GNOME's Settings overlap, a change in either shows in both:
 *
 *   Scheme Palette scheme   <-> org.gnome.desktop.interface color-scheme
 *                               (dark: prefer-dark; classic: default) and
 *                               org.gnome.desktop.a11y.interface high-contrast
 *   Mouse left_handed       <-> org.gnome.desktop.peripherals.mouse left-handed
 *   Mouse double_click_ms   <-> org.gnome.desktop.peripherals.mouse double-click
 *   Sound enabled           <-> org.gnome.desktop.sound event-sounds
 *   (font: GNOME's interface font and text scaling, followed; WPS has no
 *   font setting of its own yet)
 *
 * Every wps-shell follows GNOME's font. One process, the desktop, keeps
 * the rest in step both ways (following `gsettings monitor`): a value is
 * written only where it differs, so a change never echoes back. When the
 * desktop starts, GNOME's values win (other programs follow them already).
 * WPS_NO_GSETTINGS turns all of it off (tests).
 */
#ifndef WPS_GNOME_H
#define WPS_GNOME_H

#include <stdbool.h>

#include "ui.h"

/* set: write one WPS setting (class, key, value); font: the interface
 * font changed (lay out again). */
void gnome_start(ui_app *a, bool both_ways, void (*set)(const char *cls, const char *key, const char *value),
                 void (*font)(void));
void gnome_stop(void);

/* The settings objects' values, as last read (call for every key, then
 * gnome_wps_read once). */
void gnome_wps_value(const char *cls, const char *key, const char *value);
void gnome_wps_read(void);

/* `gsettings get SCHEMA KEY`, unquoted ("uint32 " dropped), into out; false
 * if there is none. The one reader of GNOME's settings in wps-shell (the
 * wallpaper's, the sound theme's, these). */
bool gnome_get(const char *schema, const char *key, char *out, size_t n);

/* For tests: GNOME's value for a key we follow ("color-scheme", ...). */
const char *gnome_value(const char *key);

#endif
