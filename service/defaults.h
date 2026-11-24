/* Class and type settings: the chain's levels between an object and its
 * declared default (object-properties-design.md 2.2; defaults.c). Scopes
 * are "class:NAME" and "type:MIME" ("type:image/\*" for a whole kind).
 * Loop thread only. */
#ifndef OSD_DEFAULTS_H
#define OSD_DEFAULTS_H

#include "db.h"

void osd_defaults_init(osd_db *db, const char *vol);
/* A scope's value of a property (by the class whose state keeps it), or
 * NULL if the scope has none. Caller frees. */
char *osd_defaults_get(const char *scope, const char *store, const char *prop);
/* Sets it (value NULL: removes it, so the next level shows). 0 or -1. */
int osd_defaults_set(const char *scope, const char *store, const char *prop, const char *value);
/* Every value a scope holds. */
void osd_defaults_each(const char *scope, void (*fn)(const char *store, const char *prop, const char *value, void *user), void *user);

#endif
