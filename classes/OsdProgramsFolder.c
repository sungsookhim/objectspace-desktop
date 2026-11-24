/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "OsdProgramsFolder.h"

#include "osd_classes.h"

char *OsdProgramsFolder__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Programs");
}
