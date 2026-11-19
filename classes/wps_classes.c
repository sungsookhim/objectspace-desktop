#include "wps_classes.h"

#include <stdlib.h>
#include <string.h>

#include "WPAbstract.h"
#include "WPDataFile.h"
#include "WPFileSystem.h"
#include "WPFolder.h"
#include "WPObject.h"
#include "WPProgram.h"
#include "WPShadow.h"
#include "WPSetup.h"
#include "WPSystem.h"
#include "WPSound.h"
#include "WPMouse.h"
#include "WPSchemePalette.h"

char *wps_strdup(const char *s)
{
    return s ? strdup(s) : NULL;
}

char *wps_concat(const char *a, const char *b)
{
    size_t la = a ? strlen(a) : 0, lb = b ? strlen(b) : 0;
    char *r = malloc(la + lb + 1);
    if (!r) return NULL;
    if (la) memcpy(r, a, la);
    if (lb) memcpy(r + la, b, lb);
    r[la + lb] = 0;
    return r;
}

wpo_status wps_classes_register(void)
{
    wpo_status (*const regs[])(void) = {
        WPObject_register, WPFileSystem_register, WPDataFile_register, WPFolder_register,
        WPAbstract_register, WPProgram_register, WPShadow_register,
        WPSetup_register, WPSystem_register, WPSound_register, WPMouse_register, WPSchemePalette_register,
    };
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        wpo_status st = regs[i]();
        if (st != WPO_OK) return st;
    }
    return WPO_OK;
}
