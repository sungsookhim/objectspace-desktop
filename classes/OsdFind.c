/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "OsdFind.h"

#include "osd_classes.h"

char *OsdFind__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Find");
}
