/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * Settings shared with GNOME (gui-design.md: Settings shared with GNOME).
 * Where Objectspace and GNOME's Settings overlap, a change in either shows in both:
 *
 *   Scheme Palette scheme   <-> org.gnome.desktop.interface color-scheme
 *                               (a dark scheme, by its windows' background:
 *                               prefer-dark; a light one: default) and
 *                               org.gnome.desktop.a11y.interface high-contrast;
 *                               from GNOME, the scheme changes only when it
 *                               is not already as GNOME says (a dark scheme
 *                               of the user's stays when GNOME says dark)
 *                            -> org.gnome.desktop.interface accent-color: the
 *                               accent nearest the scheme's selection colour
 *                               (libadwaita programs' accent, 0.7)
 *   Mouse left_handed       <-> org.gnome.desktop.peripherals.mouse left-handed
 *   Mouse double_click_ms   <-> org.gnome.desktop.peripherals.mouse double-click
 *   Sound enabled           <-> org.gnome.desktop.sound event-sounds
 *   (font: GNOME's interface font and text scaling, followed; Objectspace has no
 *   font setting of its own yet)
 *
 * Every osd-shell follows GNOME's font. One process, the desktop, keeps
 * the rest in step both ways (following `gsettings monitor`): a value is
 * written only where it differs, so a change never echoes back. When the
 * desktop starts inside GNOME, GNOME's values win (other programs follow
 * them already); in the Objectspace session, Objectspace's (its scheme is
 * the desktop's look, and libadwaita programs follow it: 0.7 step 6).
 * libadwaita's fonts and text size are GNOME's interface font and text
 * scaling, which Objectspace follows: the same for both already.
 * OSD_NO_GSETTINGS turns all of it off (tests).
 */
#ifndef OSD_GNOME_H
#define OSD_GNOME_H

#include <stdbool.h>

#include "ui.h"

/* set: write one Objectspace setting (class, key, value); font: the interface
 * font changed (lay out again). */
void gnome_start(ui_app *a, bool both_ways, void (*set)(const char *cls, const char *key, const char *value),
                 void (*font)(void));
void gnome_stop(void);

/* The settings objects' values, as last read (call for every key, then
 * gnome_osd_read once). */
void gnome_osd_value(const char *cls, const char *key, const char *value);
void gnome_osd_read(void);

/* `gsettings get SCHEMA KEY`, unquoted ("uint32 " dropped), into out; false
 * if there is none. The one reader of GNOME's settings in osd-shell (the
 * wallpaper's, the sound theme's, these). */
bool gnome_get(const char *schema, const char *key, char *out, size_t n);

/* For tests: GNOME's value for a key we follow ("color-scheme", ...). */
const char *gnome_value(const char *key);

/* GNOME's accent colour ("blue", "teal", ... "slate") nearest a colour:
 * written as libadwaita's accent from the scheme's selection (0.7). */
const char *gnome_accent_of(ui_color c);

#endif
