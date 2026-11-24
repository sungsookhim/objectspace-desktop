/*
 * The state bag: the stored properties of one object, per class.
 *
 * Classes put their properties here in osdSaveState and read them in
 * osdRestoreProperties; the service loads the bag from storage before
 * restoring and writes the keys each class touched after saving. Only
 * touched keys are written, so concurrent writers of other keys are never
 * overwritten (design: Concurrency). Loop thread only.
 */
#ifndef OSD_STATE_H
#define OSD_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "attrs.h"
#include "osdo.h"

typedef struct osd_state osd_state;

typedef struct {
    char cls[64];
    osd_props props;       /* current values */
    char **touched;        /* keys set or removed since the last save */
    size_t ntouched, captouched;
} osd_cstate;

struct osd_state {
    osd_cstate *c;
    size_t n, cap;
};

void osd_state_free(osd_state *st);
osd_cstate *osd_state_class(osd_state **st, const char *cls, bool create);

/* Set a property; marks it touched only if the value changed. */
void osd_state_put_str(osdo_object *o, const char *cls, const char *key, const char *val);
void osd_state_put_int(osdo_object *o, const char *cls, const char *key, int64_t v);
void osd_state_remove(osdo_object *o, const char *cls, const char *key);
bool osd_state_get_str(osdo_object *o, const char *cls, const char *key, char **out);
bool osd_state_get_int(osdo_object *o, const char *cls, const char *key, int64_t *out);

/* Replace a class's values with loaded ones (does not mark touched). */
void osd_state_load(osdo_object *o, const char *cls, const osd_props *loaded);
void osd_state_clear_touched(osd_cstate *c);

/* Called by osdSaveDeferred; the service installs it to schedule saves. */
extern void (*osd_on_dirty)(osdo_object *o);
/* Called by osdObjectChanged; the service installs it to notify subscribers. */
extern void (*osd_on_changed)(osdo_object *o);

#endif
