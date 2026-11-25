/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Thumbnails (see thumbs.h). */
#define _GNU_SOURCE
#include "thumbs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "md5.h"
#include "mime.h"

extern char **environ;

/* ------------------------------------------------------------ names */

static void uri_of(const char *path, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = snprintf(out, n, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p && o + 4 < n; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~/", *p)) out[o++] = (char)*p;
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

static void cache_root(char *out, size_t n)
{
    const char *c = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    if (c && *c) snprintf(out, n, "%s/thumbnails", c);
    else snprintf(out, n, "%s/.cache/thumbnails", h ? h : "");
}

/* The standard's sizes: the bucket for px, and its pixels. */
static const char *bucket(int px, int *size)
{
    if (px <= 128) { *size = 128; return "normal"; }
    if (px <= 256) { *size = 256; return "large"; }
    *size = 512;
    return "x-large";
}

static void mkdirs(const char *p)
{
    char t[PATH_MAX];
    snprintf(t, sizeof t, "%s", p);
    for (char *s = t + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(t, 0700); *s = '/'; }
    mkdir(t, 0700);
}

/* ------------------------------------------------------------ PNG text */

/* A PNG's tEXt value for key (out ""), true if it is a PNG. */
static bool png_text(const char *file, const char *key, char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen(file, "rb");
    if (!f) return false;
    unsigned char sig[8], hd[8];
    bool png = fread(sig, 1, 8, f) == 8 && !memcmp(sig, "\x89PNG\r\n\x1a\n", 8);
    while (png && fread(hd, 1, 8, f) == 8) {
        uint32_t len = (uint32_t)hd[0] << 24 | hd[1] << 16 | hd[2] << 8 | hd[3];
        if (!memcmp(hd + 4, "IDAT", 4) || !memcmp(hd + 4, "IEND", 4) || len > 1 << 20) break;
        if (!memcmp(hd + 4, "tEXt", 4)) {
            char *d = malloc(len + 1);
            if (d && fread(d, 1, len, f) == len) {
                d[len] = 0;
                size_t kl = strlen(d);
                if (!strcmp(d, key) && kl < len) snprintf(out, n, "%.*s", (int)(len - kl - 1), d + kl + 1);
            }
            free(d);
            fseek(f, 4, SEEK_CUR);
        } else fseek(f, (long)len + 4, SEEK_CUR);
    }
    fclose(f);
    return png;
}

static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

static void write_text_chunk(FILE *o, const char *key, const char *val)
{
    size_t kl = strlen(key), vl = strlen(val), len = kl + 1 + vl;
    unsigned char *c = malloc(len + 12);
    put32(c, (uint32_t)len);
    memcpy(c + 4, "tEXt", 4);
    memcpy(c + 8, key, kl + 1);
    memcpy(c + 8 + kl + 1, val, vl);
    put32(c + 8 + len, (uint32_t)crc32(0, c + 4, (uInt)(len + 4)));
    fwrite(c, 1, len + 12, o);
    free(c);
}

/* in (a PNG) to out, with the standard's text after its header. */
static bool png_with_text(const char *in, const char *out, const char *uri, int64_t mtime)
{
    FILE *f = fopen(in, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = sz > 33 && sz < 64 << 20 ? malloc((size_t)sz) : NULL;
    bool ok = d && fread(d, 1, (size_t)sz, f) == (size_t)sz && !memcmp(d, "\x89PNG\r\n\x1a\n", 8) && !memcmp(d + 12, "IHDR", 4);
    fclose(f);
    if (!ok) { free(d); return false; }
    char tmp[PATH_MAX + 16], mt[32];
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", out, (int)getpid());
    FILE *o = fopen(tmp, "wb");
    if (!o) { free(d); return false; }
    fchmod(fileno(o), 0600);
    fwrite(d, 1, 33, o);   /* the signature and IHDR (8 + 25) */
    snprintf(mt, sizeof mt, "%lld", (long long)mtime);
    write_text_chunk(o, "Thumb::URI", uri);
    write_text_chunk(o, "Thumb::MTime", mt);
    write_text_chunk(o, "Software", "Objectspace");
    fwrite(d + 33, 1, (size_t)sz - 33, o);
    free(d);
    ok = fclose(o) == 0 && rename(tmp, out) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

/* ------------------------------------------------------------ thumbnailers */

struct tn { char *mime, *exec; };
static struct tn *tns;
static size_t ntns;
static bool tns_read;

/* A program, by its path or found on PATH. */
static bool on_path(const char *prog)
{
    if (strchr(prog, '/')) return access(prog, X_OK) == 0;
    const char *path = getenv("PATH");
    char *copy = strdup(path && *path ? path : "/usr/local/bin:/usr/bin");
    bool found = false;
    for (char *save = NULL, *d = strtok_r(copy, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", d, prog);
        found = access(full, X_OK) == 0;
    }
    free(copy);
    return found;
}

static void read_thumbnailer(const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f) return;
    char line[2048], exec[1024] = "", mimes[1024] = "", tryexec[256] = "";
    bool in = false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { in = !strcmp(line, "[Thumbnailer Entry]"); continue; }
        if (!in) continue;
        if (!strncmp(line, "Exec=", 5)) snprintf(exec, sizeof exec, "%s", line + 5);
        else if (!strncmp(line, "MimeType=", 9)) snprintf(mimes, sizeof mimes, "%s", line + 9);
        else if (!strncmp(line, "TryExec=", 8)) snprintf(tryexec, sizeof tryexec, "%s", line + 8);
    }
    fclose(f);
    if (!exec[0] || !mimes[0]) return;
    if (tryexec[0] && !on_path(tryexec)) return;   /* (not installed) */
    for (char *save = NULL, *m = strtok_r(mimes, ";", &save); m; m = strtok_r(NULL, ";", &save)) {
        if (!*m) continue;
        tns = realloc(tns, (ntns + 1) * sizeof *tns);
        tns[ntns].mime = strdup(m);
        tns[ntns++].exec = strdup(exec);
    }
}

static void read_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 12 || strcmp(e->d_name + l - 12, ".thumbnailer")) continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        read_thumbnailer(p);
    }
    closedir(d);
}

static void read_thumbnailers(void)
{
    if (tns_read) return;
    tns_read = true;
    char p[PATH_MAX];
    const char *dh = getenv("XDG_DATA_HOME"), *h = getenv("HOME"), *dd = getenv("XDG_DATA_DIRS");
    if (dh && *dh) snprintf(p, sizeof p, "%s/thumbnailers", dh);
    else snprintf(p, sizeof p, "%s/.local/share/thumbnailers", h ? h : "");
    read_dir(p);   /* (the user's first: they win) */
    char *dirs = strdup(dd && *dd ? dd : "/usr/local/share:/usr/share");
    for (char *save = NULL, *x = strtok_r(dirs, ":", &save); x; x = strtok_r(NULL, ":", &save)) {
        snprintf(p, sizeof p, "%s/thumbnailers", x);
        read_dir(p);
    }
    free(dirs);
}

static const char *exec_for(const char *mime)
{
    read_thumbnailers();
    for (size_t i = 0; mime && i < ntns; i++) if (!strcmp(tns[i].mime, mime)) return tns[i].exec;
    return NULL;
}

bool thumbs_possible(const char *path)
{
    const char *m = mime_of_name(path);
    return m && exec_for(m);
}

/* ------------------------------------------------------------ the cache in memory */

#define MEM 300
static struct mem { char *key; cairo_surface_t *s; unsigned long used; } mem[MEM];
static unsigned long use_n;

static cairo_surface_t *mem_get(const char *key)
{
    for (int i = 0; i < MEM; i++)
        if (mem[i].key && !strcmp(mem[i].key, key)) { mem[i].used = ++use_n; return mem[i].s; }
    return NULL;
}

static void mem_put(const char *key, cairo_surface_t *s)
{
    int at = 0;
    for (int i = 0; i < MEM; i++) {
        if (!mem[i].key) { at = i; break; }
        if (mem[i].used < mem[at].used) at = i;
    }
    free(mem[at].key);
    if (mem[at].s) cairo_surface_destroy(mem[at].s);
    mem[at].key = strdup(key);
    mem[at].s = s;
    mem[at].used = ++use_n;
}

/* Failed or being made (no second job): by key. */
#define KNOWN 1024
static char *failed[KNOWN];
static size_t nfailed;

static bool is_failed(const char *key)
{
    for (size_t i = 0; i < nfailed && i < KNOWN; i++) if (failed[i] && !strcmp(failed[i], key)) return true;
    return false;
}

static void set_failed(const char *key)
{
    size_t i = nfailed++ % KNOWN;
    free(failed[i]);
    failed[i] = strdup(key);
}

/* ------------------------------------------------------------ jobs */

#define RUNNING 2
#define QUEUE 200
#define LIMIT_MS 10000

struct job {
    char key[PATH_MAX + 64], path[PATH_MAX], out[PATH_MAX + 64], tmp[PATH_MAX + 64], uri[PATH_MAX * 3 + 16];
    int64_t mtime;
    int size;
    pid_t pid;
    int fd;
    double started;
};

static ui_app *app;
static void (*on_changed)(void);
static struct job *queue[QUEUE];
static size_t nqueue;
static struct job *running[RUNNING];
static int timer_fd = -1;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static bool queued(const char *key)
{
    for (size_t i = 0; i < nqueue; i++) if (!strcmp(queue[i]->key, key)) return true;
    for (int i = 0; i < RUNNING; i++) if (running[i] && !strcmp(running[i]->key, key)) return true;
    return false;
}

static void start_next(void);

static void finished(struct job *j, bool ok)
{
    for (int i = 0; i < RUNNING; i++) if (running[i] == j) running[i] = NULL;
    if (j->fd >= 0) { ui_app_unwatch_fd(app, j->fd); close(j->fd); }
    if (ok && png_with_text(j->tmp, j->out, j->uri, j->mtime)) {
        cairo_surface_t *s = cairo_image_surface_create_from_png(j->out);
        if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) mem_put(j->key, s);
        else { cairo_surface_destroy(s); set_failed(j->key); }
    } else set_failed(j->key);
    unlink(j->tmp);
    free(j);
    if (on_changed) on_changed();
    start_next();
}

static void child_done(void *arg, int fd, short rev)
{
    (void)fd; (void)rev;
    struct job *j = arg;
    int st = 0;
    bool ok = waitpid(j->pid, &st, WNOHANG) == j->pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    struct stat sb;
    ok = ok && stat(j->tmp, &sb) == 0 && sb.st_size > 0;
    finished(j, ok);
}

static void on_timer(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    uint64_t x;
    if (read(fd, &x, sizeof x) < 0) {}
    bool any = false;
    for (int i = 0; i < RUNNING; i++) {
        struct job *j = running[i];
        if (!j) continue;
        any = true;
        if (now_ms() - j->started > LIMIT_MS) {   /* too long: stopped, failed */
            kill(j->pid, SIGKILL);
            waitpid(j->pid, NULL, 0);
            finished(j, false);
        }
    }
    if (!any && !nqueue) {
        struct itimerspec off = { 0 };
        timerfd_settime(timer_fd, 0, &off, NULL);
    }
}

/* Exec with %i (the file), %u (its URI), %o (the output), %s (the size). */
static bool spawn(struct job *j, const char *exec)
{
    char *argv[32], buf[4096];
    size_t argc = 0, b = 0;
    char *p = strdup(exec);
    for (char *save = NULL, *t = strtok_r(p, " \t", &save); t && argc < 31; t = strtok_r(NULL, " \t", &save)) {
        char *start = buf + b;
        for (const char *c = t; *c && b < sizeof buf - 1; c++) {
            const char *rep = NULL;
            char sz[16];
            if (c[0] == '%' && c[1]) {
                c++;
                if (*c == 'i') rep = j->path;
                else if (*c == 'u') rep = j->uri;
                else if (*c == 'o') rep = j->tmp;
                else if (*c == 's') { snprintf(sz, sizeof sz, "%d", j->size); rep = sz; }
                else if (*c == '%') rep = "%";
                else rep = "";
                size_t l = strlen(rep);
                if (b + l >= sizeof buf - 1) break;
                memcpy(buf + b, rep, l);
                b += l;
            } else buf[b++] = *c;
        }
        buf[b++] = 0;
        argv[argc++] = start;
    }
    argv[argc] = NULL;
    bool ok = false;
    if (argc) {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        ok = posix_spawnp(&j->pid, argv[0], &fa, NULL, argv, environ) == 0;
        posix_spawn_file_actions_destroy(&fa);
    }
    free(p);
    if (!ok) return false;
    j->fd = (int)syscall(SYS_pidfd_open, j->pid, 0);
    if (j->fd < 0) { waitpid(j->pid, NULL, 0); return false; }
    j->started = now_ms();
    ui_app_watch_fd(app, j->fd, POLLIN, child_done, j);
    return true;
}

static void start_next(void)
{
    for (int i = 0; i < RUNNING && nqueue; i++) {
        if (running[i]) continue;
        struct job *j = queue[--nqueue];   /* (the last asked first: what is on screen now) */
        const char *exec = exec_for(mime_of_name(j->path));
        j->fd = -1;
        if (!exec || !spawn(j, exec)) { set_failed(j->key); free(j); i--; continue; }
        running[i] = j;
        if (timer_fd < 0) {
            timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            if (timer_fd >= 0) ui_app_watch_fd(app, timer_fd, POLLIN, on_timer, NULL);
        }
        struct itimerspec it = { { 1, 0 }, { 1, 0 } };
        if (timer_fd >= 0) timerfd_settime(timer_fd, 0, &it, NULL);
    }
}

/* ------------------------------------------------------------ asking */

void thumbs_init(ui_app *a, void (*changed)(void))
{
    app = a;
    on_changed = changed;
}

/* A thumbnail file, if it is valid for mtime: loaded. */
static cairo_surface_t *load_valid(const char *file, int64_t mtime, bool check)
{
    char mt[32];
    if (!png_text(file, "Thumb::MTime", mt, sizeof mt)) return NULL;
    if (check && (!mt[0] || strtoll(mt, NULL, 10) != mtime)) return NULL;
    cairo_surface_t *s = cairo_image_surface_create_from_png(file);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); return NULL; }
    return s;
}

/* The folder's own exceptions (.osd-thumbnails): the picture for name. */
static cairo_surface_t *exception_for(const char *dir, const char *name)
{
    char f[PATH_MAX + 32], line[PATH_MAX * 2];
    snprintf(f, sizeof f, "%s/.osd-thumbnails", dir);
    FILE *fp = fopen(f, "r");
    if (!fp) return NULL;
    cairo_surface_t *s = NULL;
    while (!s && fgets(line, sizeof line, fp)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab || line[0] == '#') continue;
        *tab = 0;
        if (strcmp(line, name)) continue;
        char pic[PATH_MAX * 2];
        if (tab[1] == '/') snprintf(pic, sizeof pic, "%s", tab + 1);
        else snprintf(pic, sizeof pic, "%s/%s", dir, tab + 1);
        s = cairo_image_surface_create_from_png(pic);
        if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); s = NULL; }
    }
    fclose(fp);
    return s;
}

cairo_surface_t *thumbs_get(const char *path, int64_t mtime, int px)
{
    if (!path || path[0] != '/') return NULL;
    int size;
    const char *bk = bucket(px, &size);
    char key[PATH_MAX + 64];
    snprintf(key, sizeof key, "%s|%lld|%s", path, (long long)mtime, bk);
    cairo_surface_t *s = mem_get(key);
    if (s || is_failed(key) || queued(key)) return s;
    char dir[PATH_MAX], name[NAME_MAX + 1];
    const char *sl = strrchr(path, '/');
    snprintf(dir, sizeof dir, "%.*s", (int)(sl - path), path);
    snprintf(name, sizeof name, "%s", sl + 1);
    /* 3. the folder's exceptions, first: chosen by hand for this one */
    if ((s = exception_for(dir[0] ? dir : "/", name))) { mem_put(key, s); return s; }
    /* 1. the user's shared cache */
    char uri[PATH_MAX * 3 + 16], md5[33], root[PATH_MAX], file[PATH_MAX + 128];
    uri_of(path, uri, sizeof uri);
    md5_hex(uri, md5);
    cache_root(root, sizeof root);
    snprintf(file, sizeof file, "%s/%s/%s.png", root, bk, md5);
    if ((s = load_valid(file, mtime, true))) { mem_put(key, s); return s; }
    /* 2. the folder's shared repository (removable media): by the file's name */
    char nmd5[33], shared[PATH_MAX + 128];
    md5_hex(name, nmd5);
    snprintf(shared, sizeof shared, "%s/.sh_thumbnails/%s/%s.png", dir, size > 128 ? "large" : "normal", nmd5);
    if ((s = load_valid(shared, mtime, true))) { mem_put(key, s); return s; }
    /* made: asked of a thumbnailer */
    if (!thumbs_possible(path)) { set_failed(key); return NULL; }
    if (nqueue == QUEUE) {   /* (full: the oldest asked dropped; asked again when shown) */
        free(queue[0]);
        memmove(queue, queue + 1, (QUEUE - 1) * sizeof *queue);
        nqueue--;
    }
    struct job *j = calloc(1, sizeof *j);
    snprintf(j->key, sizeof j->key, "%s", key);
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->uri, sizeof j->uri, "%s", uri);
    snprintf(j->out, sizeof j->out, "%s", file);
    char d2[PATH_MAX + 32];
    snprintf(d2, sizeof d2, "%s/%s", root, bk);
    mkdirs(d2);
    snprintf(j->tmp, sizeof j->tmp, "%s/.objectspace-%d-%p.png", d2, (int)getpid(), (void *)j);
    j->mtime = mtime;
    j->size = size;
    queue[nqueue++] = j;
    start_next();
    return NULL;
}
