#define _GNU_SOURCE
#include "volumes.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Undo mountinfo's octal escapes (\040 for a space, ...). */
static void unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] && r[3]) {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 3;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

static void uuid_of(const char *source, unsigned maj, unsigned min, char *out, size_t n)
{
    char real[PATH_MAX];
    snprintf(out, n, "dev-%u-%u", maj, min);   /* fallback, not stable */
    if (!realpath(source, real)) return;
    DIR *d = opendir("/dev/disk/by-uuid");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char link[PATH_MAX + 32], target[PATH_MAX];
        snprintf(link, sizeof link, "/dev/disk/by-uuid/%s", de->d_name);
        if (realpath(link, target) && strcmp(target, real) == 0) {
            snprintf(out, n, "%s", de->d_name);
            break;
        }
    }
    closedir(d);
}

static bool sysfs_portable(unsigned maj, unsigned min)
{
    char p[128], real[PATH_MAX], f[PATH_MAX + 32], buf[8];
    snprintf(p, sizeof p, "/sys/dev/block/%u:%u", maj, min);
    if (!realpath(p, real)) return false;
    if (strstr(real, "/usb")) return true;
    /* A partition's "removable" flag lives on its parent disk. */
    for (int up = 0; up < 2; up++) {
        snprintf(f, sizeof f, "%s/removable", real);
        FILE *fp = fopen(f, "r");
        if (fp) {
            bool r = fgets(buf, sizeof buf, fp) && buf[0] == '1';
            fclose(fp);
            if (r) return true;
        }
        char *slash = strrchr(real, '/');
        if (!slash) break;
        *slash = 0;
    }
    return false;
}

static bool listed(const wps_env *env, const char *file, const char *uuid)
{
    char path[PATH_MAX + 32], line[128];
    snprintf(path, sizeof path, "%s/%s", env->config_dir, file);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool hit = false;
    while (!hit && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n \t")] = 0;
        hit = strcmp(line, uuid) == 0;
    }
    fclose(f);
    return hit;
}

int wps_volumes_scan(wps_volumes *out, const wps_env *env)
{
    out->v = NULL;
    out->n = 0;
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return -1;
    char line[8192];
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        /* id parent maj:min root mountpoint options [optional...] - fstype source superopts */
        char root[PATH_MAX], mnt[PATH_MAX], opts[1024], fstype[64], source[PATH_MAX];
        unsigned maj, min;
        if (sscanf(line, "%*u %*u %u:%u %4095s %4095s %1023s", &maj, &min, root, mnt, opts) != 5) continue;
        char *dash = strstr(line, " - ");
        if (!dash || sscanf(dash + 3, "%63s %4095s", fstype, source) != 2) continue;
        if (strncmp(source, "/dev/", 5) != 0) continue;   /* block devices only */
        if (strcmp(root, "/") != 0) continue;             /* skip bind mounts of subdirectories */
        unescape(mnt);
        unescape(source);
        if (out->n == cap) {
            cap = cap ? cap * 2 : 16;
            wps_volume *nv = realloc(out->v, cap * sizeof *nv);
            if (!nv) break;
            out->v = nv;
        }
        wps_volume *v = &out->v[out->n++];
        memset(v, 0, sizeof *v);
        snprintf(v->mnt, sizeof v->mnt, "%s", mnt);
        snprintf(v->fstype, sizeof v->fstype, "%s", fstype);
        snprintf(v->source, sizeof v->source, "%s", source);
        v->major = maj;
        v->minor = min;
        char *o = opts;
        v->ro = strncmp(o, "ro,", 3) == 0 || strcmp(o, "ro") == 0;
        uuid_of(source, maj, min, v->uuid, sizeof v->uuid);
        v->portable = (sysfs_portable(maj, min) || listed(env, "portable", v->uuid)) &&
                      !listed(env, "not-portable", v->uuid);
    }
    fclose(f);
    /* Never the volume that holds our own state. */
    const wps_volume *home = wps_volumes_of_path(out, env->state_dir);
    if (home) ((wps_volume *)home)->portable = false;
    return 0;
}

void wps_volumes_free(wps_volumes *vs)
{
    free(vs->v);
    vs->v = NULL;
    vs->n = 0;
}

const wps_volume *wps_volumes_of_path(const wps_volumes *vs, const char *path)
{
    const wps_volume *best = NULL;
    size_t bestlen = 0;
    for (size_t i = 0; i < vs->n; i++) {
        size_t l = strlen(vs->v[i].mnt);
        bool match = strcmp(vs->v[i].mnt, "/") == 0 ||
                     (strncmp(path, vs->v[i].mnt, l) == 0 && (path[l] == '/' || path[l] == 0));
        if (strcmp(vs->v[i].mnt, "/") == 0) l = 1;
        if (match && l >= bestlen) { best = &vs->v[i]; bestlen = l; }
    }
    return best;
}

const wps_volume *wps_volumes_by_uuid(const wps_volumes *vs, const char *uuid)
{
    for (size_t i = 0; i < vs->n; i++)
        if (strcmp(vs->v[i].uuid, uuid) == 0) return &vs->v[i];
    return NULL;
}

const char *wps_volume_relpath(const wps_volume *v, const char *path)
{
    if (strcmp(v->mnt, "/") == 0) return path[0] == '/' ? path + 1 : path;
    size_t l = strlen(v->mnt);
    if (strncmp(path, v->mnt, l) != 0) return path;
    path += l;
    while (*path == '/') path++;
    return path;
}
