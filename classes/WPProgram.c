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

bool WPProgram__wpSaveState(wpo_object *self)
{
    WPProgram_parent_wpSaveState(self);
    const char *c = WPProgram_data(self)->command;
    if (c) wps_state_put_str(self, "WPProgram", "command", c);
    else wps_state_remove(self, "WPProgram", "command");
    return true;
}

void WPProgram__wpRestoreProperties(wpo_object *self)
{
    WPProgram_parent_wpRestoreProperties(self);
    char *c = NULL;
    free(WPProgram_data(self)->command);
    WPProgram_data(self)->command = wps_state_get_str(self, "WPProgram", "command", &c) ? c : NULL;
}

char *WPProgram__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Program");
}
