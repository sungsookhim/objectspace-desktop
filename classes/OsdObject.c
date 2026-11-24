/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "OsdObject.h"
#include "osd_props.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osd_classes.h"

void OsdObject__osdInitData(osdo_object *self)
{
    OsdObject_parent_osdInitData(self);
    /* Instance data starts zeroed: no title, no ID, not dirty. */
}

void OsdObject__osdUnInitData(osdo_object *self)
{
    free(OsdObject_data(self)->title);
    free(OsdObject_data(self)->icon);
    osd_state_free(OsdObject_data(self)->state);
    OsdObject_parent_osdUnInitData(self);
}

char *OsdObject__osdQueryTitle(osdo_object *self)
{
    OsdObjectData *d = OsdObject_data(self);
    if (d->title) return osd_strdup(d->title);
    /* No title of its own: use the class's title. */
    return OsdObject_osdclsQueryTitle(self->cls);
}

bool OsdObject__osdSetTitle(osdo_object *self, const char *title)
{
    OsdObjectData *d = OsdObject_data(self);
    char *t = osd_strdup(title);
    if (title && !t) return false;
    free(d->title);
    d->title = t;
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

char *OsdObject__osdQueryHandle(osdo_object *self)
{
    /* "" until assigned: IDs are given lazily, when the service first
     * stores something about the object (design: Object identity). */
    return osd_strdup(OsdObject_data(self)->id);
}

char *OsdObject__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    (void)self;
    return osd_concat(menu, "1\tOpen\n2\tProperties\n41\tCreate another\n40\tDelete...\n");
}

char *OsdObject__osdQueryIcon(osdo_object *self)
{
    OsdObjectData *d = OsdObject_data(self);
    return osd_strdup(d->icon ? d->icon : "");
}

bool OsdObject__osdSetIcon(osdo_object *self, const char *path)
{
    OsdObjectData *d = OsdObject_data(self);
    /* "": its kind's icon again; otherwise a full path (the GUI keeps a
     * PNG copy in Objectspace's icons folder, so the source may go). */
    if (path && *path && path[0] != '/') return false;
    char *p = path && *path ? osd_strdup(path) : NULL;
    if (path && *path && !p) return false;
    free(d->icon);
    d->icon = p;
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

/* The pages of every declared property (each class's .osdi), the root
 * class's first; a class adds pages of its own by overriding this. */
char *OsdObject__osdAddSettingsPages(osdo_object *self, const char *pages)
{
    char *decl = osd_prop_pages(self, NULL);
    char *r = osd_concat(pages, decl);
    free(decl);
    return r;
}

char *OsdObject__osdQueryProperty(osdo_object *self, const char *name) { return osd_prop_get(self, name); }
bool OsdObject__osdSetProperty(osdo_object *self, const char *name, const char *value) { return osd_prop_set(self, name, value); }
char *OsdObject__osdListProperties(osdo_object *self) { return osd_prop_list(self); }

bool OsdObject__osdMenuItemSelected(osdo_object *self, int64_t item)
{
    (void)self;
    return item == 1 || item == 2;   /* handled in M5, with the GUI API */
}

void OsdObject__osdSaveDeferred(osdo_object *self)
{
    OsdObject_data(self)->dirty = 1;
    if (osd_on_dirty) osd_on_dirty(self);   /* the service schedules the write */
}

void OsdObject__osdObjectChanged(osdo_object *self)
{
    OsdObject_data(self)->changes++;
    if (osd_on_changed) osd_on_changed(self);   /* the service tells subscribers */
}

bool OsdObject__osdQueryTemplate(osdo_object *self) { return OsdObject_data(self)->is_template; }

bool OsdObject__osdSetTemplate(osdo_object *self, bool on)
{
    OsdObject_data(self)->is_template = on;
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

bool OsdObject__osdSaveState(osdo_object *self)
{
    OsdObjectData *d = OsdObject_data(self);
    if (d->title) osd_state_put_str(self, "OsdObject", "title", d->title);
    else osd_state_remove(self, "OsdObject", "title");
    if (d->is_template) osd_state_put_int(self, "OsdObject", "template", 1);
    else osd_state_remove(self, "OsdObject", "template");
    if (d->icon) osd_state_put_str(self, "OsdObject", "icon", d->icon);
    else osd_state_remove(self, "OsdObject", "icon");
    return true;
}

void OsdObject__osdRestoreProperties(osdo_object *self)
{
    OsdObjectData *d = OsdObject_data(self);
    char *t = NULL;
    int64_t tpl = 0;
    free(d->title);
    d->title = osd_state_get_str(self, "OsdObject", "title", &t) ? t : NULL;
    d->is_template = osd_state_get_int(self, "OsdObject", "template", &tpl) && tpl;
    free(d->icon);
    t = NULL;
    d->icon = osd_state_get_str(self, "OsdObject", "icon", &t) ? t : NULL;
}

char *OsdObject__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Object");
}
