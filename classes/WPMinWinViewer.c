#include "WPMinWinViewer.h"

#include "wps_classes.h"

char *WPMinWinViewer__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Minimized Window Viewer");
}
