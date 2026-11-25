/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "osd_classes.h"

#include <stdlib.h>
#include <string.h>

#include "OsdAbstract.h"
#include "OsdDataFile.h"
#include "OsdFileSystem.h"
#include "OsdFolder.h"
#include "OsdObject.h"
#include "OsdProgram.h"
#include "OsdShadow.h"
#include "OsdSetup.h"
#include "OsdSystem.h"
#include "OsdSound.h"
#include "OsdMouse.h"
#include "OsdSchemePalette.h"
#include "OsdScheme.h"
#include "OsdFontPalette.h"
#include "OsdColorPalette.h"
#include "OsdLauncher.h"
#include "OsdTheBar.h"
#include "OsdMessages.h"
#include "OsdMinWinViewer.h"
#include "OsdRestoreDesktop.h"
#include "OsdCommandPrompt.h"
#include "OsdFind.h"
#include "OsdTrashCan.h"
#include "OsdComputer.h"
#include "OsdDrives.h"
#include "OsdDrive.h"
#include "OsdProgramsFolder.h"

char *osd_strdup(const char *s)
{
    return s ? strdup(s) : NULL;
}

char *osd_concat(const char *a, const char *b)
{
    size_t la = a ? strlen(a) : 0, lb = b ? strlen(b) : 0;
    char *r = malloc(la + lb + 1);
    if (!r) return NULL;
    if (la) memcpy(r, a, la);
    if (lb) memcpy(r + la, b, lb);
    r[la + lb] = 0;
    return r;
}

osdo_status osd_classes_register(void)
{
    osdo_status (*const regs[])(void) = {
        OsdObject_register, OsdFileSystem_register, OsdDataFile_register, OsdFolder_register,
        OsdAbstract_register, OsdProgram_register, OsdShadow_register,
        OsdSetup_register, OsdSystem_register, OsdSound_register, OsdMouse_register, OsdSchemePalette_register, OsdFontPalette_register, OsdColorPalette_register,
        OsdLauncher_register, OsdTheBar_register, OsdMessages_register,
        OsdScheme_register,
        OsdMinWinViewer_register, OsdRestoreDesktop_register, OsdCommandPrompt_register, OsdFind_register, OsdTrashCan_register, OsdComputer_register, OsdDrives_register, OsdDrive_register, OsdProgramsFolder_register,
    };
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        osdo_status st = regs[i]();
        if (st != OSDO_OK) return st;
    }
    return OSDO_OK;
}
