/*
 * Shadows (design: Links; decided 2026-11-18: WPS object files).
 *
 * A shadow is a WPS object file, as in OS/2, where a shadow was a WPS
 * object and not a file-system link:
 *
 *   <name>.wpobj      [WPS Object]
 *                     Class=OsdShadow
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
#ifndef OSD_SHADOWS_H
#define OSD_SHADOWS_H

#include <stdbool.h>

#include "db.h"
#include "objects.h"
#include "osdo_loop.h"

#define OSD_SHADOW_TMP ".wps-tmp-"   /* prefix of temporary names; never tracked */

void osd_shadows_init(osdo_loop *loop, osd_db *db);

/* Loop thread: a central object appeared in ('c') or left ('d') a folder
 * (the service refreshes listings and tells subscribers). */
extern void (*osd_on_central_change)(const char *folder, const char *id, char kind);

/* Create a shadow of `target` (a path or id:ID) in `folder`. `done` gets
 * the new link's path, or NULL and an error message. */
void osd_shadow_create(const char *target, const char *folder,
                       void (*done)(void *arg, const char *link, const char *error), void *arg);

/* Loop-thread hooks from the object layer and the tracker. */
void osd_shadows_target_moved(const char *id, const char *newpath);
void osd_shadows_prefix_moved(const char *oldpath, const char *newpath);
void osd_shadows_target_deleted(const char *id);
/* A .wpobj file was created, moved in, loaded or listed: index it if it
 * is a shadow (any thread). */
void osd_shadows_file_seen(const char *path);
/* A folder came into view: index the shadow files in it (any user's). */
void osd_shadows_folder_seen(const char *folder);
/* Read a shadow file (any thread): its target's ID and path hint. */
bool osd_shadow_read(const char *path, char id[OSD_ID_LEN + 1], char *target_path, size_t tn);

/* Trash folders: the per-user trash and per-volume .Trash-<uid>. */
bool osd_in_trash(const char *path);

#endif
