#include "WPAbstract.h"

#include "wps_classes.h"

char *WPAbstract__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Abstract Object");
}
