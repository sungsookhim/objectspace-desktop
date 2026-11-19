/*
 * Shadows (design: Links; decided 2026-11-18: WPS object files).
 *
 * A shadow is a WPS object file, as in OS/2, where a shadow was a WPS
 * object and not a file-system link:
 *
 *   <name>.wpobj      [WPS Object]
 *                     Class=WPShadow
 *                     Target=<the target's object ID>
 *                     TargetPath=<where the target was last seen>
 *
 * The ID makes it a shadow wherever the file is moved or copied, by WPS or
 * by any other program; the path is a hint. Each service keeps an index
 * (the `shadows` table) of the shadow files it has seen: created, loaded,
 * listed, or appearing in a watched folder. When a target moves, the hint
 * in every indexed shadow is updated in place; when a target is deleted
 * (not moved to the trash), its shadows are removed.
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
/* A .wpobj file was created, moved in, loaded or listed: index it if it
 * is a shadow (any thread). */
void wps_shadows_file_seen(const char *path);
/* A folder came into view: index the shadow files in it (any user's). */
void wps_shadows_folder_seen(const char *folder);
/* Read a shadow file (any thread): its target's ID and path hint. */
bool wps_shadow_read(const char *path, char id[WPS_ID_LEN + 1], char *target_path, size_t tn);

/* Trash folders: the per-user trash and per-volume .Trash-<uid>. */
bool wps_in_trash(const char *path);

#endif
