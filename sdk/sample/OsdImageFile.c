/* The SDK's sample plug-in (OsdImageFile.osdi). Build: make (the SDK's
 * Makefile); install: copy osdimagefile.so to ~/.config/wps/plugins and log
 * in again. It runs in its own sandboxed host (wps-plughost): what it
 * changes in an object's state goes back to WPS. */
#include "OsdImageFile.h"

#include <stdlib.h>

#include "osd_classes.h"
#include "osd_state.h"

#define RATE_FIVE 7001   /* its menu item */

char *OsdImageFile__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Image");
}

char *OsdImageFile__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    char *base = OsdImageFile_parent_osdModifyPopupMenu(self, menu);
    char *r = osd_concat(base, "7001\tRate it 5 stars\n");
    free(base);
    return r;
}

bool OsdImageFile__osdMenuItemSelected(osdo_object *self, int64_t item)
{
    if (item != RATE_FIVE) return OsdImageFile_parent_osdMenuItemSelected(self, item);
    /* its declared property, as WPS keeps it (a choice: the option's text) */
    osd_state_put_str(self, "OsdImageFile", "rating", "5");
    return true;
}

osdo_status osdo_plugin_init(uint32_t abi)
{
    if (abi != OSDO_ABI_VERSION) return OSDO_EVERSION;
    return OsdImageFile_register();
}
