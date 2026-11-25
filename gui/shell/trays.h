/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * TheBar's trays (0.6 step 2; OS/2 Warp 4's WarpCenter trays, v0.6-design 1):
 * named sets of objects shown on the bar, one at a time.
 *
 * A tray is a folder of shadows in the Trays folder
 * ($XDG_DATA_HOME/objectspace/Trays; OSD_TRAYS_DIR for tests), so it is
 * an object like any other (the first start makes Default and shows the
 * Trays folder in System). The objects keep the order of their stored
 * positions in the tray's folder (left to right), as the Launcher's
 * buttons do. The current tray is TheBar object's setting (tray_current).
 *
 * This file keeps the trays and acts on them; TheBar draws them and
 * passes on clicks and drops.
 */
#ifndef OSD_TRAYS_H
#define OSD_TRAYS_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui.h"

struct tray_obj {
    char name[NAME_MAX + 16];  /* as listed ("\x01vobj:<id>" for a central object) */
    char title[256];
    char icon[PATH_MAX];       /* its own icon, or "" */
    char target[PATH_MAX];     /* what a shadow stands for */
    char id[48];
    ui_kind kind;
    bool shadow, link, folder_target;
};

/* changed: the trays or the current one's objects changed (draw again). */
void trays_start(ui_app *a, void (*changed)(void), void (*open_folder)(const char *path), void (*open_props)(const char *path));
const char *trays_dir(void);

size_t trays_count(void);                 /* the trays */
const char *trays_name(size_t i);         /* by name, any case */
const char *trays_current(void);          /* "" while none is read */
size_t trays_n(void);                     /* the current tray's objects */
const struct tray_obj *trays_obj(size_t i);

/* TheBar object's setting: the tray to show (kept when it is not there yet). */
void trays_want(const char *name);
void trays_next(void);                    /* the tray button: the next one (and kept) */
void trays_choose(const char *name);      /* from the tray menu (and kept) */

void trays_open(size_t i);                /* a click on an object */
/* Its menu: the object's own, with Remove from tray. */
void trays_obj_menu(ui_surface *s, size_t i, double x, double y);
/* The tray button's menu: the trays, Add tray, Rename tray..., Delete tray. */
void trays_menu(ui_surface *s, double x, double y, uint32_t serial);

/* Objects dropped (text/uri-list or Objectspace's list: paths and
 * vobj: addresses), at place `at` among the objects (trays_n(): the end):
 * shadows made in the current tray. */
void trays_drop(const char *data, size_t len, size_t at);

/* An event from osdd: true if it was the trays' (a folder changed). */
bool trays_event(const char *name, int64_t sub);

#endif
