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
#ifndef WPS_VOLUMES_H
#define WPS_VOLUMES_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "env.h"

typedef struct {
    char uuid[WPS_VOLID_MAX];
    char mnt[PATH_MAX];
    char fstype[32];
    char source[PATH_MAX];
    unsigned major, minor;
    bool ro;
    bool portable;
} wps_volume;

typedef struct {
    wps_volume *v;
    size_t n;
} wps_volumes;

/* Read /proc/self/mountinfo (only block-device filesystems). */
int wps_volumes_scan(wps_volumes *out, const wps_env *env);
void wps_volumes_free(wps_volumes *vs);

/* The volume a path lives on (the longest matching mount point). */
const wps_volume *wps_volumes_of_path(const wps_volumes *vs, const char *path);
const wps_volume *wps_volumes_by_uuid(const wps_volumes *vs, const char *uuid);

/* Path relative to the volume's mount point ("" for the root). */
const char *wps_volume_relpath(const wps_volume *v, const char *path);

#endif
