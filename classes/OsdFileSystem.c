#include "OsdFileSystem.h"

#include <stdlib.h>
#include <string.h>

#include "osd_classes.h"

void OsdFileSystem__osdUnInitData(osdo_object *self)
{
    free(OsdFileSystem_data(self)->path);
    OsdFileSystem_parent_osdUnInitData(self);
}

char *OsdFileSystem__osdQueryTitle(osdo_object *self)
{
    /* A title set by the user wins; otherwise the file name. */
    if (OsdObject_data(self)->title) return OsdFileSystem_parent_osdQueryTitle(self);
    const char *p = OsdFileSystem_data(self)->path;
    if (!p) return OsdFileSystem_parent_osdQueryTitle(self);
    const char *slash = strrchr(p, '/');
    return osd_strdup(slash && slash[1] ? slash + 1 : p);
}

char *OsdFileSystem__osdQueryPath(osdo_object *self)
{
    return osd_strdup(OsdFileSystem_data(self)->path);
}

bool OsdFileSystem__osdSetPath(osdo_object *self, const char *path)
{
    char *p = osd_strdup(path);
    if (path && !p) return false;
    free(OsdFileSystem_data(self)->path);
    OsdFileSystem_data(self)->path = p;
    return true;
}

char *OsdFileSystem__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("File System Object");
}
