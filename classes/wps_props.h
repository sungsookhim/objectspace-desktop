/* Declared properties (object-properties-design.md 2.1; wps_props.c). */
#ifndef WPS_PROPS_H
#define WPS_PROPS_H

#include <stdbool.h>

#include "wpo.h"

/* Its value as text (a checkbox "1"/"0", a choice its option); NULL if the
 * object has no such property. Caller frees. */
char *wps_prop_get(wpo_object *self, const char *name);
/* Sets it from text (a choice: the option or its number; a checkbox:
 * 1/0, true/false, yes/no, on/off); false if refused. */
bool wps_prop_set(wpo_object *self, const char *name, const char *value);
/* Every property and its value: "name\tvalue\n" lines, the root class's first. */
char *wps_prop_list(wpo_object *self);
/* The settings pages of the declared properties ("page"/"field" lines; the
 * setter "@prop"); only those `only` declares if it is not NULL. */
char *wps_prop_pages(wpo_object *self, const wpo_class *only);

/* The same, with where the value came from in `origin`: "object", a scope
 * of the chain ("type:image/png", "class:WPFolder"), or "default". */
char *wps_prop_get_origin(wpo_object *self, const char *name, char *origin, size_t on);
/* Its own value removed: the chain's shows again. False if it has none to
 * remove (a value its class gives, like its title). */
bool wps_prop_reset(wpo_object *self, const char *name);
/* A value from text, canonical for the property's type; NULL if not one. */
char *wps_prop_canonical(const wpo_property_def *p, const char *v);
/* The class whose state keeps a property of `cls`, or NULL. */
const char *wps_prop_store(const wpo_class *cls, const char *name);
/* The service's: the chain's value for a property the object has none of
 * (its type's, its class's, ...), with the scope in origin; NULL if none. */
extern char *(*wps_prop_inherit)(wpo_object *self, const wpo_property_def *p, const char *store, char *origin, size_t on);

/* The service's: a class's or a type's own value (NULL: none), and setting
 * it (value NULL: removed); for objects that stand for all of a class (the
 * System object's Folders and Fonts fields are all folders'). */
extern char *(*wps_scope_get)(const char *scope, const char *store, const char *prop);
extern bool (*wps_scope_set)(const char *scope, const char *store, const char *prop, const char *value);
/* The service's: a backed property's value where Linux keeps it (NULL: not
 * kept for this object), setting it there, and a choice's options there. */
extern char *(*wps_backing_get)(wpo_object *self, const wpo_property_def *p);
extern bool (*wps_backing_set)(wpo_object *self, const wpo_property_def *p, const char *value);
extern char *(*wps_backing_options)(wpo_object *self, const wpo_property_def *p);
/* The service's: the schemes' titles ("Classic,Dark,High contrast,..."), or NULL. */
extern char *(*wps_scheme_titles)(void);

/* A choice's options now ("a,b,c": its `options` method's, else declared). Caller frees. */
char *wps_prop_choices(wpo_object *self, const wpo_property_def *p);
int wps_option_index(const char *options, const char *v);
char *wps_option_at(const char *options, long k);

#endif
