/* Plug-in: a subclass of TBase, compiled against an older or newer TBase. */
#include "TChild.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void TChild__wpInitData(wpo_object *self)
{
    TChild_parent_wpInitData(self);
    TChild_data(self)->c = 77;
    TBase_tSetA(self, 5);
}

char *TChild__tDescribe(wpo_object *self)
{
    char *base = TChild_parent_tDescribe(self);
    char *s = malloc(128);
    if (s) snprintf(s, 128, "%s child c=%d", base ? base : "?", TChild_data(self)->c);
    free(base);
    return s;
}

int64_t TChild__tGetC(wpo_object *self) { return TChild_data(self)->c; }

wpo_status wpo_plugin_init(uint32_t abi)
{
    if (abi != WPO_ABI_VERSION) return WPO_EVERSION;
    return TChild_register();
}
