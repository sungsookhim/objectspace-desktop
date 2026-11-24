/*
 * File types for drops on programs (gui/shell): a file's type from its
 * name (shared-mime-info's glob table; no match: its contents, as the
 * service has it, classes/osd_magic.c; a folder: inode/directory), and whether a program
 * opens it - the types its desktop entry lists (MimeType=), a file's type
 * counting as each type it is a kind of (subclasses: text/x-csrc is
 * text/plain; aliases followed). A program with no list takes files only
 * if its command has a field code for them; an executable takes any.
 */
#define _GNU_SOURCE
#include "mime.h"
#include "osd_magic.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

struct glob { char *suffix, *type; int weight; };
struct pair { char *a, *b; };
static struct glob *globs;
static size_t nglobs;
static struct pair *subs, *aliases;
static size_t nsubs, naliases;
static bool loaded;

static void load_pairs(const char *path, struct pair **out, size_t *n)
{
    FILE *f = fopen(path, "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *sp = strchr(line, ' ');
        if (!sp || line[0] == '#') continue;
        *sp = 0;
        *out = realloc(*out, (*n + 1) * sizeof **out);
        (*out)[*n].a = strdup(line);
        (*out)[(*n)++].b = strdup(sp + 1);
    }
    if (f) fclose(f);
}

static void load(void)
{
    if (loaded) return;
    loaded = true;
    FILE *f = fopen("/usr/share/mime/globs2", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\r\n")] = 0;
        char *save = NULL, *w = strtok_r(line, ":", &save), *type = strtok_r(NULL, ":", &save), *glob = strtok_r(NULL, ":", &save);
        if (!w || !type || !glob || strncmp(glob, "*.", 2) != 0 || strpbrk(glob + 2, "*?[")) continue;
        globs = realloc(globs, (nglobs + 1) * sizeof *globs);
        globs[nglobs].suffix = strdup(glob + 1);   /* ".png" */
        globs[nglobs].type = strdup(type);
        globs[nglobs++].weight = atoi(w);
    }
    if (f) fclose(f);
    load_pairs("/usr/share/mime/subclasses", &subs, &nsubs);   /* "child parent" */
    load_pairs("/usr/share/mime/aliases", &aliases, &naliases);  /* "alias type" */
}


const char *mime_of_name(const char *name)
{
    load();
    const char *best = NULL;
    size_t bestlen = 0, ln = strlen(name);
    int bestw = -1;
    for (size_t i = 0; i < nglobs; i++) {
        size_t lg = strlen(globs[i].suffix);
        if (lg > ln || strcasecmp(name + ln - lg, globs[i].suffix) != 0) continue;
        if (lg > bestlen || (lg == bestlen && globs[i].weight > bestw)) { best = globs[i].type; bestlen = lg; bestw = globs[i].weight; }
    }
    return best;
}

const char *mime_of_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return "inode/directory";
    const char *name = strrchr(path, '/');
    const char *best = mime_of_name(name ? name + 1 : path);
    if (best) return best;
    const char *sniffed = osd_mime_sniff(path);   /* as the service does (classes/osd_magic.c) */
    return sniffed ? sniffed : "application/octet-stream";
}

static const char *unalias(const char *t)
{
    for (size_t i = 0; i < naliases; i++) if (!strcmp(aliases[i].a, t)) return aliases[i].b;
    return t;
}

/* Is type t (or one of the types it is a kind of) in the list? */
static bool covered(const char *t, const char *const *list, size_t n, int depth)
{
    if (depth > 8) return false;
    t = unalias(t);
    for (size_t i = 0; i < n; i++) {
        const char *l = unalias(list[i]);
        if (!strcmp(l, t) || !strcmp(l, "*/*") || !strcmp(l, "application/octet-stream")) return true;
        size_t sl = strlen(l);
        if (sl > 2 && !strcmp(l + sl - 2, "/*") && !strncmp(l, t, sl - 1)) return true;   /* e.g. image/ followed by a star */
    }
    for (size_t i = 0; i < nsubs; i++)
        if (!strcmp(subs[i].a, t) && covered(subs[i].b, list, n, depth + 1)) return true;
    /* every text type is a kind of text/plain (shared-mime-info's rule) */
    if (!strncmp(t, "text/", 5) && strcmp(t, "text/plain") && depth == 0 && covered("text/plain", list, n, depth + 1)) return true;
    return false;
}

bool program_opens(const char *program, const char *const *files, size_t nfiles)
{
    load();
    size_t pl = strlen(program);
    if (pl <= 8 || strcmp(program + pl - 8, ".desktop")) return true;   /* an executable: any */
    FILE *f = fopen(program, "r");
    if (!f) return true;   /* unknown: let the service decide */
    char line[8192], types[8192] = "", exec[4096] = "";
    bool in_entry = false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { in_entry = !strcmp(line, "[Desktop Entry]"); continue; }
        if (!in_entry) continue;
        if (!strncmp(line, "MimeType=", 9)) snprintf(types, sizeof types, "%s", line + 9);
        else if (!strncmp(line, "Exec=", 5)) snprintf(exec, sizeof exec, "%s", line + 5);
    }
    fclose(f);
    if (!types[0]) return strstr(exec, "%f") || strstr(exec, "%F") || strstr(exec, "%u") || strstr(exec, "%U");
    const char *list[256];
    size_t n = 0;
    for (char *save = NULL, *t = strtok_r(types, ";", &save); t && n < 256; t = strtok_r(NULL, ";", &save))
        if (*t) list[n++] = t;
    for (size_t i = 0; i < nfiles; i++)
        if (!covered(mime_of_file(files[i]), list, n, 0)) return false;
    return true;
}
