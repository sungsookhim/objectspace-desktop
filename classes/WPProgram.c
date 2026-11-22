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

bool WPProgram__wpSetProgramType(wpo_object *self, int64_t choice)
{
    if (choice < 0 || choice > 2) return false;
    wps_state_put_int(self, "WPProgram", "ptype", choice);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPProgram__wpSetCloseOnExit(wpo_object *self, bool on)
{
    wps_state_put_int(self, "WPProgram", "close_on_exit", on);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPProgram__wpSetWorkingDir(wpo_object *self, const char *dir)
{
    if (!dir || strchr(dir, '\n') || strchr(dir, '\t') || (*dir && dir[0] != '/' && dir[0] != '~')) return false;
    if (*dir) wps_state_put_str(self, "WPProgram", "cwd", dir);
    else wps_state_remove(self, "WPProgram", "cwd");
    WPObject_wpSaveDeferred(self);
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
    int64_t ptype = 0, close_exit = 1;
    char *wd = NULL;
    wps_state_get_int(self, "WPProgram", "ptype", &ptype);
    wps_state_get_int(self, "WPProgram", "close_on_exit", &close_exit);
    wps_state_get_str(self, "WPProgram", "cwd", &wd);
    size_t n = (c ? strlen(c) : 0) + (wd ? strlen(wd) : 0) + 512;
    char *line = malloc(n);
    snprintf(line, n, "page\tProgram\nfield\tcommand\ttext\tCommand\t%s\twpSetCommand\n"
             "field\tcwd\ttext\tWorking directory\t%s\twpSetWorkingDir\n"
             "field\tprogram_type\tchoice\tProgram type\t%lld\twpSetProgramType\tautomatic,window,text mode (in a terminal)\n"
             "field\tclose_on_exit\tcheckbox\tClose window on exit (text mode)\t%d\twpSetCloseOnExit\n",
             c ? c : "", wd ? wd : "", (long long)ptype, close_exit != 0);
    free(wd);
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
