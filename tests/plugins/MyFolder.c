/* Plug-in: adds a menu item to every folder once it replaces WPFolder. */
#include "MyFolder.h"

#include <stdlib.h>

#include "wps_classes.h"

char *MyFolder__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    char *base = MyFolder_parent_wpModifyPopupMenu(self, menu);
    char *r = wps_concat(base, "90\tMy Plug-in Item\n");
    free(base);
    return r;
}

wpo_status wpo_plugin_init(uint32_t abi)
{
    if (abi != WPO_ABI_VERSION) return WPO_EVERSION;
    return MyFolder_register();
}
