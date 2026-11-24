/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "OsdAbstract.h"

#include "osd_classes.h"

char *OsdAbstract__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Abstract Object");
}
