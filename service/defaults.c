/* Class and type settings (object-properties-design.md 2.2): the levels of
 * the chain between an object and its class's declared default - "all
 * folders" (class:OsdFolder), "all PNG files" (type:image/png), "all images"
 * (type:image/\*). Kept in the user's DB under the scope as the locator,
 * per store class and property, on the volume of the user's WPS folder. */
#define _GNU_SOURCE
#include "defaults.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "attrs.h"
#include "env.h"

static osd_db *db;
static char vol[OSD_VOLID_MAX];

/* the scopes read so far, with their rows */
struct scope {
    char *name;
    osd_props by_store_prop;   /* key "Store\x1fprop", text values */
    struct scope *next;
};
static struct scope *scopes;

void osd_defaults_init(osd_db *d, const char *v)
{
    db = d;
    snprintf(vol, sizeof vol, "%s", v ? v : "");
}

static void row(const char *cls, const char *prop, char type, const void *val, uint32_t len, void *user)
{
    struct scope *s = user;
    char k[512];
    snprintf(k, sizeof k, "%s\x1f%s", cls, prop);
    if (type == 's') osd_props_set(&s->by_store_prop, k, 's', val, len);
}

static struct scope *scope_of(const char *name)
{
    for (struct scope *s = scopes; s; s = s->next)
        if (!strcmp(s->name, name)) return s;
    struct scope *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->name = strdup(name);
    osd_props_init(&s->by_store_prop);
    if (db) osd_db_get_locator(db, vol, name, row, s);
    s->next = scopes;
    scopes = s;
    return s;
}

char *osd_defaults_get(const char *scope, const char *store, const char *prop)
{
    struct scope *s = scope_of(scope);
    if (!s) return NULL;
    char k[512], *v = NULL;
    snprintf(k, sizeof k, "%s\x1f%s", store, prop);
    return osd_props_str(&s->by_store_prop, k, &v) ? v : NULL;
}

int osd_defaults_set(const char *scope, const char *store, const char *prop, const char *value)
{
    struct scope *s = scope_of(scope);
    if (!s || !db) return -1;
    char k[512], key[1024];
    snprintf(k, sizeof k, "%s\x1f%s", store, prop);
    osd_db_key(key, sizeof key, scope, store, prop);
    if (!value) {
        osd_props_remove(&s->by_store_prop, k);
        return osd_db_delete(db, vol, key) < 0 ? -1 : 0;
    }
    osd_props_set_str(&s->by_store_prop, k, value);
    return osd_db_put(db, vol, key, 's', value, (uint32_t)strlen(value)) < 0 ? -1 : 0;
}

void osd_defaults_each(const char *scope, void (*fn)(const char *store, const char *prop, const char *value, void *user), void *user)
{
    struct scope *s = scope_of(scope);
    for (size_t i = 0; s && i < s->by_store_prop.n; i++) {
        const osd_prop *p = &s->by_store_prop.p[i];
        char k[512], v[4096];
        snprintf(k, sizeof k, "%s", p->key);
        char *sep = strchr(k, '\x1f');
        if (!sep) continue;
        *sep = 0;
        snprintf(v, sizeof v, "%.*s", (int)p->len, (const char *)p->val);
        fn(k, sep + 1, v, user);
    }
}
