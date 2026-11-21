#include "WPProgram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wps_classes.h"
#include "wps_startup_hook.h"

bool (*wps_startup_query)(wpo_object *o, bool *on);
bool (*wps_startup_set)(wpo_object *o, bool on);

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

bool WPProgram__wpSetStartAtLogin(wpo_object *self, bool on)
{
    if (!wps_startup_set || !wps_startup_set(self, on)) return false;
    WPObject_wpObjectChanged(self);
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

char *WPProgram__wpAddSettingsPages(wpo_object *self, const char *pages)
{
    char *base = WPProgram_parent_wpAddSettingsPages(self, pages);
    const char *c = WPProgram_data(self)->command;
    size_t n = (c ? strlen(c) : 0) + 128;
    char *line = malloc(n);
    snprintf(line, n, "page\tProgram\nfield\tcommand\ttext\tCommand\t%s\twpSetCommand\n", c ? c : "");
    bool on = false;
    if (wps_startup_query && wps_startup_query(self, &on)) {   /* an entry in Startup */
        char *l2 = wps_concat(line, on ? "field\tstart_at_login\tcheckbox\tStart at login\t1\twpSetStartAtLogin\n"
                                       : "field\tstart_at_login\tcheckbox\tStart at login\t0\twpSetStartAtLogin\n");
        free(line);
        line = l2;
    }
    char *r = wps_concat(base, line);
    free(base);
    free(line);
    return r;
}

char *WPProgram__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Program");
}
