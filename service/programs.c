/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * Programs: the keeper of the Programs folder (see programs.h).
 *
 * A sync reads the entries and the record of what was made on a worker,
 * decides, removes the shadows of programs gone (or moved to another
 * category), then makes the new shadows on the loop thread
 * (osd_shadow_create) and writes the record when the last is made. One
 * sync at a time; a change during one starts another after it.
 */
#define _GNU_SOURCE
#include "programs.h"

#include <dirent.h>
#include <poll.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "env.h"
#include "objects.h"
#include "shadows.h"
#include "listing.h"

static osdo_loop *loop;
static char folder[PATH_MAX], record[PATH_MAX];
static bool syncing, again, waiting;
static int ino = -1;

/* ------------------------------------------------------------ categories */

static const char *const cat_map[][2] = {
    { "AudioVideo", "Multimedia" }, { "Audio", "Multimedia" }, { "Video", "Multimedia" },
    { "Development", "Development" }, { "Education", "Office" }, { "Game", "Games" },
    { "Graphics", "Graphics" }, { "Network", "Internet" }, { "Office", "Office" },
    { "Science", "Office" }, { "Settings", "Settings" }, { "System", "System" },
    { "Utility", "Accessories" },
};

const char *osd_program_category(const char *categories)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", categories ? categories : "");
    char *save = NULL;
    for (char *c = strtok_r(buf, ";", &save); c; c = strtok_r(NULL, ";", &save))
        for (size_t i = 0; i < sizeof cat_map / sizeof cat_map[0]; i++)
            if (!strcmp(c, cat_map[i][0])) return cat_map[i][1];
    return "Other";
}

/* --------------------------------------------------------- the entries */

struct entry { char id[256], path[PATH_MAX], cat[32]; bool shown; };
struct made { char id[256], cat[32], path[PATH_MAX], link[PATH_MAX]; bool keep, gone; };

struct sync {
    struct entry *e;
    size_t ne;
    struct made *m;
    size_t nm;
    /* to do on the loop thread */
    struct { char folder[PATH_MAX], id[OSD_ID_LEN + 1]; } *removed;
    size_t nremoved;
    size_t *create;          /* entries to make shadows of */
    size_t ncreate, pending;
};

static bool in_list(const char *list, const char *const *desks, size_t nd)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", list);
    char *save = NULL;
    for (char *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save))
        for (size_t i = 0; i < nd; i++) if (!strcasecmp(t, desks[i])) return true;
    return false;
}

static bool on_path(const char *prog)
{
    if (strchr(prog, '/')) return access(prog, X_OK) == 0;
    const char *p = getenv("PATH");
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", p && *p ? p : "/usr/local/bin:/usr/bin:/bin");
    char *save = NULL;
    for (char *d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        char f[PATH_MAX];
        snprintf(f, sizeof f, "%s/%s", d, prog);
        if (access(f, X_OK) == 0) return true;
    }
    return false;
}

/* Is this entry shown, and in which category? */
static bool read_entry(const char *path, char *cat, size_t cn)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[4096], type[32] = "", cats[1024] = "", only[512] = "", not[512] = "", tryexec[PATH_MAX] = "";
    bool in = false, nodisplay = false, hidden = false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { in = !strcmp(line, "[Desktop Entry]"); continue; }
        if (!in) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "Type")) snprintf(type, sizeof type, "%s", v);
        else if (!strcmp(k, "Categories")) snprintf(cats, sizeof cats, "%s", v);
        else if (!strcmp(k, "OnlyShowIn")) snprintf(only, sizeof only, "%s", v);
        else if (!strcmp(k, "NotShowIn")) snprintf(not, sizeof not, "%s", v);
        else if (!strcmp(k, "TryExec")) snprintf(tryexec, sizeof tryexec, "%s", v);
        else if (!strcmp(k, "NoDisplay")) nodisplay = !strcmp(v, "true");
        else if (!strcmp(k, "Hidden")) hidden = !strcmp(v, "true");
    }
    fclose(f);
    if (strcmp(type, "Application") || nodisplay || hidden) return false;
    /* this desktop: the session's (XDG_CURRENT_DESKTOP), and Objectspace */
    const char *desks[8];
    size_t nd = 0;
    char xd[256];
    const char *e = getenv("OSD_PROGRAMS_DESKTOP");
    if (!e) e = getenv("XDG_CURRENT_DESKTOP");
    snprintf(xd, sizeof xd, "%s", e && *e ? e : "GNOME");
    char *save = NULL;
    for (char *t = strtok_r(xd, ":", &save); t && nd < 7; t = strtok_r(NULL, ":", &save)) desks[nd++] = t;
    desks[nd++] = "Objectspace";
    if (only[0] && !in_list(only, desks, nd)) return false;
    if (not[0] && in_list(not, desks, nd)) return false;
    if (tryexec[0] && !on_path(tryexec)) return false;
    snprintf(cat, cn, "%s", osd_program_category(cats));
    return true;
}

static void scan_dir(struct sync *s, const char *dir, const char *prefix, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *de; (de = readdir(d));) {
        if (de->d_name[0] == '.') continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 2) {   /* kde4/x.desktop is kde4-x.desktop */
                char pre[256];
                snprintf(pre, sizeof pre, "%s%s-", prefix, de->d_name);
                scan_dir(s, p, pre, depth + 1);
            }
            continue;
        }
        size_t l = strlen(de->d_name);
        if (l < 9 || strcmp(de->d_name + l - 8, ".desktop")) continue;
        char id[256];
        snprintf(id, sizeof id, "%s%s", prefix, de->d_name);
        bool seen = false;   /* an earlier folder has it: that one wins (even hidden) */
        for (size_t i = 0; i < s->ne && !seen; i++) seen = !strcmp(s->e[i].id, id);
        if (seen) continue;
        s->e = realloc(s->e, (s->ne + 1) * sizeof *s->e);
        struct entry *en = &s->e[s->ne++];
        memset(en, 0, sizeof *en);
        snprintf(en->id, sizeof en->id, "%s", id);
        snprintf(en->path, sizeof en->path, "%s", p);
        en->shown = read_entry(p, en->cat, sizeof en->cat);
    }
    closedir(d);
}

/* The application folders, most important first. */
static size_t app_dirs(char dirs[][PATH_MAX], size_t max)
{
    size_t n = 0;
    const char *dh = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (dh && *dh) snprintf(dirs[n++], PATH_MAX, "%s/applications", dh);
    else snprintf(dirs[n++], PATH_MAX, "%s/.local/share/applications", home ? home : "");
    const char *dd = getenv("XDG_DATA_DIRS");
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", dd && *dd ? dd : "/usr/local/share:/usr/share");
    char *save = NULL;
    for (char *t = strtok_r(buf, ":", &save); t && n < max; t = strtok_r(NULL, ":", &save)) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/applications", t);
        bool dup = false;
        for (size_t i = 0; i < n; i++) dup |= !strcmp(dirs[i], p);
        if (!dup) snprintf(dirs[n++], PATH_MAX, "%s", p);
    }
    return n;
}

/* ---------------------------------------------------------- the record */

static void read_record(struct sync *s)
{
    FILE *f = fopen(record, "r");
    char line[PATH_MAX * 2 + 512];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#') continue;
        char *fld[4];
        int k = 0;
        char *save = NULL;
        for (char *t = strtok_r(line, "\t", &save); t && k < 4; t = strtok_r(NULL, "\t", &save)) fld[k++] = t;
        if (k < 3) continue;
        s->m = realloc(s->m, (s->nm + 1) * sizeof *s->m);
        struct made *m = &s->m[s->nm++];
        memset(m, 0, sizeof *m);
        snprintf(m->id, sizeof m->id, "%s", fld[0]);
        snprintf(m->cat, sizeof m->cat, "%s", fld[1]);
        snprintf(m->path, sizeof m->path, "%s", fld[2]);
        if (k > 3) snprintf(m->link, sizeof m->link, "%s", fld[3]);
    }
    if (f) fclose(f);
}

static void write_record(struct sync *s)
{
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.new", record);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# Programs osdd made (desktop file ID, category, entry, shadow): a shadow\n"
               "# deleted by the user is not made again while its line is here.\n");
    for (size_t i = 0; i < s->nm; i++)
        if (s->m[i].keep) fprintf(f, "%s\t%s\t%s\t%s\n", s->m[i].id, s->m[i].cat, s->m[i].path, s->m[i].link);
    fclose(f);
    rename(tmp, record);
}

static bool link_exists(const char *link)
{
    if (!link[0]) return false;
    if (link[0] == '/') return access(link, F_OK) == 0;
    osd_loaded ld;
    osd_load(link, false, &ld);
    bool ok = !ld.err;
    osd_loaded_free(&ld);
    return ok;
}

/* ---------------------------------------------------------- the sync */

static void sync_work(void *arg)
{
    struct sync *s = arg;
    char dirs[16][PATH_MAX];
    size_t nd = app_dirs(dirs, 16);
    for (size_t i = 0; i < nd; i++) scan_dir(s, dirs[i], "", 0);
    read_record(s);
    for (size_t k = 0; k < s->nm; k++) {
        struct made *m = &s->m[k];
        struct entry *en = NULL;
        for (size_t i = 0; i < s->ne && !en; i++) if (!strcmp(s->e[i].id, m->id)) en = &s->e[i];
        bool exists = link_exists(m->link);
        if (en && en->shown && !strcmp(en->cat, m->cat) && !strcmp(en->path, m->path)) {
            m->keep = true;   /* as made; or deleted by the user (kept so: not made again) */
            en->shown = false;   /* (nothing to make) */
            continue;
        }
        /* gone, hidden, or now another category or entry: its shadow goes */
        if (exists) {
            if (!strncmp(m->link, "vobj:", 5)) {
                if (osd_central_delete(m->link + 5) == 0) {
                    s->removed = realloc(s->removed, (s->nremoved + 1) * sizeof *s->removed);
                    snprintf(s->removed[s->nremoved].folder, PATH_MAX, "%s/%s", folder, m->cat);
                    snprintf(s->removed[s->nremoved].id, OSD_ID_LEN + 1, "%s", m->link + 5);
                    s->nremoved++;
                }
            } else unlink(m->link);
        }
        /* moved to another category or entry: made again there - unless
         * the user had deleted it (still deleted: the record follows) */
        if (en && en->shown && !exists) {
            en->shown = false;
            m->keep = true;
            m->link[0] = 0;
            snprintf(m->cat, sizeof m->cat, "%s", en->cat);
            snprintf(m->path, sizeof m->path, "%s", en->path);
        }
    }
    for (size_t i = 0; i < s->ne; i++) {
        if (!s->e[i].shown) continue;
        char cf[PATH_MAX + 40];
        snprintf(cf, sizeof cf, "%s/%s", folder, s->e[i].cat);
        mkdir(cf, 0755);
        s->create = realloc(s->create, (s->ncreate + 1) * sizeof *s->create);
        s->create[s->ncreate++] = i;
    }
}

static void sync_free(struct sync *s)
{
    free(s->e);
    free(s->m);
    free(s->removed);
    free(s->create);
    free(s);
}

static void sync_finish(struct sync *s)
{
    write_record(s);
    sync_free(s);
    syncing = false;
    if (again) { again = false; osd_programs_sync(); }
}

struct one { struct sync *s; size_t entry; };

static void made_one(void *arg, const char *link, const char *error)
{
    struct one *o = arg;
    struct sync *s = o->s;
    struct entry *en = &s->e[o->entry];
    if (link) {
        s->m = realloc(s->m, (s->nm + 1) * sizeof *s->m);
        struct made *m = &s->m[s->nm++];
        memset(m, 0, sizeof *m);
        snprintf(m->id, sizeof m->id, "%s", en->id);
        snprintf(m->cat, sizeof m->cat, "%s", en->cat);
        snprintf(m->path, sizeof m->path, "%s", en->path);
        snprintf(m->link, sizeof m->link, "%s", link);
        m->keep = true;
    } else {
        fprintf(stderr, "osdd: programs: no shadow of %s: %s\n", en->path, error ? error : "?");
    }
    free(o);
    if (--s->pending == 0) sync_finish(s);
}

static void sync_done(void *arg)
{
    struct sync *s = arg;
    if (s->ncreate) osd_listing_invalidate(folder);   /* category folders made (perhaps just after it was read) */
    for (size_t i = 0; i < s->nremoved; i++) {
        osd_central_forget(s->removed[i].id);
        if (osd_on_central_change) osd_on_central_change(s->removed[i].folder, s->removed[i].id, 'd');
    }
    if (!s->ncreate) { sync_finish(s); return; }
    s->pending = s->ncreate;
    for (size_t k = 0; k < s->ncreate; k++) {
        struct entry *en = &s->e[s->create[k]];
        char cf[PATH_MAX + 40];
        snprintf(cf, sizeof cf, "%s/%s", folder, en->cat);
        struct one *o = calloc(1, sizeof *o);
        o->s = s;
        o->entry = s->create[k];
        osd_shadow_create(en->path, cf, made_one, o);
    }
}

void osd_programs_sync(void)
{
    if (!loop) return;
    struct stat st;
    if (stat(folder, &st) != 0 || !S_ISDIR(st.st_mode)) return;   /* not made (yet), or removed by the user */
    if (syncing) { again = true; return; }
    syncing = true;
    osdo_defer(loop, "programs", sync_work, sync_done, calloc(1, sizeof(struct sync)));
}

/* -------------------------------------------------------- watching */

static void settle_work(void *arg) { (void)arg; usleep(1000 * 1000); }   /* a package installs many files */
static void settle_done(void *arg) { (void)arg; waiting = false; osd_programs_sync(); }

static void watch_dirs(void)
{
    char dirs[16][PATH_MAX];
    size_t nd = app_dirs(dirs, 16);
    for (size_t i = 0; i < nd; i++)
        inotify_add_watch(ino, dirs[i], IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ONLYDIR);
}

static void on_inotify(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    char buf[8192];
    while (read(fd, buf, sizeof buf) > 0) {}
    if (!waiting) { waiting = true; osdo_defer(loop, "programs settle", settle_work, settle_done, NULL); }
}

void osd_programs_init(osdo_loop *l, const char *programs_folder, const char *state_dir)
{
    loop = l;
    snprintf(folder, sizeof folder, "%s", programs_folder);
    snprintf(record, sizeof record, "%s/programs-made", state_dir);
    /* the user's own application folder, so it can be watched */
    char dirs[16][PATH_MAX];
    if (app_dirs(dirs, 16)) osd_mkdirs(dirs[0], 0755);
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino >= 0) {
        watch_dirs();
        osdo_loop_watch_fd(loop, ino, POLLIN, on_inotify, NULL);
    }
    osd_programs_sync();
}

void osd_programs_shutdown(void)
{
    if (ino >= 0) { osdo_loop_unwatch_fd(loop, ino); close(ino); ino = -1; }
    loop = NULL;
}
