#include "WPProgramsFolder.h"

#include "wps_classes.h"

char *WPProgramsFolder__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Programs");
}
