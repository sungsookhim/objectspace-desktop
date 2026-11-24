#include "OsdMinWinViewer.h"

#include "osd_classes.h"

char *OsdMinWinViewer__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Minimized Window Viewer");
}
