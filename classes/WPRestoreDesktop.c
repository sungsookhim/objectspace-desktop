#include "WPRestoreDesktop.h"

#include "wps_classes.h"

char *WPRestoreDesktop__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Restore Desktop");
}
