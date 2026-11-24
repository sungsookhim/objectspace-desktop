/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * Mounted volumes: identity (filesystem UUID), mount point, and whether the
 * volume is portable, i.e. carries per-user state with it (design: State
 * that travels with a volume).
 *
 * A volume is portable if its block device is removable or on USB, or if
 * <config>/portable lists its UUID (one per line). A volume listed in
 * <config>/not-portable never is, and neither is the volume holding the
 * service's state directory.
 */
#ifndef OSD_VOLUMES_H
#define OSD_VOLUMES_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "env.h"

typedef struct {
    char uuid[OSD_VOLID_MAX];
    char mnt[PATH_MAX];
    char fstype[32];
    char source[PATH_MAX];
    unsigned major, minor;
    bool ro;
    bool portable;
} osd_volume;

typedef struct {
    osd_volume *v;
    size_t n;
} osd_volumes;

/* Read /proc/self/mountinfo (only block-device filesystems). */
int osd_volumes_scan(osd_volumes *out, const osd_env *env);
void osd_volumes_free(osd_volumes *vs);

/* The volume a path lives on (the longest matching mount point). */
const osd_volume *osd_volumes_of_path(const osd_volumes *vs, const char *path);
const osd_volume *osd_volumes_by_uuid(const osd_volumes *vs, const char *uuid);

/* Path relative to the volume's mount point ("" for the root). */
const char *osd_volume_relpath(const osd_volume *v, const char *path);

#endif
