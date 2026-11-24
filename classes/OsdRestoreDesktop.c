/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "OsdRestoreDesktop.h"

#include "osd_classes.h"

char *OsdRestoreDesktop__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Restore Desktop");
}
