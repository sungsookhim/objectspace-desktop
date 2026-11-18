#include "WPProgram.h"

#include <stdlib.h>

#include "wps_classes.h"

void WPProgram__wpUnInitData(wpo_object *self)
{
    free(WPProgram_data(self)->command);
    WPProgram_parent_wpUnInitData(self);
}

char *WPProgram__wpQueryCommand(wpo_object *self)
{
    return wps_strdup(WPProgram_data(self)->command);
}

bool WPProgram__wpSetCommand(wpo_object *self, const char *command)
{
    char *c = wps_strdup(command);
    if (command && !c) return false;
    free(WPProgram_data(self)->command);
    WPProgram_data(self)->command = c;
    WPObject_wpSaveDeferred(self);
    return true;
}

char *WPProgram__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Program");
}
