/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#define _GNU_SOURCE
#include "attrs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define FORMAT_VERSION 1
#define MAX_ATTR (64 * 1024)

const char *osd_astatus_str(osd_astatus st)
{
    switch (st) {
    case OSD_A_OK: return "ok";
    case OSD_A_NOPERM: return "no permission";
    case OSD_A_NOTSUP: return "attributes not supported";
    case OSD_A_BUSY: return "file busy (lock timeout)";
    case OSD_A_TOOBIG: return "too big for an attribute";
    case OSD_A_NOFILE: return "no such file";
    case OSD_A_CORRUPT: return "stored value is corrupt";
    case OSD_A_ERROR: return "error";
    }
    return "?";
}

/* ------------------------------------------------------------ properties */

void osd_props_init(osd_props *ps) { memset(ps, 0, sizeof *ps); }

void osd_props_free(osd_props *ps)
{
    for (size_t i = 0; i < ps->n; i++) { free(ps->p[i].key); free(ps->p[i].val); }
    free(ps->p);
    osd_props_init(ps);
}

static osd_prop *find(const osd_props *ps, const char *key)
{
    for (size_t i = 0; i < ps->n; i++)
        if (strcmp(ps->p[i].key, key) == 0) return &ps->p[i];
    return NULL;
}

const osd_prop *osd_props_get(const osd_props *ps, const char *key) { return find(ps, key); }

int osd_props_set(osd_props *ps, const char *key, char type, const void *val, uint32_t len)
{
    if (strlen(key) > 255) return -1;
    unsigned char *v = malloc(len ? len : 1);
    if (!v) return -1;
    if (len) memcpy(v, val, len);
    osd_prop *p = find(ps, key);
    if (p) {
        free(p->val);
        p->type = type;
        p->len = len;
        p->val = v;
        return 0;
    }
    if (ps->n == ps->cap) {
        size_t ncap = ps->cap ? ps->cap * 2 : 8;
        osd_prop *np = realloc(ps->p, ncap * sizeof *np);
        if (!np) { free(v); return -1; }
        ps->p = np;
        ps->cap = ncap;
    }
    char *k = strdup(key);
    if (!k) { free(v); return -1; }
    ps->p[ps->n++] = (osd_prop){ k, type, len, v };
    return 0;
}

int osd_props_set_str(osd_props *ps, const char *key, const char *s)
{
    return osd_props_set(ps, key, 's', s, (uint32_t)strlen(s));
}

int osd_props_set_int(osd_props *ps, const char *key, int64_t v)
{
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)((uint64_t)v >> (8 * i));
    return osd_props_set(ps, key, 'i', b, 8);
}

int osd_props_set_bool(osd_props *ps, const char *key, bool v)
{
    unsigned char b = v ? 1 : 0;
    return osd_props_set(ps, key, 'b', &b, 1);
}

bool osd_props_remove(osd_props *ps, const char *key)
{
    osd_prop *p = find(ps, key);
    if (!p) return false;
    free(p->key);
    free(p->val);
    *p = ps->p[--ps->n];
    return true;
}

bool osd_props_str(const osd_props *ps, const char *key, char **out)
{
    const osd_prop *p = find(ps, key);
    if (!p || p->type != 's') return false;
    char *s = malloc(p->len + 1);
    if (!s) return false;
    memcpy(s, p->val, p->len);
    s[p->len] = 0;
    *out = s;
    return true;
}

bool osd_props_int(const osd_props *ps, const char *key, int64_t *out)
{
    const osd_prop *p = find(ps, key);
    if (!p || p->type != 'i' || p->len != 8) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p->val[i] << (8 * i);
    *out = (int64_t)v;
    return true;
}

/* ------------------------------------------------------------ encoding */

int osd_props_decode(const void *buf, size_t len, osd_props *out)
{
    const unsigned char *b = buf;
    osd_props_init(out);
    if (len == 0) return 0;              /* no attribute yet */
    if (len < 9 || b[0] != FORMAT_VERSION) return -1;
    for (int i = 0; i < 8; i++) out->counter |= (uint64_t)b[1 + i] << (8 * i);
    size_t o = 9;
    while (o < len) {
        if (o + 1 > len) goto bad;
        size_t kl = b[o++];
        if (kl == 0 || o + kl + 5 > len) goto bad;
        char key[256];
        memcpy(key, b + o, kl);
        key[kl] = 0;
        o += kl;
        char type = (char)b[o++];
        uint32_t vl = (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24;
        o += 4;
        if (vl > len - o) goto bad;
        if (osd_props_set(out, key, type, b + o, vl) < 0) goto bad;
        o += vl;
    }
    return 0;
bad:
    osd_props_free(out);
    return -1;
}

unsigned char *osd_props_encode(const osd_props *ps, size_t *len)
{
    size_t n = 9;
    for (size_t i = 0; i < ps->n; i++) n += 1 + strlen(ps->p[i].key) + 1 + 4 + ps->p[i].len;
    unsigned char *b = malloc(n), *w = b;
    if (!b) return NULL;
    *w++ = FORMAT_VERSION;
    for (int i = 0; i < 8; i++) *w++ = (unsigned char)(ps->counter >> (8 * i));
    for (size_t i = 0; i < ps->n; i++) {
        size_t kl = strlen(ps->p[i].key);
        *w++ = (unsigned char)kl;
        memcpy(w, ps->p[i].key, kl);
        w += kl;
        *w++ = (unsigned char)ps->p[i].type;
        uint32_t vl = ps->p[i].len;
        for (int k = 0; k < 4; k++) *w++ = (unsigned char)(vl >> (8 * k));
        memcpy(w, ps->p[i].val, vl);
        w += vl;
    }
    *len = n;
    return b;
}

/* ------------------------------------------------------------ file I/O */

static osd_astatus from_errno(int e)
{
    switch (e) {
    case EACCES: case EPERM: case EROFS: return OSD_A_NOPERM;
    case ENOTSUP: return OSD_A_NOTSUP;
    case ENOSPC: case E2BIG: case ERANGE: return OSD_A_TOOBIG;
    case ENOENT: case ESTALE: return OSD_A_NOFILE;
    default: return OSD_A_ERROR;
    }
}

static void attr_name(char *buf, size_t n, const char *cls)
{
    snprintf(buf, n, OSD_ATTR_PREFIX "%s", cls);
}

/* Read and decode the attribute from an open file. */
static osd_astatus read_fd(int fd, const char *name, osd_props *out)
{
    unsigned char *buf = malloc(MAX_ATTR);
    if (!buf) return OSD_A_ERROR;
    ssize_t n = fgetxattr(fd, name, buf, MAX_ATTR);
    osd_astatus st = OSD_A_OK;
    if (n < 0) {
        if (errno == ENODATA) osd_props_init(out);
        else st = from_errno(errno);
    } else if (osd_props_decode(buf, (size_t)n, out) < 0) {
        st = OSD_A_CORRUPT;
    }
    free(buf);
    return st;
}

static int open_target(const char *path)
{
    /* O_NOFOLLOW: never act on a symlink's target by accident; symlinks
     * cannot carry user attributes anyway. */
    return open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
}

osd_astatus osd_attr_read(const char *path, const char *cls, osd_props *out)
{
    char name[300];
    attr_name(name, sizeof name, cls);
    int fd = open_target(path);
    if (fd < 0) return errno == ELOOP ? OSD_A_NOTSUP : from_errno(errno);
    osd_astatus st = read_fd(fd, name, out);
    close(fd);
    return st;
}

static bool lock_with_timeout(int fd, int timeout_ms)
{
    struct timespec step = { 0, 10 * 1000 * 1000 };
    for (int waited = 0;; waited += 10) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) return true;
        if (errno != EWOULDBLOCK || waited >= timeout_ms) return false;
        nanosleep(&step, NULL);
    }
}

osd_astatus osd_attr_update(const char *path, const char *cls, const osd_props *set,
                            const char *const *removed, int lock_timeout_ms,
                            uint64_t *expect, osd_props *result)
{
    char name[300];
    attr_name(name, sizeof name, cls);
    int fd = open_target(path);
    if (fd < 0) return errno == ELOOP ? OSD_A_NOTSUP : from_errno(errno);
    if (!lock_with_timeout(fd, lock_timeout_ms)) { close(fd); return OSD_A_BUSY; }

    osd_props cur;
    osd_astatus st = read_fd(fd, name, &cur);
    if (st != OSD_A_OK) goto out;
    if (expect && *expect != cur.counter) {
        *expect = cur.counter;
        osd_props_free(&cur);
        st = OSD_A_ERROR;
        goto out;
    }
    for (size_t i = 0; set && i < set->n; i++)
        if (osd_props_set(&cur, set->p[i].key, set->p[i].type, set->p[i].val, set->p[i].len) < 0) {
            st = OSD_A_ERROR;
            osd_props_free(&cur);
            goto out;
        }
    for (size_t i = 0; removed && removed[i]; i++) osd_props_remove(&cur, removed[i]);
    cur.counter++;
    size_t len;
    unsigned char *enc = osd_props_encode(&cur, &len);
    if (!enc) st = OSD_A_ERROR;
    else if (fsetxattr(fd, name, enc, len, 0) < 0) st = from_errno(errno);
    free(enc);
    if (st == OSD_A_OK && result) *result = cur;
    else osd_props_free(&cur);
out:
    flock(fd, LOCK_UN);
    close(fd);
    return st;
}

/* ------------------------------------------------------------------ IDs */

void osd_new_id(char out[OSD_ID_LEN + 1])
{
    unsigned char b[16];
    if (getrandom(b, sizeof b, 0) != (ssize_t)sizeof b) memset(b, 0, sizeof b);
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    snprintf(out, OSD_ID_LEN + 1,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
             b[12], b[13], b[14], b[15]);
}

bool osd_attr_get_id(const char *path, char *id)
{
    ssize_t n = lgetxattr(path, OSD_ATTR_ID, id, OSD_ID_LEN);
    if (n != OSD_ID_LEN) return false;
    id[OSD_ID_LEN] = 0;
    return true;
}

osd_astatus osd_attr_assign_id(const char *path, const char *candidate, char *id)
{
    if (lsetxattr(path, OSD_ATTR_ID, candidate, OSD_ID_LEN, XATTR_CREATE) == 0) {
        memcpy(id, candidate, OSD_ID_LEN + 1);
        return OSD_A_OK;
    }
    if (errno == EEXIST) return osd_attr_get_id(path, id) ? OSD_A_OK : OSD_A_CORRUPT;
    return from_errno(errno);
}
