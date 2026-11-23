/* A scheme (WPScheme.wpi): its colours and sizes are its declared
 * properties; the GUI draws with the one the Scheme Palette chooses. */
#include "WPScheme.h"

#include "wps_classes.h"

char *WPScheme__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Scheme");
}
