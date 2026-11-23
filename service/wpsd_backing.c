/* wpsd: properties Linux keeps (object-properties-design.md 2.6). A
 * property declared with `backing` is read from and written to its place,
 * not the object's state - so WPS and every other program see the same:
 *   mimeapps          the program a file's type opens with: mimeapps.list's
 *                     default (shared with GNOME); by the program's name
 *   desktop:MimeType  the types a program's desktop entry opens (its
 *                     Association page; the user's copy of the entry)
 * NULL from a get: not kept for this object (the property is left out). */
#define _GNU_SOURCE
#include "wpsd.h"
#include "wps_props.h"

static const char *file_of(wpo_object *o)
{
    return wpo_is_a(o, WPFileSystemClass) ? WPFileSystem_data(o)->path : NULL;
}

/* a program from a desktop entry (not WPS's own): its file */
static const char *desktop_entry_of(wpo_object *o)
{
    sobj *so = wps_sobj_of(o);
    size_t sl = so ? strlen(so->path) : 0;
    return wpo_is_a(o, WPProgramClass) && so && !so->central && sl > 8 && !strcmp(so->path + sl - 8, ".desktop") ? so->path : NULL;
}

static char *type_of(wpo_object *o)
{
    if (!wpo_is_a(o, WPDataFileClass)) return NULL;
    wpo_value r = wpo_v_void();
    char *m = NULL;
    if (wpo_send(o, "wpQueryMimeType", NULL, 0, &r) == WPO_OK && r.type == 's' && r.u.s) m = strdup(r.u.s);
    wpo_value_clear(&r);
    return m;
}

/* ---- mimeapps: the programs for a type, by name ("," in a name: " ") */

static void clean_name(char *n) { for (; *n; n++) if (*n == ',' || *n == '\t' || *n == '\n') *n = ' '; }

char *mimeapps_options(const char *mime)
{
    wps_app *a = calloc(64, sizeof *a);
    int n = a ? wps_apps_for_mime(mime, a, 64) : 0;
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
    wps_app *a = calloc(64, sizeof *a);
    int n = a ? wps_apps_for_mime(mime, a, 64) : 0;
    char *v = NULL;
    for (int i = 0; i < n && !v; i++)
        if (a[i].is_default) { clean_name(a[i].name); v = strdup(a[i].name); }
    free(a);
    return v ? v : strdup("");   /* (none set: "") */
}

bool mimeapps_set(const char *mime, const char *name)
{
    wps_app *a = calloc(64, sizeof *a);
    int n = a ? wps_apps_for_mime(mime, a, 64) : 0;
    bool ok = false;
    for (int i = 0; i < n && !ok; i++) {
        clean_name(a[i].name);
        if (!strcmp(a[i].name, name)) ok = wps_app_set_default_mime(mime, a[i].id);
    }
    free(a);
    return ok;
}

/* ---- the hooks (wps_props.h) */

static char *backing_get(wpo_object *o, const wpo_property_def *p)
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
        wps_app_types(d, types, sizeof types);
        for (char *q = types; *q; q++) if (*q == '\t') *q = ' ';
        return strdup(types);
    }
    return NULL;
}

static bool backing_set(wpo_object *o, const wpo_property_def *p, const char *v)
{
    if (!strcmp(p->backing, "mimeapps")) {
        char *m = file_of(o) ? type_of(o) : NULL;
        bool ok = m && mimeapps_set(m, v);
        free(m);
        return ok;
    }
    if (!strcmp(p->backing, "desktop:MimeType")) {
        const char *d = desktop_entry_of(o);
        return d && wps_app_set_types(d, v);
    }
    return false;
}

static char *backing_options(wpo_object *o, const wpo_property_def *p)
{
    if (strcmp(p->backing, "mimeapps")) return NULL;
    char *m = file_of(o) ? type_of(o) : NULL;
    char *v = m ? mimeapps_options(m) : NULL;
    free(m);
    return v;
}

void backing_init(void)
{
    wps_backing_get = backing_get;
    wps_backing_set = backing_set;
    wps_backing_options = backing_options;
}
