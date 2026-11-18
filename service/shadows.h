/*
 * Shadows (design: Links, Shadows maintained by any user).
 *
 * A shadow of a file or folder is a managed symlink: a real symlink with an
 * absolute path to its target, plus a record on the folder that holds it
 * (the folder's WPFolder property "shadow:<name>" = target object ID), so
 * any user who can write that folder can maintain it. Each service also
 * keeps an index (the `shadows` table) of the shadows it knows of, learned
 * when it creates one or loads a folder carrying records.
 *
 * When a target moves, every known shadow pointing at it is rewritten
 * atomically (a new symlink renamed over the old one). When a target is
 * deleted, its managed shadows are removed; a move to the trash is not a
 * deletion. Symlinks made by users are never rewritten or removed.
 */
#ifndef WPS_SHADOWS_H
#define WPS_SHADOWS_H

#include <stdbool.h>

#include "db.h"
#include "objects.h"
#include "wpo_loop.h"

#define WPS_SHADOW_TMP ".wps-tmp-"   /* prefix of temporary names; never tracked */

void wps_shadows_init(wpo_loop *loop, wps_db *db);

/* Create a shadow of `target` (a path or id:ID) in `folder`. `done` gets
 * the new link's path, or NULL and an error message. */
void wps_shadow_create(const char *target, const char *folder,
                       void (*done)(void *arg, const char *link, const char *error), void *arg);

/* Loop-thread hooks from the object layer and the tracker. */
void wps_shadows_target_moved(const char *id, const char *newpath);
void wps_shadows_prefix_moved(const char *oldpath, const char *newpath);
void wps_shadows_target_deleted(const char *id);
void wps_shadows_folder_loaded(const char *folder, const wps_loaded *ld);

/* Trash folders: the per-user trash and per-volume .Trash-<uid>. */
bool wps_in_trash(const char *path);

#endif
