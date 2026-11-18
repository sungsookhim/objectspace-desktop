#include "WPDataFile.h"

#include <stdio.h>
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

/* The type from the file name, through shared-mime-info's glob table
 * (loaded once, by wps_mime_init, before the loop starts). */
static struct { char *glob; char *type; int weight; } *globs;
static size_t nglobs;

void wps_mime_init(const char *globs2)
{
    FILE *f = fopen(globs2 ? globs2 : "/usr/share/mime/globs2", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\r\n")] = 0;
        char *w = strtok(line, ":"), *type = strtok(NULL, ":"), *glob = strtok(NULL, ":");
        if (!w || !type || !glob || strncmp(glob, "*.", 2) != 0 || strpbrk(glob + 2, "*?[")) continue;
        globs = realloc(globs, (nglobs + 1) * sizeof *globs);
        globs[nglobs].glob = strdup(glob + 1);   /* ".png" */
        globs[nglobs].type = strdup(type);
        globs[nglobs++].weight = atoi(w);
    }
    if (f) fclose(f);
}

const char *wps_mime_for_name(const char *name)
{
    const char *best = NULL;
    size_t bestlen = 0;
    int bestw = -1;
    size_t ln = strlen(name);
    for (size_t i = 0; i < nglobs; i++) {
        size_t lg = strlen(globs[i].glob);
        if (lg > ln || strcasecmp(name + ln - lg, globs[i].glob) != 0) continue;
        /* The longest matching suffix wins (".tar.gz" over ".gz"), then weight. */
        if (lg > bestlen || (lg == bestlen && globs[i].weight > bestw)) {
            best = globs[i].type;
            bestlen = lg;
            bestw = globs[i].weight;
        }
    }
    return best;
}

char *WPDataFile__wpQueryMimeType(wpo_object *self)
{
    const char *p = WPFileSystem_data(self)->path;
    const char *slash = p ? strrchr(p, '/') : NULL;
    const char *t = p ? wps_mime_for_name(slash ? slash + 1 : p) : NULL;
    return wps_strdup(t ? t : "application/octet-stream");
}

char *WPDataFile__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Data File");
}
