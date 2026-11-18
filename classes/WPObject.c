#include "WPObject.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wps_classes.h"

void WPObject__wpInitData(wpo_object *self)
{
    WPObject_parent_wpInitData(self);
    /* Instance data starts zeroed: no title, no ID, not dirty. */
}

void WPObject__wpUnInitData(wpo_object *self)
{
    free(WPObject_data(self)->title);
    wps_state_free(WPObject_data(self)->state);
    WPObject_parent_wpUnInitData(self);
}

char *WPObject__wpQueryTitle(wpo_object *self)
{
    WPObjectData *d = WPObject_data(self);
    if (d->title) return wps_strdup(d->title);
    /* No title of its own: use the class's title. */
    return WPObject_wpclsQueryTitle(self->cls);
}

bool WPObject__wpSetTitle(wpo_object *self, const char *title)
{
    WPObjectData *d = WPObject_data(self);
    char *t = wps_strdup(title);
    if (title && !t) return false;
    free(d->title);
    d->title = t;
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

char *WPObject__wpQueryHandle(wpo_object *self)
{
    /* "" until assigned: IDs are given lazily, when the service first
     * stores something about the object (design: Object identity). */
    return wps_strdup(WPObject_data(self)->id);
}

char *WPObject__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    (void)self;
    return wps_concat(menu, "1\tOpen\n2\tProperties\n");
}

bool WPObject__wpMenuItemSelected(wpo_object *self, int64_t item)
{
    (void)self;
    return item == 1 || item == 2;   /* handled in M5, with the GUI API */
}

void WPObject__wpSaveDeferred(wpo_object *self)
{
    WPObject_data(self)->dirty = 1;
    if (wps_on_dirty) wps_on_dirty(self);   /* the service schedules the write */
}

void WPObject__wpObjectChanged(wpo_object *self)
{
    WPObject_data(self)->changes++;          /* subscribers are notified in M5 */
}

bool WPObject__wpSaveState(wpo_object *self)
{
    WPObjectData *d = WPObject_data(self);
    if (d->title) wps_state_put_str(self, "WPObject", "title", d->title);
    else wps_state_remove(self, "WPObject", "title");
    return true;
}

void WPObject__wpRestoreProperties(wpo_object *self)
{
    WPObjectData *d = WPObject_data(self);
    char *t = NULL;
    free(d->title);
    d->title = wps_state_get_str(self, "WPObject", "title", &t) ? t : NULL;
}

char *WPObject__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Object");
}
