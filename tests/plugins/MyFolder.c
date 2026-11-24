/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Plug-in: adds a menu item to every folder once it replaces OsdFolder. */
#include "MyFolder.h"

#include <stdlib.h>

#include "osd_classes.h"

char *MyFolder__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    char *base = MyFolder_parent_osdModifyPopupMenu(self, menu);
    char *r = osd_concat(base, "90\tMy Plug-in Item\n");
    free(base);
    return r;
}

osdo_status osdo_plugin_init(uint32_t abi)
{
    if (abi != OSDO_ABI_VERSION) return OSDO_EVERSION;
    return MyFolder_register();
}
