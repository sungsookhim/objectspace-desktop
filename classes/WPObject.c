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
    free(WPObject_data(self)->icon);
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
    return wps_concat(menu, "1\tOpen\n2\tProperties\n41\tCreate another\n40\tDelete...\n");
}

char *WPObject__wpQueryIcon(wpo_object *self)
{
    WPObjectData *d = WPObject_data(self);
    return wps_strdup(d->icon ? d->icon : "");
}

bool WPObject__wpSetIcon(wpo_object *self, const char *path)
{
    WPObjectData *d = WPObject_data(self);
    /* "": its kind's icon again; otherwise a full path (the GUI keeps a
     * PNG copy in WPS's icons folder, so the source may go). */
    if (path && *path && path[0] != '/') return false;
    char *p = path && *path ? wps_strdup(path) : NULL;
    if (path && *path && !p) return false;
    free(d->icon);
    d->icon = p;
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

char *WPObject__wpAddSettingsPages(wpo_object *self, const char *pages)
{
    WPObjectData *d = WPObject_data(self);
    char *title = WPObject_wpQueryTitle(self);
    size_t n = strlen(title ? title : "") + strlen(d->icon ? d->icon : "") + 192;
    char *line = malloc(n);
    snprintf(line, n, "page\tGeneral\nfield\ttitle\ttext\tTitle\t%s\twpSetTitle\n"
                      "field\ttemplate\tcheckbox\tTemplate\t%d\twpSetTemplate\n"
                      "page\tIcon\nfield\ticon\ticon\tIcon\t%s\twpSetIcon\n", title ? title : "", d->is_template,
             d->icon ? d->icon : "");
    char *r = wps_concat(pages, line);
    free(line);
    free(title);
    return r;
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
    WPObject_data(self)->changes++;
    if (wps_on_changed) wps_on_changed(self);   /* the service tells subscribers */
}

bool WPObject__wpQueryTemplate(wpo_object *self) { return WPObject_data(self)->is_template; }

bool WPObject__wpSetTemplate(wpo_object *self, bool on)
{
    WPObject_data(self)->is_template = on;
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPObject__wpSaveState(wpo_object *self)
{
    WPObjectData *d = WPObject_data(self);
    if (d->title) wps_state_put_str(self, "WPObject", "title", d->title);
    else wps_state_remove(self, "WPObject", "title");
    if (d->is_template) wps_state_put_int(self, "WPObject", "template", 1);
    else wps_state_remove(self, "WPObject", "template");
    if (d->icon) wps_state_put_str(self, "WPObject", "icon", d->icon);
    else wps_state_remove(self, "WPObject", "icon");
    return true;
}

void WPObject__wpRestoreProperties(wpo_object *self)
{
    WPObjectData *d = WPObject_data(self);
    char *t = NULL;
    int64_t tpl = 0;
    free(d->title);
    d->title = wps_state_get_str(self, "WPObject", "title", &t) ? t : NULL;
    d->is_template = wps_state_get_int(self, "WPObject", "template", &tpl) && tpl;
    free(d->icon);
    t = NULL;
    d->icon = wps_state_get_str(self, "WPObject", "icon", &t) ? t : NULL;
}

char *WPObject__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Object");
}
