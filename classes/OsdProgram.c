#include "OsdProgram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osd_classes.h"
#include "osd_startup_hook.h"

bool (*osd_startup_query)(osdo_object *o, bool *on);
bool (*osd_startup_set)(osdo_object *o, bool on);

void OsdProgram__osdUnInitData(osdo_object *self)
{
    free(OsdProgram_data(self)->command);
    OsdProgram_parent_osdUnInitData(self);
}

char *OsdProgram__osdQueryCommand(osdo_object *self)
{
    return osd_strdup(OsdProgram_data(self)->command);
}

bool OsdProgram__osdSetCommand(osdo_object *self, const char *command)
{
    char *c = osd_strdup(command);
    if (command && !c) return false;
    free(OsdProgram_data(self)->command);
    OsdProgram_data(self)->command = c;
    OsdObject_osdSaveDeferred(self);
    return true;
}

bool OsdProgram__osdSetStartAtLogin(osdo_object *self, bool on)
{
    if (!osd_startup_set || !osd_startup_set(self, on)) return false;
    OsdObject_osdObjectChanged(self);
    return true;
}

bool OsdProgram__osdSetProgramType(osdo_object *self, int64_t choice)
{
    if (choice < 0 || choice > 2) return false;
    osd_state_put_int(self, "OsdProgram", "ptype", choice);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

bool OsdProgram__osdSetCloseOnExit(osdo_object *self, bool on)
{
    osd_state_put_int(self, "OsdProgram", "close_on_exit", on);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

bool OsdProgram__osdSetWorkingDir(osdo_object *self, const char *dir)
{
    if (!dir || strchr(dir, '\n') || strchr(dir, '\t') || (*dir && dir[0] != '/' && dir[0] != '~')) return false;
    if (*dir) osd_state_put_str(self, "OsdProgram", "cwd", dir);
    else osd_state_remove(self, "OsdProgram", "cwd");
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

bool OsdProgram__osdSaveState(osdo_object *self)
{
    OsdProgram_parent_osdSaveState(self);
    const char *c = OsdProgram_data(self)->command;
    if (c) osd_state_put_str(self, "OsdProgram", "command", c);
    else osd_state_remove(self, "OsdProgram", "command");
    return true;
}

void OsdProgram__osdRestoreProperties(osdo_object *self)
{
    OsdProgram_parent_osdRestoreProperties(self);
    char *c = NULL;
    free(OsdProgram_data(self)->command);
    OsdProgram_data(self)->command = osd_state_get_str(self, "OsdProgram", "command", &c) ? c : NULL;
}

/* Its Program page (OsdProgram.osdi). */
int64_t OsdProgram__osdQueryProgramType(osdo_object *self)
{
    int64_t ptype = 0;
    osd_state_get_int(self, "OsdProgram", "ptype", &ptype);
    return ptype;
}

/* Start at login: shown for an entry in Startup only */
bool OsdProgram__osdQueryInStartup(osdo_object *self)
{
    bool on = false;
    return osd_startup_query && osd_startup_query(self, &on);
}

bool OsdProgram__osdQueryStartAtLogin(osdo_object *self)
{
    bool on = false;
    return osd_startup_query && osd_startup_query(self, &on) && on;
}

char *OsdProgram__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Program");
}
