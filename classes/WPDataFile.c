#include "WPDataFile.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "wps_classes.h"

char *WPDataFile__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    char *base = WPDataFile_parent_wpModifyPopupMenu(self, menu);
    char *r = wps_concat(base, "10\tOpen With\n");
    free(base);
    return r;
}

char *WPDataFile__wpQueryMimeType(wpo_object *self)
{
    /* By extension only in M1; shared-mime-info in M5. */
    static const struct { const char *ext, *type; } map[] = {
        { ".txt", "text/plain" }, { ".png", "image/png" }, { ".odt", "application/vnd.oasis.opendocument.text" },
    };
    const char *p = WPFileSystem_data(self)->path;
    const char *dot = p ? strrchr(p, '.') : NULL;
    for (size_t i = 0; dot && i < sizeof map / sizeof map[0]; i++)
        if (strcasecmp(dot, map[i].ext) == 0) return wps_strdup(map[i].type);
    return wps_strdup("application/octet-stream");
}

char *WPDataFile__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Data File");
}
