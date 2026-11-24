#include "OsdRestoreDesktop.h"

#include "osd_classes.h"

char *OsdRestoreDesktop__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Restore Desktop");
}
