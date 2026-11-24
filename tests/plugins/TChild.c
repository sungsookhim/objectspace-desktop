/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Plug-in: a subclass of TBase, compiled against an older or newer TBase. */
#include "TChild.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void TChild__osdInitData(osdo_object *self)
{
    TChild_parent_osdInitData(self);
    TChild_data(self)->c = 77;
    TBase_tSetA(self, 5);
}

char *TChild__tDescribe(osdo_object *self)
{
    char *base = TChild_parent_tDescribe(self);
    char *s = malloc(128);
    if (s) snprintf(s, 128, "%s child c=%d", base ? base : "?", TChild_data(self)->c);
    free(base);
    return s;
}

int64_t TChild__tGetC(osdo_object *self) { return TChild_data(self)->c; }

osdo_status osdo_plugin_init(uint32_t abi)
{
    if (abi != OSDO_ABI_VERSION) return OSDO_EVERSION;
    return TChild_register();
}
