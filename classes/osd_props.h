/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Declared properties (object-properties-design.md 2.1; osd_props.c). */
#ifndef OSD_PROPS_H
#define OSD_PROPS_H

#include <stdbool.h>

#include "osdo.h"

/* Its value as text (a checkbox "1"/"0", a choice its option); NULL if the
 * object has no such property. Caller frees. */
char *osd_prop_get(osdo_object *self, const char *name);
/* Sets it from text (a choice: the option or its number; a checkbox:
 * 1/0, true/false, yes/no, on/off); false if refused. */
bool osd_prop_set(osdo_object *self, const char *name, const char *value);
/* Every property and its value: "name\tvalue\n" lines, the root class's first. */
char *osd_prop_list(osdo_object *self);
/* The settings pages of the declared properties ("page"/"field" lines; the
 * setter "@prop"); only those `only` declares if it is not NULL. */
char *osd_prop_pages(osdo_object *self, const osdo_class *only);

/* The same, with where the value came from in `origin`: "object", a scope
 * of the chain ("type:image/png", "class:OsdFolder"), or "default". */
char *osd_prop_get_origin(osdo_object *self, const char *name, char *origin, size_t on);
/* Its own value removed: the chain's shows again. False if it has none to
 * remove (a value its class gives, like its title). */
bool osd_prop_reset(osdo_object *self, const char *name);
/* A value from text, canonical for the property's type; NULL if not one. */
char *osd_prop_canonical(const osdo_property_def *p, const char *v);
/* The class whose state keeps a property of `cls`, or NULL. */
const char *osd_prop_store(const osdo_class *cls, const char *name);
/* The service's: the chain's value for a property the object has none of
 * (its type's, its class's, ...), with the scope in origin; NULL if none. */
extern char *(*osd_prop_inherit)(osdo_object *self, const osdo_property_def *p, const char *store, char *origin, size_t on);

/* The service's: a class's or a type's own value (NULL: none), and setting
 * it (value NULL: removed); for objects that stand for all of a class (the
 * System object's Folders and Fonts fields are all folders'). */
extern char *(*osd_scope_get)(const char *scope, const char *store, const char *prop);
extern bool (*osd_scope_set)(const char *scope, const char *store, const char *prop, const char *value);
/* The service's: a backed property's value where Linux keeps it (NULL: not
 * kept for this object), setting it there, and a choice's options there. */
extern char *(*osd_backing_get)(osdo_object *self, const osdo_property_def *p);
extern bool (*osd_backing_set)(osdo_object *self, const osdo_property_def *p, const char *value);
extern char *(*osd_backing_options)(osdo_object *self, const osdo_property_def *p);
/* The service's: the schemes' titles ("Classic,Dark,High contrast,..."), or NULL. */
extern char *(*osd_scheme_titles)(void);

/* A choice's options now ("a,b,c": its `options` method's, else declared). Caller frees. */
char *osd_prop_choices(osdo_object *self, const osdo_property_def *p);
int osd_option_index(const char *options, const char *v);
char *osd_option_at(const char *options, long k);

#endif
