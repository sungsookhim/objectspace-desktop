#include "WPFileSystem.h"

#include <stdlib.h>
#include <string.h>

#include "wps_classes.h"

void WPFileSystem__wpUnInitData(wpo_object *self)
{
    free(WPFileSystem_data(self)->path);
    WPFileSystem_parent_wpUnInitData(self);
}

char *WPFileSystem__wpQueryTitle(wpo_object *self)
{
    /* A title set by the user wins; otherwise the file name. */
    if (WPObject_data(self)->title) return WPFileSystem_parent_wpQueryTitle(self);
    const char *p = WPFileSystem_data(self)->path;
    if (!p) return WPFileSystem_parent_wpQueryTitle(self);
    const char *slash = strrchr(p, '/');
    return wps_strdup(slash && slash[1] ? slash + 1 : p);
}

char *WPFileSystem__wpQueryPath(wpo_object *self)
{
    return wps_strdup(WPFileSystem_data(self)->path);
}

bool WPFileSystem__wpSetPath(wpo_object *self, const char *path)
{
    char *p = wps_strdup(path);
    if (path && !p) return false;
    free(WPFileSystem_data(self)->path);
    WPFileSystem_data(self)->path = p;
    return true;
}

char *WPFileSystem__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("File System Object");
}
