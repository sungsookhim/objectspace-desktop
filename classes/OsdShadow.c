/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "OsdShadow.h"

#include "osd_classes.h"

void OsdShadow__osdUnInitData(osdo_object *self)
{
    osdo_unref(OsdShadow_data(self)->target);
    OsdShadow_parent_osdUnInitData(self);
}

char *OsdShadow__osdQueryTitle(osdo_object *self)
{
    /* A shadow shows its target's title unless it has its own. */
    osdo_object *t = OsdShadow_data(self)->target;
    if (!OsdObject_data(self)->title && t) return OsdObject_osdQueryTitle(t);
    return OsdShadow_parent_osdQueryTitle(self);
}

osdo_object *OsdShadow__osdQueryTarget(osdo_object *self)
{
    return osdo_ref(OsdShadow_data(self)->target);
}

bool OsdShadow__osdSetTarget(osdo_object *self, osdo_object *target)
{
    if (target && !osdo_is_a(target, OsdObjectClass)) return false;
    if (target == self) return false;
    osdo_object *old = OsdShadow_data(self)->target;
    OsdShadow_data(self)->target = osdo_ref(target);
    osdo_unref(old);
    return true;
}

char *OsdShadow__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Shadow");
}
