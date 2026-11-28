/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "OsdDataFile.h"
#include "osd_magic.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/xattr.h>

#include "osd_classes.h"
#include "osd_startup_hook.h"

char *OsdDataFile__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    /* (Open With: osdd makes Open a cascade of the type's programs) */
    return OsdDataFile_parent_osdModifyPopupMenu(self, menu);
}

/* The type from the file name, through shared-mime-info's glob table
 * (loaded once, by osd_mime_init, before the loop starts). */
static struct { char *glob; char *type; int weight; bool cs; } *globs;
static size_t nglobs;

void osd_mime_init(const char *globs2)
{
    FILE *f = fopen(globs2 ? globs2 : "/usr/share/mime/globs2", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\r\n")] = 0;
        char *w = strtok(line, ":"), *type = strtok(NULL, ":"), *glob = strtok(NULL, ":"), *flags = strtok(NULL, ":");
        if (!w || !type || !glob || strncmp(glob, "*.", 2) != 0 || strpbrk(glob + 2, "*?[")) continue;
        globs = realloc(globs, (nglobs + 1) * sizeof *globs);
        globs[nglobs].glob = strdup(glob + 1);   /* ".png" */
        globs[nglobs].type = strdup(type);
        globs[nglobs].cs = flags && strstr(flags, "cs");   /* "*.C" is C++, "*.c" C */
        globs[nglobs++].weight = atoi(w);
    }
    if (f) fclose(f);
}

const char *osd_mime_for_name(const char *name)
{
    /* As the spec says: a match of the name's own case first (".c" is C,
     * ".C" C++), then one in any case; the longest suffix wins (".tar.gz"
     * over ".gz"), then weight. */
    size_t ln = strlen(name);
    for (int pass = 0; pass < 2; pass++) {
        const char *best = NULL;
        size_t bestlen = 0;
        int bestw = -1;
        for (size_t i = 0; i < nglobs; i++) {
            size_t lg = strlen(globs[i].glob);
            if (lg > ln) continue;
            if (pass == 0 ? strcmp(name + ln - lg, globs[i].glob) != 0
                          : globs[i].cs || strcasecmp(name + ln - lg, globs[i].glob) != 0) continue;
            if (lg > bestlen || (lg == bestlen && globs[i].weight > bestw)) {
                best = globs[i].type;
                bestlen = lg;
                bestw = globs[i].weight;
            }
        }
        if (best) return best;
    }
    return NULL;
}

/* ---- a type set on the file (0.8; OS/2's .TYPE): user.mime_type */

#define MIME_ATTR "user.mime_type"
const char *(*osd_mime_overlay_get)(const char *path, char *out, size_t n);
bool (*osd_mime_overlay_set)(const char *path, const char *mime);

bool osd_mime_valid(const char *s)
{
    if (!s || !*s || strlen(s) > 127) return false;
    const char *sl = strchr(s, '/');
    if (!sl || sl == s || !sl[1] || strchr(sl + 1, '/')) return false;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("/.+-_", *p)) return false;
    return true;
}

const char *osd_file_mime_set(const char *path, char *out, size_t n)
{
    if (!path || n < 2) return NULL;
    char v[160];
    ssize_t l = getxattr(path, MIME_ATTR, v, sizeof v - 1);
    if (l > 0) {
        v[l] = 0;
        v[strcspn(v, "\r\n")] = 0;   /* (some programs end it with a newline) */
        if (osd_mime_valid(v)) { snprintf(out, n, "%s", v); return out; }
        return NULL;
    }
    if (l < 0 && (errno == ENOTSUP || errno == EPERM) && osd_mime_overlay_get) {   /* (no attributes there: the database) */
        const char *o = osd_mime_overlay_get(path, out, n);
        return o && osd_mime_valid(o) ? o : NULL;
    }
    return NULL;
}

const char *osd_file_mime(const char *path, const char **source)
{
    static __thread char set[160];
    if (!path) return NULL;
    if (osd_file_mime_set(path, set, sizeof set)) { if (source) *source = "set"; return set; }
    const char *slash = strrchr(path, '/');
    const char *t = osd_mime_for_name(slash ? slash + 1 : path);
    if (t) { if (source) *source = "name"; return t; }
    t = osd_mime_sniff(path);   /* the name says nothing: its contents (an empty Data File is text) */
    if (source) *source = t ? "contents" : NULL;
    return t;
}

bool osd_file_mime_store(const char *path, const char *mime)
{
    if (!path) return false;
    bool off = !mime || !*mime;
    if (!off && !osd_mime_valid(mime)) return false;
    int r = off ? removexattr(path, MIME_ATTR) : setxattr(path, MIME_ATTR, mime, strlen(mime), 0);
    if (r == 0 || (off && errno == ENODATA)) {
        if (osd_mime_overlay_set) osd_mime_overlay_set(path, NULL);   /* (an old one in the database: gone) */
        return true;
    }
    if ((errno == ENOTSUP || errno == EPERM) && osd_mime_overlay_set) return osd_mime_overlay_set(path, off ? NULL : mime);
    return false;
}

char *OsdDataFile__osdQueryMimeType(osdo_object *self)
{
    const char *t = osd_file_mime(OsdFileSystem_data(self)->path, NULL);
    return osd_strdup(t ? t : "application/octet-stream");
}

/* ---- the Type page */

char *OsdDataFile__osdQueryTypeShown(osdo_object *self)
{
    const char *src = NULL;
    const char *t = osd_file_mime(OsdFileSystem_data(self)->path, &src);
    char out[300];
    snprintf(out, sizeof out, "%s (%s)", t ? t : "application/octet-stream",
             !src ? "nothing says otherwise" : !strcmp(src, "set") ? "set on it" : !strcmp(src, "name") ? "from its name" : "from its contents");
    return osd_strdup(out);
}

#define TYPE_AUTO "by its name and contents"

char *OsdDataFile__osdQueryTypeSet(osdo_object *self)
{
    char v[160];
    return osd_strdup(osd_file_mime_set(OsdFileSystem_data(self)->path, v, sizeof v) ? v : TYPE_AUTO);
}

bool OsdDataFile__osdSetTypeSet(osdo_object *self, const char *value)
{
    const char *p = OsdFileSystem_data(self)->path;
    return osd_file_mime_store(p, !value || !strcmp(value, TYPE_AUTO) ? NULL : value);
}

/* The choices: none set, the one set (if any), the name's and contents'
 * (so it can be set to what it is), then the common types. */
char *OsdDataFile__osdQueryTypeChoices(osdo_object *self)
{
    static const char *const common[] = {
        "text/plain", "text/markdown", "text/html", "text/csv", "text/x-csrc", "text/x-python", "application/x-shellscript",
        "application/json", "application/xml", "application/pdf", "application/vnd.oasis.opendocument.text",
        "application/vnd.oasis.opendocument.spreadsheet", "application/vnd.oasis.opendocument.presentation",
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "image/png", "image/jpeg", "image/gif",
        "image/svg+xml", "image/webp", "audio/mpeg", "audio/ogg", "audio/flac", "audio/x-wav", "video/mp4", "video/webm",
        "video/x-matroska", "application/zip", "application/x-tar", "application/gzip", "application/x-cd-image",
        "font/ttf", "application/octet-stream",
    };
    const char *p = OsdFileSystem_data(self)->path;
    char set[160] = "";
    osd_file_mime_set(p, set, sizeof set);
    const char *slash = p ? strrchr(p, '/') : NULL;
    const char *byname = p ? osd_mime_for_name(slash ? slash + 1 : p) : NULL, *bycont = p ? osd_mime_sniff(p) : NULL;
    const char *first[] = { set, byname, bycont };
    char out[4096];
    size_t len = (size_t)snprintf(out, sizeof out, "%s", TYPE_AUTO);
    for (size_t i = 0; i < 3 + sizeof common / sizeof *common; i++) {
        const char *t = i < 3 ? first[i] : common[i - 3];
        if (!t || !*t) continue;
        char needle[200];   /* (each once) */
        snprintf(needle, sizeof needle, ",%s,", t);
        char hay[4200];
        snprintf(hay, sizeof hay, ",%s,", out);
        if (strstr(hay, needle)) continue;
        if (len + strlen(t) + 2 < sizeof out) len += (size_t)snprintf(out + len, sizeof out - len, ",%s", t);
    }
    return osd_strdup(out);
}

char *OsdDataFile__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Data File");
}

/* ---- an executable's Program page (OsdDataFile.osdi) */

bool (*osd_is_program)(const char *path);

bool OsdDataFile__osdQueryIsProgram(osdo_object *self)
{
    const char *path = OsdFileSystem_data(self)->path;
    return path && osd_is_program && osd_is_program(path);
}
