#include "OsdCommandPrompt.h"

#include "osd_classes.h"

char *OsdCommandPrompt__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Command Prompt");
}
