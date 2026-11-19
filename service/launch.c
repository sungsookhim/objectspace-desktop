/* Programs and files WPS starts, tracked while they run (see launch.h). */
#define _GNU_SOURCE
#include "launch.h"

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
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "plugins.h"
#include "wps_classes.h"

/* ------------------------------------------------------------- tracking */

/* One path opened by started programs; `procs` counts their live processes. */
struct use {
    struct use *next;
    char *path;
    int procs;
};

struct proc {
    struct proc *next;
    pid_t pid;
    int fd;            /* pidfd */
    struct use *u;
};

static wpo_loop *loop;
static void (*on_changed)(const char *path, bool in_use);
static struct use *uses;
static struct proc *procs;

static void use_put(struct use *u)
{
    if (--u->procs > 0) return;
    if (on_changed) on_changed(u->path, false);
    for (struct use **pp = &uses; *pp; pp = &(*pp)->next)
        if (*pp == u) { *pp = u->next; break; }
    free(u->path);
    free(u);
}

static struct proc *proc_of(pid_t pid)
{
    for (struct proc *p = procs; p; p = p->next)
        if (p->pid == pid) return p;
    return NULL;
}

static void on_exit_fd(int fd, short rev, void *arg);

static bool track(pid_t pid, struct use *u)
{
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) return false;
    struct proc *p = calloc(1, sizeof *p);
    if (!p) { close(fd); return false; }
    p->pid = pid;
    p->fd = fd;
    p->u = u;
    u->procs++;
    p->next = procs;
    procs = p;
    wpo_loop_watch_fd(loop, fd, POLLIN, on_exit_fd, p);
    return true;
}

/* Children this service did not start itself were adopted (it is a
 * subreaper): descendants of a started program whose parent exited. They
 * count for the program whose process just ended. */
static void adopt(struct use *u)
{
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[64], buf[4096];
        snprintf(p, sizeof p, "/proc/self/task/%.20s/children", de->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        for (char *s = buf, *e; *s; s = e) {
            long pid = strtol(s, &e, 10);
            if (e == s) break;
            if (pid > 0 && !proc_of((pid_t)pid) && !wps_plugins_is_host((pid_t)pid)) track((pid_t)pid, u);
        }
    }
    closedir(d);
}

static void on_exit_fd(int fd, short rev, void *arg)
{
    (void)rev;
    struct proc *p = arg;
    siginfo_t si;
    memset(&si, 0, sizeof si);
    /* Reaps it when it is our child (started here, or adopted). */
    waitid(P_PIDFD, (id_t)fd, &si, WEXITED | WNOHANG);
    adopt(p->u);   /* before it stops counting: its orphans came to us */
    wpo_loop_unwatch_fd(loop, fd);
    close(fd);
    for (struct proc **pp = &procs; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    struct use *u = p->u;
    free(p);
    use_put(u);
}

void wps_launch_init(wpo_loop *l, void (*changed)(const char *path, bool in_use))
{
    loop = l;
    on_changed = changed;
    prctl(PR_SET_CHILD_SUBREAPER, 1);
}

void wps_launch_shutdown(void)
{
    /* Started programs keep running; the service just stops watching. */
    while (procs) {
        struct proc *p = procs;
        procs = p->next;
        wpo_loop_unwatch_fd(loop, p->fd);
        close(p->fd);
        free(p);
    }
    while (uses) {
        struct use *u = uses;
        uses = u->next;
        free(u->path);
        free(u);
    }
    on_changed = NULL;
}

static pid_t spawn(char **argv, const char *cwd)
{
    posix_spawnattr_t at;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (cwd) posix_spawn_file_actions_addchdir_np(&fa, cwd);
    extern char **environ;
    pid_t pid = -1;
    int e = posix_spawnp(&pid, argv[0], &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (e) { errno = e; return -1; }
    return pid;
}

pid_t wps_launch(char **argv, const char *path, const char *cwd)
{
    pid_t pid = spawn(argv, cwd);
    if (pid < 0 || !path || !*path) return pid;
    struct use *u = NULL;
    for (struct use *x = uses; x && !u; x = x->next)
        if (!strcmp(x->path, path)) u = x;
    bool fresh = !u;
    if (fresh) {
        u = calloc(1, sizeof *u);
        if (!u || !(u->path = strdup(path))) { free(u); return pid; }
        u->next = uses;
        uses = u;
    }
    if (!track(pid, u)) {
        if (fresh) { uses = u->next; free(u->path); free(u); }
        return pid;   /* started, just not tracked */
    }
    if (fresh && on_changed) on_changed(path, true);
    return pid;
}

bool wps_launch_in_use(const char *path)
{
    for (struct use *u = uses; u; u = u->next)
        if (!strcmp(u->path, path)) return true;
    return false;
}

void wps_launch_moved(const char *from, const char *to)
{
    if (!from || !*from || !to || !*to) return;
    size_t fl = strlen(from);
    for (struct use *u = uses; u; u = u->next) {
        /* the file itself, or a folder it is in */
        if (strncmp(u->path, from, fl) || (u->path[fl] && u->path[fl] != '/')) continue;
        char *n = malloc(strlen(to) + strlen(u->path + fl) + 1);
        if (!n) continue;
        strcpy(n, to);
        strcat(n, u->path + fl);
        free(u->path);
        u->path = n;
    }
}

/* ------------------------------------------------------------ Exec lines */

int wps_split_exec(const char *exec, const char *file, char *buf, size_t n, char **argv, int max)
{
    int argc = 0;
    size_t o = 0;
    bool used = false;
    const char *p = exec;
    while (*p && argc < max - 2) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t start = o;
        bool quoted = false, any = false, keep = true;   /* any: something literal, quotes, or the file */
        for (; *p && (quoted || (*p != ' ' && *p != '\t')); p++) {
            if (*p == '"') { quoted = !quoted; any = true; continue; }
            if (*p == '\\' && p[1]) {
                p++;
            } else if (*p == '%' && p[1]) {
                p++;
                if (strchr("fFuU", *p)) {
                    if (!file) continue;
                    used = any = true;
                    for (const char *f = file; *f && o + 1 < n; f++) buf[o++] = *f;
                    continue;
                }
                if (*p == 'i' || *p == 'c' || *p == 'k') { keep = false; continue; }   /* icon, name, desktop file */
                if (*p != '%') continue;   /* deprecated or unknown codes: dropped */
            }
            if (o + 1 < n) buf[o++] = *p;
            any = true;
        }
        if (o + 1 < n) buf[o++] = 0;
        if (any && keep) argv[argc++] = buf + start;   /* "" stays an (empty) argument */
        else o = start;
    }
    if (file && !used && argc > 0 && argc < max - 1 && o + strlen(file) + 1 < n) {
        argv[argc++] = buf + o;
        strcpy(buf + o, file);
        o += strlen(file) + 1;
    }
    argv[argc] = NULL;
    return argc;
}

/* ------------------------------------------------------- default handler */

/* The value of `key` in `section` of a small ini-style file, into out. */
static bool ini_get(const char *file, const char *section, const char *key, char *out, size_t n)
{
    FILE *f = fopen(file, "re");
    if (!f) return false;
    char line[4096];
    bool in = false, found = false;
    size_t kl = strlen(key);
    while (!found && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { in = !strcmp(line, section); continue; }
        if (!in || strncmp(line, key, kl)) continue;
        const char *v = line + kl;
        while (*v == ' ') v++;
        if (*v != '=') continue;
        v++;
        while (*v == ' ') v++;
        snprintf(out, n, "%s", v);
        found = true;
    }
    fclose(f);
    return found;
}

/* The search paths for one kind of XDG directory, home first. */
static int xdg_dirs(bool config, char dirs[][PATH_MAX], int max)
{
    int k = 0;
    const char *home = getenv("HOME");
    const char *h = getenv(config ? "XDG_CONFIG_HOME" : "XDG_DATA_HOME");
    if (h && *h) snprintf(dirs[k++], PATH_MAX, "%s", h);
    else if (home) snprintf(dirs[k++], PATH_MAX, "%s/%s", home, config ? ".config" : ".local/share");
    const char *l = getenv(config ? "XDG_CONFIG_DIRS" : "XDG_DATA_DIRS");
    if (!l || !*l) l = config ? "/etc/xdg" : "/usr/local/share:/usr/share";
    char copy[8192];
    snprintf(copy, sizeof copy, "%s", l);
    for (char *s = copy, *t; k < max && (t = strsep(&s, ":"));)
        if (*t) snprintf(dirs[k++], PATH_MAX, "%s", t);
    return k;
}

/* Where an installed .desktop file with this ID is, if anywhere. */
static bool find_desktop(const char *id, char *out, size_t n)
{
    char dirs[32][PATH_MAX];
    int nd = xdg_dirs(false, dirs, 32);
    for (int i = 0; i < nd; i++) {
        snprintf(out, n, "%s/applications/%s", dirs[i], id);
        if (access(out, R_OK) == 0) return true;
        /* "vendor-app.desktop" may be "vendor/app.desktop" */
        char sub[NAME_MAX + 1];
        snprintf(sub, sizeof sub, "%s", id);
        for (char *d = sub; (d = strchr(d, '-')); d++) {
            *d = '/';
            snprintf(out, n, "%s/applications/%s", dirs[i], sub);
            if (access(out, R_OK) == 0) return true;
            *d = '-';
        }
    }
    return false;
}

/* The first installed application among a semicolon-separated list. */
static bool first_installed(const char *list, char *out, size_t n)
{
    char copy[4096];
    snprintf(copy, sizeof copy, "%s", list);
    for (char *s = copy, *t; (t = strsep(&s, ";"));)
        if (*t && find_desktop(t, out, n)) return true;
    return false;
}

static bool default_app(const char *mime, char *desktop, size_t n)
{
    char val[4096];
    char cfg[32][PATH_MAX], data[32][PATH_MAX];
    int nc = xdg_dirs(true, cfg, 32), nd = xdg_dirs(false, data, 32);
    /* mimeapps.list, desktop-specific first, in the spec's order */
    char cur[256] = "";
    const char *xd = getenv("XDG_CURRENT_DESKTOP");
    snprintf(cur, sizeof cur, "%s", xd ? xd : "");
    for (char *c = cur; *c; c++) *c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
    char *desks[8];
    int ndesk = 0;
    for (char *s = cur, *t; ndesk < 7 && (t = strsep(&s, ":"));)
        if (*t) desks[ndesk++] = t;
    for (int pass = 0; pass < 2; pass++) {
        int nn = pass == 0 ? nc : nd;
        for (int i = 0; i < nn; i++) {
            const char *base = pass == 0 ? cfg[i] : data[i];
            for (int k = 0; k <= ndesk; k++) {
                char f[PATH_MAX + 64];
                if (k < ndesk) snprintf(f, sizeof f, "%s%s/%s-mimeapps.list", base, pass ? "/applications" : "", desks[k]);
                else snprintf(f, sizeof f, "%s%s/mimeapps.list", base, pass ? "/applications" : "");
                if (ini_get(f, "[Default Applications]", mime, val, sizeof val) && first_installed(val, desktop, n))
                    return true;
            }
        }
    }
    /* no default: the first application that declares the type */
    for (int i = 0; i < nd; i++) {
        char f[PATH_MAX + 64];
        snprintf(f, sizeof f, "%s/applications/mimeinfo.cache", data[i]);
        if (ini_get(f, "[MIME Cache]", mime, val, sizeof val) && first_installed(val, desktop, n)) return true;
    }
    return false;
}

/* A file whose name says nothing: text if its start has no NUL bytes
 * (as GNOME decides, roughly). */
static bool looks_like_text(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return false; }
    char b[4096];
    ssize_t n = read(fd, b, sizeof b);
    close(fd);
    return n >= 0 && !memchr(b, 0, (size_t)n);
}

int wps_handler_argv(const char *file, char *buf, size_t n, char **argv, int max)
{
    const char *base = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
    const char *mime = wps_mime_for_name(base);
    if (!mime && looks_like_text(file)) mime = "text/plain";
    char desktop[PATH_MAX], exec[4096], v[64];
    if (!mime || !default_app(mime, desktop, sizeof desktop)) return 0;
    if (!ini_get(desktop, "[Desktop Entry]", "Exec", exec, sizeof exec)) return 0;
    if (ini_get(desktop, "[Desktop Entry]", "Terminal", v, sizeof v) && !strcmp(v, "true")) return 0;
    return wps_split_exec(exec, file, buf, n, argv, max);
}

bool wps_is_program_file(const char *path)
{
    if (access(path, X_OK) != 0) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return false;
    char m[4] = { 0 };
    ssize_t n = read(fd, m, sizeof m);
    close(fd);
    return (n == 4 && !memcmp(m, "\x7f" "ELF", 4)) || (n >= 2 && m[0] == '#' && m[1] == '!');
}

/* ------------------------------------------------------------ lock files */

bool wps_is_lock_file(const char *name)
{
    size_t l = strlen(name);
    if (!strncmp(name, ".~lock.", 7) && l > 8 && name[l - 1] == '#') return true;   /* LibreOffice */
    if (name[0] == '.' && l > 5 && !strcmp(name + l - 4, ".swp")) return true;     /* vim */
    if (!strncmp(name, ".#", 2) && l > 2) return true;                              /* Emacs */
    if (!strncmp(name, "~$", 2) && l > 2) return true;                              /* Microsoft Office */
    return false;
}

bool wps_lock_file_present(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1]) return false;
    const char *name = slash + 1;
    if (wps_is_lock_file(name)) return false;
    int dl = (int)(slash - path);
    char p[PATH_MAX + 16];
    struct stat st;
    snprintf(p, sizeof p, "%.*s/.~lock.%s#", dl, path, name);
    if (lstat(p, &st) == 0) return true;
    snprintf(p, sizeof p, "%.*s/.%s.swp", dl, path, name);
    if (lstat(p, &st) == 0) return true;
    snprintf(p, sizeof p, "%.*s/.#%s", dl, path, name);
    if (lstat(p, &st) == 0) return true;
    /* Office shortens long names: "~$" and the name without its first two
     * characters (when longer than 7); try both forms. */
    snprintf(p, sizeof p, "%.*s/~$%s", dl, path, name);
    if (lstat(p, &st) == 0) return true;
    if (strlen(name) > 7) {
        snprintf(p, sizeof p, "%.*s/~$%s", dl, path, name + 2);
        if (lstat(p, &st) == 0) return true;
    }
    return false;
}
