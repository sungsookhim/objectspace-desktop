#include "WPObject.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#include "wps_classes.h"

void WPObject__wpInitData(wpo_object *self)
{
    WPObject_parent_wpInitData(self);
    /* Instance data starts zeroed: no title, no ID, not dirty. */
}

void WPObject__wpUnInitData(wpo_object *self)
{
    free(WPObject_data(self)->title);
    WPObject_parent_wpUnInitData(self);
}

char *WPObject__wpQueryTitle(wpo_object *self)
{
    WPObjectData *d = WPObject_data(self);
    if (d->title) return wps_strdup(d->title);
    /* No title of its own: use the class's title. */
    return WPObject_wpclsQueryTitle(self->cls);
}

/* Lazy identity (design: Object identity): an object gets an ID only when
 * WPS first stores something about it. */
static void assign_id(WPObjectData *d)
{
    if (d->id[0]) return;
    unsigned char b[16];
    if (getrandom(b, sizeof b, 0) != (ssize_t)sizeof b) return;
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);   /* UUID version 4 */
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    snprintf(d->id, sizeof d->id,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

bool WPObject__wpSetTitle(wpo_object *self, const char *title)
{
    WPObjectData *d = WPObject_data(self);
    char *t = wps_strdup(title);
    if (title && !t) return false;
    free(d->title);
    d->title = t;
    assign_id(d);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

char *WPObject__wpQueryHandle(wpo_object *self)
{
    return wps_strdup(WPObject_data(self)->id);   /* "" until assigned */
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
    WPObject_data(self)->dirty = 1;   /* written to storage in M2 */
}

void WPObject__wpObjectChanged(wpo_object *self)
{
    (void)self;   /* notifies subscribers in M5 */
}

wpo_pending *WPObject__wpRestoreState(wpo_object *self)
{
    (void)self;
    /* Reading state is disk I/O and runs on a worker from M2 on; the shell
     * completes at once. */
    wpo_pending *p = wpo_pending_new();
    wpo_pending_complete(p, wpo_v_bool(true));
    return p;
}

char *WPObject__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Object");
}
