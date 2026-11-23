/* The SDK's sample plug-in (WPImageFile.wpi). Build: make (the SDK's
 * Makefile); install: copy wpimagefile.so to ~/.config/wps/plugins and log
 * in again. It runs in its own sandboxed host (wps-plughost): what it
 * changes in an object's state goes back to WPS. */
#include "WPImageFile.h"

#include <stdlib.h>

#include "wps_classes.h"
#include "wps_state.h"

#define RATE_FIVE 7001   /* its menu item */

char *WPImageFile__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Image");
}

char *WPImageFile__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    char *base = WPImageFile_parent_wpModifyPopupMenu(self, menu);
    char *r = wps_concat(base, "7001\tRate it 5 stars\n");
    free(base);
    return r;
}

bool WPImageFile__wpMenuItemSelected(wpo_object *self, int64_t item)
{
    if (item != RATE_FIVE) return WPImageFile_parent_wpMenuItemSelected(self, item);
    /* its declared property, as WPS keeps it (a choice: the option's text) */
    wps_state_put_str(self, "WPImageFile", "rating", "5");
    return true;
}

wpo_status wpo_plugin_init(uint32_t abi)
{
    if (abi != WPO_ABI_VERSION) return WPO_EVERSION;
    return WPImageFile_register();
}
