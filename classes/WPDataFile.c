#include "WPDataFile.h"
#include "wps_magic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "wps_classes.h"
#include "wps_startup_hook.h"

char *WPDataFile__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    /* (Open With: wpsd makes Open a cascade of the type's programs) */
    return WPDataFile_parent_wpModifyPopupMenu(self, menu);
}

/* The type from the file name, through shared-mime-info's glob table
 * (loaded once, by wps_mime_init, before the loop starts). */
static struct { char *glob; char *type; int weight; bool cs; } *globs;
static size_t nglobs;

void wps_mime_init(const char *globs2)
{
    FILE *f = fopen(globs2 ? globs2 : "/usr/share/mime/globs2", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\r\n")] = 0;
        char *w = strtok(line, ":"), *type = strtok(NULL, ":"), *glob = strtok(NULL, ":"), *flags = strtok(NULL, ":");
        if (!w || !type || !glob || strncmp(glob, "*.", 2) != 0 || strpbrk(glob + 2, "*?[")) continue;
        globs = realloc(globs, (nglobs + 1) * sizeof *globs);
        globs[nglobs].glob = strdup(glob + 1);   /* ".png" */
        globs[nglobs].type = strdup(type);
        globs[nglobs].cs = flags && strstr(flags, "cs");   /* "*.C" is C++, "*.c" C */
        globs[nglobs++].weight = atoi(w);
    }
    if (f) fclose(f);
}

const char *wps_mime_for_name(const char *name)
{
    /* As the spec says: a match of the name's own case first (".c" is C,
     * ".C" C++), then one in any case; the longest suffix wins (".tar.gz"
     * over ".gz"), then weight. */
    size_t ln = strlen(name);
    for (int pass = 0; pass < 2; pass++) {
        const char *best = NULL;
        size_t bestlen = 0;
        int bestw = -1;
        for (size_t i = 0; i < nglobs; i++) {
            size_t lg = strlen(globs[i].glob);
            if (lg > ln) continue;
            if (pass == 0 ? strcmp(name + ln - lg, globs[i].glob) != 0
                          : globs[i].cs || strcasecmp(name + ln - lg, globs[i].glob) != 0) continue;
            if (lg > bestlen || (lg == bestlen && globs[i].weight > bestw)) {
                best = globs[i].type;
                bestlen = lg;
                bestw = globs[i].weight;
            }
        }
        if (best) return best;
    }
    return NULL;
}

char *WPDataFile__wpQueryMimeType(wpo_object *self)
{
    const char *p = WPFileSystem_data(self)->path;
    const char *slash = p ? strrchr(p, '/') : NULL;
    const char *t = p ? wps_mime_for_name(slash ? slash + 1 : p) : NULL;
    if (!t && p) t = wps_mime_sniff(p);   /* the name says nothing: its contents (an empty Data File is text) */
    return wps_strdup(t ? t : "application/octet-stream");
}

char *WPDataFile__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Data File");
}

/* ---- an executable's Program page (WPDataFile.wpi) */

bool (*wps_is_program)(const char *path);

bool WPDataFile__wpQueryIsProgram(wpo_object *self)
{
    const char *path = WPFileSystem_data(self)->path;
    return path && wps_is_program && wps_is_program(path);
}
