/*
 * The state bag: the stored properties of one object, per class.
 *
 * Classes put their properties here in wpSaveState and read them in
 * wpRestoreProperties; the service loads the bag from storage before
 * restoring and writes the keys each class touched after saving. Only
 * touched keys are written, so concurrent writers of other keys are never
 * overwritten (design: Concurrency). Loop thread only.
 */
#ifndef WPS_STATE_H
#define WPS_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "attrs.h"
#include "wpo.h"

typedef struct wps_state wps_state;

typedef struct {
    char cls[64];
    wps_props props;       /* current values */
    char **touched;        /* keys set or removed since the last save */
    size_t ntouched, captouched;
} wps_cstate;

struct wps_state {
    wps_cstate *c;
    size_t n, cap;
};

void wps_state_free(wps_state *st);
wps_cstate *wps_state_class(wps_state **st, const char *cls, bool create);

/* Set a property; marks it touched only if the value changed. */
void wps_state_put_str(wpo_object *o, const char *cls, const char *key, const char *val);
void wps_state_put_int(wpo_object *o, const char *cls, const char *key, int64_t v);
void wps_state_remove(wpo_object *o, const char *cls, const char *key);
bool wps_state_get_str(wpo_object *o, const char *cls, const char *key, char **out);
bool wps_state_get_int(wpo_object *o, const char *cls, const char *key, int64_t *out);

/* Replace a class's values with loaded ones (does not mark touched). */
void wps_state_load(wpo_object *o, const char *cls, const wps_props *loaded);
void wps_state_clear_touched(wps_cstate *c);

/* Called by wpSaveDeferred; the service installs it to schedule saves. */
extern void (*wps_on_dirty)(wpo_object *o);

#endif
