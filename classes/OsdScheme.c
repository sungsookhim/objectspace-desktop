/* A scheme (OsdScheme.osdi): its colours and sizes are its declared
 * properties; the GUI draws with the one the Scheme Palette chooses. */
#include "OsdScheme.h"

#include "osd_classes.h"

char *OsdScheme__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Scheme");
}
