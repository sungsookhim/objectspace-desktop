#include "WPShadow.h"

#include "wps_classes.h"

void WPShadow__wpUnInitData(wpo_object *self)
{
    wpo_unref(WPShadow_data(self)->target);
    WPShadow_parent_wpUnInitData(self);
}

char *WPShadow__wpQueryTitle(wpo_object *self)
{
    /* A shadow shows its target's title unless it has its own. */
    wpo_object *t = WPShadow_data(self)->target;
    if (!WPObject_data(self)->title && t) return WPObject_wpQueryTitle(t);
    return WPShadow_parent_wpQueryTitle(self);
}

wpo_object *WPShadow__wpQueryTarget(wpo_object *self)
{
    return wpo_ref(WPShadow_data(self)->target);
}

bool WPShadow__wpSetTarget(wpo_object *self, wpo_object *target)
{
    if (target && !wpo_is_a(target, WPObjectClass)) return false;
    if (target == self) return false;
    wpo_object *old = WPShadow_data(self)->target;
    WPShadow_data(self)->target = wpo_ref(target);
    wpo_unref(old);
    return true;
}

char *WPShadow__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Shadow");
}
