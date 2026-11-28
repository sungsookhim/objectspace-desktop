/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* osdd: properties Linux keeps (object-properties-design.md 2.6). A
 * property declared with `backing` is read from and written to its place,
 * not the object's state - so Objectspace and every other program see the same:
 *   mimeapps          the program a file's type opens with: mimeapps.list's
 *                     default (shared with GNOME); by the program's name
 *   desktop:MimeType  the types a program's desktop entry opens (its
 *                     Association page; the user's copy of the entry)
 * NULL from a get: not kept for this object (the property is left out). */
#define _GNU_SOURCE
#include "osdd.h"
#include "osd_props.h"

static const char *file_of(osdo_object *o)
{
    return osdo_is_a(o, OsdFileSystemClass) ? OsdFileSystem_data(o)->path : NULL;
}

/* a program from a desktop entry (not Objectspace's own): its file */
static const char *desktop_entry_of(osdo_object *o)
{
    sobj *so = osd_sobj_of(o);
    size_t sl = so ? strlen(so->path) : 0;
    return osdo_is_a(o, OsdProgramClass) && so && !so->central && sl > 8 && !strcmp(so->path + sl - 8, ".desktop") ? so->path : NULL;
}

static char *type_of(osdo_object *o)
{
    if (!osdo_is_a(o, OsdDataFileClass)) return NULL;
    osdo_value r = osdo_v_void();
    char *m = NULL;
    if (osdo_send(o, "osdQueryMimeType", NULL, 0, &r) == OSDO_OK && r.type == 's' && r.u.s) m = strdup(r.u.s);
    osdo_value_clear(&r);
    return m;
}

/* ---- mimeapps: the programs for a type, by name ("," in a name: " ") */

static void clean_name(char *n) { for (; *n; n++) if (*n == ',' || *n == '\t' || *n == '\n') *n = ' '; }

static int by_name(const void *x, const void *y) { return strcoll(((const osd_app *)x)->name, ((const osd_app *)y)->name); }

/* (by name, not with the default first: a choice's options keep their
 * places when the default changes) */
char *mimeapps_options(const char *mime)
{
    osd_app *a = calloc(64, sizeof *a);
    int n = a ? osd_apps_for_mime(mime, a, 64) : 0;
    for (int i = 0; i < n; i++) clean_name(a[i].name);
    if (n > 1) qsort(a, (size_t)n, sizeof *a, by_name);
    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        if (a[i].hidden) continue;
        clean_name(a[i].name);
        bool dup = false;   /* (two entries of one name: the first) */
        for (int k = 0; k < i && !dup; k++) dup = !a[k].hidden && !strcmp(a[k].name, a[i].name);
        if (dup || len + strlen(a[i].name) + 2 >= cap) continue;
        len += (size_t)snprintf(out + len, cap - len, "%s%s", len ? "," : "", a[i].name);
    }
    free(a);
    return out;
}

char *mimeapps_get(const char *mime)
{
    osd_app *a = calloc(64, sizeof *a);
    int n = a ? osd_apps_for_mime(mime, a, 64) : 0;
    char *v = NULL;
    for (int i = 0; i < n && !v; i++)
        if (a[i].is_default) { clean_name(a[i].name); v = strdup(a[i].name); }
    free(a);
    return v ? v : strdup("");   /* (none set: "") */
}

bool mimeapps_set(const char *mime, const char *name)
{
    osd_app *a = calloc(64, sizeof *a);
    int n = a ? osd_apps_for_mime(mime, a, 64) : 0;
    bool ok = false;
    for (int i = 0; i < n && !ok; i++) {
        clean_name(a[i].name);
        if (!strcmp(a[i].name, name)) ok = osd_app_set_default_mime(mime, a[i].id);
    }
    free(a);
    return ok;
}

/* ---- the hooks (osd_props.h) */

static char *backing_get(osdo_object *o, const osdo_property_def *p)
{
    if (!strcmp(p->backing, "mimeapps")) {
        char *m = file_of(o) ? type_of(o) : NULL;
        char *v = m ? mimeapps_get(m) : NULL;
        free(m);
        return v;
    }
    if (!strcmp(p->backing, "desktop:MimeType")) {
        const char *d = desktop_entry_of(o);
        char types[1024] = "";
        if (!d) return NULL;
        osd_app_types(d, types, sizeof types);
        for (char *q = types; *q; q++) if (*q == '\t') *q = ' ';
        return strdup(types);
    }
    return NULL;
}

static bool backing_set(osdo_object *o, const osdo_property_def *p, const char *v)
{
    if (!strcmp(p->backing, "mimeapps")) {
        char *m = file_of(o) ? type_of(o) : NULL;
        bool ok = m && mimeapps_set(m, v);
        free(m);
        return ok;
    }
    if (!strcmp(p->backing, "desktop:MimeType")) {
        const char *d = desktop_entry_of(o);
        return d && osd_app_set_types(d, v);
    }
    return false;
}

static char *backing_options(osdo_object *o, const osdo_property_def *p)
{
    if (strcmp(p->backing, "mimeapps")) return NULL;
    char *m = file_of(o) ? type_of(o) : NULL;
    char *v = m ? mimeapps_options(m) : NULL;
    free(m);
    return v;
}

/* ---- a file's type kept in the database (0.8), where its file system
 * takes no attributes (a FAT drive): by its volume and its path there */

static bool mime_key(const char *path, char *vol, size_t vn, char *key, size_t kn)
{
    osd_volume v;
    if (!osd_volume_for_path(path, &v)) return false;
    snprintf(vol, vn, "%s", v.uuid);
    char loc[PATH_MAX + 16];
    snprintf(loc, sizeof loc, "mime:%s", osd_volume_relpath(&v, path));
    osd_db_key(key, kn, loc, "OsdDataFile", "mime_type");
    return true;
}

struct mime_find { char *out; size_t n; bool found; };

static void mime_prop(const char *cls, const char *prop, char type, const void *val, uint32_t len, void *user)
{
    struct mime_find *m = user;
    if (strcmp(cls, "OsdDataFile") || strcmp(prop, "mime_type") || type != 's') return;
    snprintf(m->out, m->n, "%.*s", (int)len, (const char *)val);
    m->found = true;
}

static const char *mime_overlay_get(const char *path, char *out, size_t n)
{
    char vol[OSD_VOLID_MAX], key[PATH_MAX + 128];
    if (!mime_key(path, vol, sizeof vol, key, sizeof key)) return NULL;
    char *sep = strchr(key, '\x1f');   /* (the locator: the key up to its first separator) */
    if (sep) *sep = 0;
    struct mime_find m = { out, n, false };
    osd_db_get_locator(db, vol, key, mime_prop, &m);
    return m.found ? out : NULL;
}

static bool mime_overlay_set(const char *path, const char *mime)
{
    char vol[OSD_VOLID_MAX], key[PATH_MAX + 128];
    if (!mime_key(path, vol, sizeof vol, key, sizeof key)) return false;
    if (!mime) return osd_db_delete(db, vol, key) == 0 || true;
    return osd_db_put(db, vol, key, 's', mime, (uint32_t)strlen(mime)) == 0;
}

void backing_init(void)
{
    osd_mime_overlay_get = mime_overlay_get;
    osd_mime_overlay_set = mime_overlay_set;
    osd_backing_get = backing_get;
    osd_backing_set = backing_set;
    osd_backing_options = backing_options;
}
