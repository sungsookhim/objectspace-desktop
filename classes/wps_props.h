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

int wps_option_index(const char *options, const char *v);
char *wps_option_at(const char *options, long k);

#endif
