/* Programs and files WPS starts, tracked while they run (see launch.h). */
#define _GNU_SOURCE
#include "launch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <ctype.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>
#include <sys/mman.h>
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

static pid_t spawn(char **argv, const char *cwd, const char *display, const char *x11, const char *const *envs, size_t nenvs)
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
    /* Its output goes where the service's does (the journal, as a GNOME
     * session's programs' does): why a program did not start is there. */
    if (cwd) posix_spawn_file_actions_addchdir_np(&fa, cwd);
    extern char **environ;
    /* The environment, with the asking GUI's display in place of ours. */
    size_t ne = 0;
    while (environ[ne]) ne++;
    /* (and its X display: the service may have started before XWayland,
     * and X programs need one) */
    char **env = calloc(ne + 4 + nenvs, sizeof *env), wl[4200], xd[64];
    size_t k = 0;
    bool w = display && *display, x = x11 && *x11;
    if (env && w) {
        /* by its name when it is in the runtime folder, as the session
         * gives it: IBus finds its daemon by a file named after it
         * ("...-unix-wayland-0"); a full path named another, and programs
         * said IBus was not running */
        const char *rt = getenv("XDG_RUNTIME_DIR"), *name = display;
        size_t rl = rt ? strlen(rt) : 0;
        if (rl && !strncmp(display, rt, rl) && display[rl] == '/' && !strchr(display + rl + 1, '/')) name = display + rl + 1;
        snprintf(wl, sizeof wl, "WAYLAND_DISPLAY=%s", name);
        env[k++] = wl;
    }
    if (env && x) {
        snprintf(xd, sizeof xd, "DISPLAY=%s", x11);
        env[k++] = xd;
    }
    /* GNOME's Settings will not start outside GNOME ("only supported under
     * GNOME and Unity"); in the WPS session most of its panels work, so it
     * is told it is in GNOME. */
    const char *b0 = strrchr(argv[0], '/');
    b0 = b0 ? b0 + 1 : argv[0];
    bool gnome_only = !strcmp(b0, "gnome-control-center");   /* (in GNOME itself: the same name) */
    for (size_t i = 0; env && i < ne; i++) {
        bool replaced = false;
        for (size_t j = 0; j < nenvs && !replaced; j++) {
            size_t nl = strcspn(envs[j], "=") + 1;
            replaced = !strncmp(environ[i], envs[j], nl);
        }
        if (!replaced && !(w && !strncmp(environ[i], "WAYLAND_DISPLAY=", 16)) && !(x && !strncmp(environ[i], "DISPLAY=", 8)) &&
            !(gnome_only && !strncmp(environ[i], "XDG_CURRENT_DESKTOP=", 20)))
            env[k++] = environ[i];
    }
    for (size_t j = 0; env && j < nenvs; j++)
        if (!(gnome_only && !strncmp(envs[j], "XDG_CURRENT_DESKTOP=", 20))) env[k++] = (char *)envs[j];
    if (env && gnome_only) env[k++] = "XDG_CURRENT_DESKTOP=GNOME";
    /* Each program in a group of its own (app.slice, as GNOME's are), not
     * in wpsd's: its memory is its own (a program using too much is what
     * the kernel stops, not wpsd with all the others), and it outlives
     * wpsd. systemd-run --scope becomes the program (the same process). */
    char *sargv[80], unit[96];
    char **run = argv;
    static unsigned serial;
    if (getenv("INVOCATION_ID") && access("/usr/bin/systemd-run", X_OK) == 0) {
        const char *b = strrchr(argv[0], '/');
        b = b ? b + 1 : argv[0];
        char name[40];
        size_t j = 0;
        for (; *b && j < sizeof name - 1; b++)
            name[j++] = (isalnum((unsigned char)*b) || *b == '-' || *b == '.') ? *b : '_';
        name[j] = 0;
        snprintf(unit, sizeof unit, "app-wps-%s-%d-%u.scope", j ? name : "program", (int)getpid(), ++serial);
        int n = 0;
        sargv[n++] = "/usr/bin/systemd-run";
        sargv[n++] = "--user";
        sargv[n++] = "--scope";
        sargv[n++] = "--quiet";
        sargv[n++] = "--collect";
        sargv[n++] = "--slice=app.slice";
        sargv[n++] = "--unit";
        sargv[n++] = unit;
        sargv[n++] = "--";
        for (int i = 0; argv[i] && n < 79; i++) sargv[n++] = argv[i];
        sargv[n] = NULL;
        run = sargv;
    }
    pid_t pid = -1;
    int e = posix_spawnp(&pid, run[0], &fa, &at, run, env ? env : environ);
    free(env);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (e) { errno = e; return -1; }
    return pid;
}

pid_t wps_launch(char **argv, const char *path, const char *cwd, const char *display, const char *x11,
                 const char *const *envs, size_t nenvs)
{
    pid_t pid = spawn(argv, cwd, display, x11, envs, nenvs);
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

int wps_split_exec_files(const char *exec, const char *const *files, size_t nfiles, char *buf, size_t n, char **argv, int max)
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
                    if (!nfiles) continue;
                    used = any = true;
                    /* the first here; the others (several dropped on it)
                     * each an argument after it - %f and %u too, which most
                     * programs take as well */
                    for (const char *f = files[0]; *f && o + 1 < n; f++) buf[o++] = *f;
                    for (size_t k = 1; k < nfiles && argc < max - 3 && o + strlen(files[k]) + 2 < n; k++) {
                        buf[o++] = 0;
                        argv[argc++] = buf + start;
                        start = o;
                        for (const char *f = files[k]; *f; f++) buf[o++] = *f;
                    }
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
    for (size_t k = 0; nfiles && !used && k < nfiles && argc > 0 && argc < max - 1 && o + strlen(files[k]) + 1 < n; k++) {
        argv[argc++] = buf + o;   /* no field code: after the command */
        strcpy(buf + o, files[k]);
        o += strlen(files[k]) + 1;
    }
    argv[argc] = NULL;
    return argc;
}

int wps_split_exec(const char *exec, const char *file, char *buf, size_t n, char **argv, int max)
{
    return wps_split_exec_files(exec, file ? &file : NULL, file ? 1 : 0, buf, n, argv, max);
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

/* The first installed application among a semicolon-separated list (and
 * its ID, if asked). */
static char *first_id;
static size_t first_idn;
static bool first_installed(const char *list, char *out, size_t n)
{
    char copy[4096];
    snprintf(copy, sizeof copy, "%s", list);
    for (char *s = copy, *t; (t = strsep(&s, ";"));)
        if (*t && find_desktop(t, out, n)) {
            if (first_id) snprintf(first_id, first_idn, "%s", t);
            return true;
        }
    return false;
}

static bool default_app(const char *mime, char *desktop, size_t n);

/* (on the loop thread only, as all of these) */
static bool default_app_id(const char *mime, char *desktop, size_t n, char *id, size_t idn)
{
    first_id = id;
    first_idn = idn;
    bool ok = default_app(mime, desktop, n);
    first_id = NULL;
    return ok;
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

/* ------------------------------------------------------------- terminals */

static bool on_path(const char *prog)
{
    if (strchr(prog, '/')) return access(prog, X_OK) == 0;
    const char *pv = getenv("PATH");
    char copy[8192], p[PATH_MAX];
    snprintf(copy, sizeof copy, "%s", pv && *pv ? pv : "/usr/local/bin:/usr/bin:/bin");
    for (char *s = copy, *t; (t = strsep(&s, ":"));) {
        snprintf(p, sizeof p, "%s/%s", *t ? t : ".", prog);
        if (access(p, X_OK) == 0) return true;
    }
    return false;
}

/* Terminals, best first, each with the arguments that open a new window of
 * its own (one that lives as long as the window, so the program can be
 * tracked) running the command that follows. */
static const char *const terminals[][5] = {
    { "xdg-terminal-exec", NULL },
    { "ptyxis", "-s", "--", NULL },
    { "gnome-terminal", "--wait", "--", NULL },
    { "kgx", "-e", NULL },
    { "konsole", "--nofork", "-e", NULL },
    { "xfce4-terminal", "--disable-server", "-x", NULL },
    { "alacritty", "-e", NULL },
    { "kitty", NULL },
    { "foot", NULL },
    { "wezterm", "start", "--", NULL },
    { "xterm", "-e", NULL },
};

/* The System object's Terminal (v0.3-design 8). */
static char term_choice[64] = "automatic", term_cmd[1024];

void wps_terminal_choose(const char *choice, const char *command)
{
    snprintf(term_choice, sizeof term_choice, "%s", choice && *choice ? choice : "automatic");
    snprintf(term_cmd, sizeof term_cmd, "%s", command ? command : "");
}

const char *wps_terminal_now(void)
{
    if (term_cmd[0]) return term_cmd;
    const char *env = getenv("WPS_TERMINAL");
    if (env && *env) return env;
    for (size_t t = 0; t < sizeof terminals / sizeof *terminals; t++)
        if (!strcmp(term_choice, terminals[t][0]) && on_path(terminals[t][0])) return terminals[t][0];
    for (size_t t = 0; t < sizeof terminals / sizeof *terminals; t++)
        if (on_path(terminals[t][0])) return terminals[t][0];
    return NULL;
}

int wps_terminal_wrap(char **argv, int argc, int max)
{
    char *pre[16];
    int k = 0;
    const char *env = term_cmd[0] ? term_cmd : getenv("WPS_TERMINAL");
    static char envbuf[1024];
    if (env && *env) {
        /* the user's command (the System object's, else WPS_TERMINAL): the
         * program's goes after these words */
        k = wps_split_exec(env, NULL, envbuf, sizeof envbuf, pre, 16);
    } else {
        /* the System object's choice if it is installed, else the first installed */
        for (int pass = 0; pass < 2 && !k; pass++)
            for (size_t t = 0; t < sizeof terminals / sizeof *terminals && !k; t++)
                if ((pass || !strcmp(term_choice, terminals[t][0])) && on_path(terminals[t][0]))
                    for (int i = 0; terminals[t][i]; i++) pre[k++] = (char *)terminals[t][i];
    }
    if (!k || argc + k >= max) return 0;
    memmove(argv + k, argv, (size_t)(argc + 1) * sizeof *argv);
    memcpy(argv, pre, (size_t)k * sizeof *argv);
    return argc + k;
}

/* Close window on exit off: the terminal waits for Enter after the
 * program ends (with any terminal: through sh). */
int wps_hold_wrap(char **argv, int argc, int max)
{
    static const char script[] = "\"$@\"; s=$?; printf '\\n[%s ended (exit status %d). Press Enter to close this window.]' \"$1\" \"$s\"; read -r _";
    if (argc < 1 || argc + 4 >= max) return argc;
    memmove(argv + 4, argv, (size_t)(argc + 1) * sizeof *argv);
    argv[0] = "sh";
    argv[1] = "-c";
    argv[2] = (char *)script;
    argv[3] = "sh";
    return argc + 4;
}

bool wps_desktop_wants_terminal(const char *desktop);

int wps_handler_argv(const char *file, char *buf, size_t n, char **argv, int max)
{
    const char *base = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
    const char *mime = wps_mime_for_name(base);
    if (!mime && looks_like_text(file)) mime = "text/plain";
    char desktop[PATH_MAX], exec[4096];
    if (!mime || !default_app(mime, desktop, sizeof desktop)) return 0;
    if (!ini_get(desktop, "[Desktop Entry]", "Exec", exec, sizeof exec)) return 0;
    int argc = wps_split_exec(exec, file, buf, n, argv, max);
    if (argc > 0 && wps_desktop_wants_terminal(desktop)) argc = wps_terminal_wrap(argv, argc, max);
    return argc;
}

/* ------------------------------------------------------------ Open With */

/* An entry's value in the user's language (Name[ko_KR], Name[ko], Name). */
static bool ini_get_local(const char *file, const char *key, char *out, size_t n)
{
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    char lang[64] = "";
    snprintf(lang, sizeof lang, "%s", l ? l : "");
    lang[strcspn(lang, ".@")] = 0;
    char k[128];
    if (lang[0] && strcmp(lang, "C") && strcmp(lang, "POSIX")) {
        snprintf(k, sizeof k, "%s[%s]", key, lang);
        if (ini_get(file, "[Desktop Entry]", k, out, n)) return true;
        char *u = strchr(lang, '_');
        if (u) {
            *u = 0;
            snprintf(k, sizeof k, "%s[%s]", key, lang);
            if (ini_get(file, "[Desktop Entry]", k, out, n)) return true;
        }
    }
    return ini_get(file, "[Desktop Entry]", key, out, n);
}

/* An application's entry read into `a` (id given); false if it is not
 * one to offer (hidden, no command, not an application). */
static bool app_read(const char *id, const char *path, wps_app *a, bool listed_only)
{
    char v[4096];
    if (ini_get(path, "[Desktop Entry]", "Hidden", v, sizeof v) && !strcmp(v, "true")) return false;
    if (ini_get(path, "[Desktop Entry]", "Type", v, sizeof v) && strcmp(v, "Application")) return false;
    if (!ini_get(path, "[Desktop Entry]", "Exec", v, sizeof v) || !*v) return false;
    if (listed_only && ini_get(path, "[Desktop Entry]", "NoDisplay", v, sizeof v) && !strcmp(v, "true")) return false;
    if (ini_get(path, "[Desktop Entry]", "TryExec", v, sizeof v) && *v && !on_path(v)) return false;
    memset(a, 0, sizeof *a);
    snprintf(a->id, sizeof a->id, "%s", id);
    snprintf(a->path, sizeof a->path, "%s", path);
    if (!ini_get_local(path, "Name", a->name, sizeof a->name)) snprintf(a->name, sizeof a->name, "%s", id);
    ini_get(path, "[Desktop Entry]", "Icon", a->icon, sizeof a->icon);
    return true;
}

static bool app_listed(const wps_app *out, int n, const char *id)
{
    for (int i = 0; i < n; i++) if (!strcmp(out[i].id, id)) return true;
    return false;
}

/* Add the installed applications of a semicolon-separated list. */
static int add_apps(const char *list, wps_app *out, int n, int max)
{
    char copy[4096], path[PATH_MAX];
    snprintf(copy, sizeof copy, "%s", list);
    for (char *s = copy, *t; n < max && (t = strsep(&s, ";"));)
        if (*t && !app_listed(out, n, t) && find_desktop(t, path, sizeof path) && app_read(t, path, &out[n], false)) n++;
    return n;
}

/* shared-mime-info's subclasses ("text/x-csrc text/plain"): a type's
 * programs include its parents' (a text editor for C files). */
static bool mime_parent(const char *mime, char *out, size_t n)
{
    char dirs[32][PATH_MAX];
    int nd = xdg_dirs(false, dirs, 32);
    for (int i = nd - 1; i >= 0; i--) {   /* (system first: the user's rarely has one) */
        char f[PATH_MAX + 32], line[512];
        snprintf(f, sizeof f, "%s/mime/subclasses", dirs[i]);
        FILE *fp = fopen(f, "re");
        if (!fp) continue;
        size_t ml = strlen(mime);
        bool found = false;
        while (!found && fgets(line, sizeof line, fp)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, mime, ml) && line[ml] == ' ') { snprintf(out, n, "%s", line + ml + 1); found = true; }
        }
        fclose(fp);
        if (found) return true;
    }
    return false;
}

static const char *file_mime(const char *file)
{
    const char *base = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
    const char *mime = wps_mime_for_name(base);
    if (!mime && looks_like_text(file)) mime = "text/plain";
    return mime;
}

int wps_apps_for_file(const char *file, char *mime_out, size_t mn, wps_app *out, int max)
{
    const char *m0 = file_mime(file);
    if (mime_out) snprintf(mime_out, mn, "%s", m0 ? m0 : "");
    if (!m0) return 0;
    int n = 0;
    char desktop[PATH_MAX], id[256];
    if (default_app_id(m0, desktop, sizeof desktop, id, sizeof id) && app_read(id, desktop, &out[0], false)) {
        out[0].is_default = true;
        n = 1;
    }
    char cfg[32][PATH_MAX], data[32][PATH_MAX], val[4096];
    int nc = xdg_dirs(true, cfg, 32), nd = xdg_dirs(false, data, 32);
    char mime[256];
    snprintf(mime, sizeof mime, "%s", m0);
    for (int depth = 0; depth < 4 && n < max; depth++) {
        /* added associations (mimeapps.list), then the declared ones */
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < (pass ? nd : nc) && n < max; i++) {
                char f[PATH_MAX + 64];
                snprintf(f, sizeof f, "%s%s/mimeapps.list", pass ? data[i] : cfg[i], pass ? "/applications" : "");
                if (ini_get(f, "[Added Associations]", mime, val, sizeof val)) n = add_apps(val, out, n, max);
            }
        /* the user's own entries (an Association page's copies: no
         * mimeinfo.cache there, usually) */
        if (nd > 0 && n < max) {
            char d[PATH_MAX + 16];
            snprintf(d, sizeof d, "%s/applications", data[0]);
            DIR *dir = opendir(d);
            struct dirent *de;
            while (dir && n < max && (de = readdir(dir))) {
                size_t l = strlen(de->d_name);
                if (l < 9 || strcmp(de->d_name + l - 8, ".desktop") || app_listed(out, n, de->d_name)) continue;
                char p[PATH_MAX + NAME_MAX + 32], types[4096];
                snprintf(p, sizeof p, "%s/%s", d, de->d_name);
                if (!ini_get(p, "[Desktop Entry]", "MimeType", types, sizeof types)) continue;
                bool has = false;
                for (char *q = types, *t; !has && (t = strsep(&q, ";"));) has = !strcmp(t, mime);
                if (has && strlen(p) < PATH_MAX && app_read(de->d_name, p, &out[n], false)) n++;
            }
            if (dir) closedir(dir);
        }
        for (int i = 0; i < nd && n < max; i++) {
            char f[PATH_MAX + 64];
            snprintf(f, sizeof f, "%s/applications/mimeinfo.cache", data[i]);
            if (ini_get(f, "[MIME Cache]", mime, val, sizeof val)) n = add_apps(val, out, n, max);
        }
        char parent[256];
        if (!mime_parent(mime, parent, sizeof parent)) break;
        snprintf(mime, sizeof mime, "%s", parent);
    }
    return n;
}

static int app_cmp(const void *a, const void *b) { return strcoll(((const wps_app *)a)->name, ((const wps_app *)b)->name); }

int wps_apps_all(wps_app *out, int max)
{
    char dirs[32][PATH_MAX];
    int nd = xdg_dirs(false, dirs, 32), n = 0;
    for (int i = 0; i < nd && n < max; i++) {
        char d[PATH_MAX + 16];
        snprintf(d, sizeof d, "%s/applications", dirs[i]);
        DIR *dir = opendir(d);
        struct dirent *de;
        while (dir && n < max && (de = readdir(dir))) {
            size_t l = strlen(de->d_name);
            if (l < 9 || strcmp(de->d_name + l - 8, ".desktop") || app_listed(out, n, de->d_name)) continue;
            char p[PATH_MAX + NAME_MAX + 32];
            snprintf(p, sizeof p, "%s/%s", d, de->d_name);
            if (strlen(p) >= PATH_MAX) continue;
            /* (one the user's folder hides stays hidden: listed first, then skipped) */
            wps_app a;
            if (app_read(de->d_name, p, &a, true)) out[n++] = a;
            else {
                memset(&out[n], 0, sizeof out[n]);
                snprintf(out[n].id, sizeof out[n].id, "%s", de->d_name);
                out[n].hidden = true;
                n++;
            }
        }
        if (dir) closedir(dir);
    }
    int w = 0;
    for (int i = 0; i < n; i++) if (!out[i].hidden) out[w++] = out[i];
    qsort(out, (size_t)w, sizeof *out, app_cmp);
    return w;
}

int wps_app_argv(const char *id, const char *file, char *buf, size_t n, char **argv, int max)
{
    char path[PATH_MAX], exec[4096];
    if (strchr(id, '/') || !find_desktop(id, path, sizeof path)) return 0;
    if (!ini_get(path, "[Desktop Entry]", "Exec", exec, sizeof exec)) return 0;
    int argc = wps_split_exec(exec, file, buf, n, argv, max);
    if (argc > 0 && wps_desktop_wants_terminal(path)) argc = wps_terminal_wrap(argv, argc, max);
    return argc;
}

/* Rewrite one key of one section of an ini file (made if need be). */
static bool ini_set(const char *file, const char *section, const char *key, const char *value)
{
    FILE *in = fopen(file, "re");
    size_t cap = 4096, len = 0;
    char *outb = malloc(cap), line[4096];
    bool in_sec = false, done = false, seen_sec = false;
    #define PUT(str) do { size_t sl_ = strlen(str); while (len + sl_ + 1 > cap) { cap *= 2; outb = realloc(outb, cap); } memcpy(outb + len, str, sl_); len += sl_; } while (0)
    size_t kl = strlen(key);
    while (in && fgets(line, sizeof line, in)) {
        char t[4096];
        snprintf(t, sizeof t, "%s", line);
        t[strcspn(t, "\r\n")] = 0;
        if (t[0] == '[') {
            if (in_sec && !done) { PUT(key); PUT("="); PUT(value); PUT("\n"); done = true; }
            in_sec = !strcmp(t, section);
            seen_sec |= in_sec;
        } else if (in_sec && !strncmp(t, key, kl) && (t[kl] == '=' || t[kl] == ' ')) {
            if (!done) { PUT(key); PUT("="); PUT(value); PUT("\n"); done = true; }
            continue;
        }
        PUT(line);
        if (len && outb[len - 1] != '\n') PUT("\n");
    }
    if (in) fclose(in);
    if (!done) {
        if (!seen_sec) { if (len) PUT("\n"); PUT(section); PUT("\n"); }
        PUT(key); PUT("="); PUT(value); PUT("\n");
    }
    #undef PUT
    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof tmp, "%s.wps-new", file);
    FILE *o = fopen(tmp, "we");
    bool ok = o && fwrite(outb, 1, len, o) == len;
    if (o && fclose(o) != 0) ok = false;
    free(outb);
    if (ok && rename(tmp, file) != 0) ok = false;
    if (!ok) unlink(tmp);
    return ok;
}

bool wps_app_types(const char *desktop, char *out, size_t n)
{
    const char *base = strrchr(desktop, '/') ? strrchr(desktop, '/') + 1 : desktop;
    char path[PATH_MAX];
    if (!find_desktop(base, path, sizeof path)) snprintf(path, sizeof path, "%s", desktop);   /* (the user's copy first) */
    out[0] = 0;
    return ini_get(path, "[Desktop Entry]", "MimeType", out, n);
}

bool wps_app_set_types(const char *desktop, const char *types)
{
    if (strchr(types, '\n') || strchr(types, '\r')) return false;
    const char *base = strrchr(desktop, '/') ? strrchr(desktop, '/') + 1 : desktop;
    char dirs[1][PATH_MAX];
    if (xdg_dirs(false, dirs, 1) < 1) return false;
    char ud[PATH_MAX + 16], mine[PATH_MAX + NAME_MAX + 32], src[PATH_MAX];
    snprintf(ud, sizeof ud, "%s/applications", dirs[0]);
    snprintf(mine, sizeof mine, "%s/%s", ud, base);
    if (access(mine, F_OK) != 0) {
        /* a copy of the entry in the user's own (GNOME reads it too) */
        if (!find_desktop(base, src, sizeof src)) snprintf(src, sizeof src, "%s", desktop);
        FILE *in = fopen(src, "re");
        if (!in) return false;
        mkdir(dirs[0], 0700);
        mkdir(ud, 0700);
        FILE *o = fopen(mine, "wxe");
        if (!o) { fclose(in); return false; }
        char buf[8192];
        size_t r;
        bool ok = true;
        while ((r = fread(buf, 1, sizeof buf, in)) > 0) ok &= fwrite(buf, 1, r, o) == r;
        fclose(in);
        if (fclose(o) != 0 || !ok) { unlink(mine); return false; }
    }
    char t[4096];
    snprintf(t, sizeof t, "%s%s", types, *types && types[strlen(types) - 1] != ';' ? ";" : "");
    return ini_set(mine, "[Desktop Entry]", "MimeType", t);
}

bool wps_app_set_default(const char *file, const char *id)
{
    const char *mime = file_mime(file);
    char path[PATH_MAX];
    if (!mime || strchr(id, '/') || strchr(id, ';') || !find_desktop(id, path, sizeof path)) return false;
    char cfg[1][PATH_MAX];
    if (xdg_dirs(true, cfg, 1) < 1) return false;
    mkdir(cfg[0], 0700);
    char f[PATH_MAX + 32], val[4096], added[4200];
    snprintf(f, sizeof f, "%s/mimeapps.list", cfg[0]);
    /* the default (shared with GNOME), and first among the added ones */
    if (!ini_set(f, "[Default Applications]", mime, id)) return false;
    snprintf(added, sizeof added, "%s;", id);
    if (ini_get(f, "[Added Associations]", mime, val, sizeof val)) {
        char copy[4096];
        snprintf(copy, sizeof copy, "%s", val);
        for (char *s = copy, *t; (t = strsep(&s, ";"));)
            if (*t && strcmp(t, id) && strlen(added) + strlen(t) + 2 < sizeof added) { strcat(added, t); strcat(added, ";"); }
    }
    return ini_set(f, "[Added Associations]", mime, added);
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

bool wps_desktop_wants_terminal(const char *desktop)
{
    char v[64];
    return ini_get(desktop, "[Desktop Entry]", "Terminal", v, sizeof v) && !strcmp(v, "true");
}

/* Libraries only programs with windows of their own link. */
static bool gui_library(const char *lib)
{
    static const char *const prefixes[] = {
        "libwayland-client", "libX11", "libxcb.", "libgtk-", "libgdk-", "libadwaita", "libQt5Gui", "libQt6Gui",
        "libQt5Widgets", "libQt6Widgets", "libSDL", "libglfw", "libEGL", "libGL.", "libGLX", "libOpenGL",
        "libvulkan", "libfltk", "libwx_", "libgdk_pixbuf", "libelementary", "libnotcurses-x",
    };
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++)
        if (!strncmp(lib, prefixes[i], strlen(prefixes[i]))) return true;
    return false;
}

/* Walk an ELF file's dynamic section: does it link a GUI library? */
#define ELF_GUI(Ehdr, Phdr, Dyn)                                                                     \
    {                                                                                                \
        const Ehdr *eh = (const Ehdr *)m;                                                            \
        if (eh->e_phoff > sz || eh->e_phnum > 4096 ||                                                \
            eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Phdr) > sz) return false;                   \
        const Phdr *ph = (const Phdr *)(m + eh->e_phoff);                                            \
        const Phdr *dyn = NULL;                                                                      \
        for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_DYNAMIC) dyn = &ph[i];          \
        if (!dyn || dyn->p_offset > sz || dyn->p_offset + dyn->p_filesz > sz) return false;          \
        const Dyn *d = (const Dyn *)(m + dyn->p_offset);                                             \
        size_t nd = dyn->p_filesz / sizeof(Dyn);                                                     \
        uint64_t strtab = 0, stroff = UINT64_MAX;                                                    \
        for (size_t i = 0; i < nd && d[i].d_tag != DT_NULL; i++) if (d[i].d_tag == DT_STRTAB) strtab = d[i].d_un.d_val; \
        for (int i = 0; i < eh->e_phnum; i++)                                                        \
            if (ph[i].p_type == PT_LOAD && strtab >= ph[i].p_vaddr && strtab < ph[i].p_vaddr + ph[i].p_filesz) \
                stroff = strtab - ph[i].p_vaddr + ph[i].p_offset;                                    \
        if (stroff >= sz) return false;                                                              \
        for (size_t i = 0; i < nd && d[i].d_tag != DT_NULL; i++) {                                   \
            if (d[i].d_tag != DT_NEEDED || stroff + d[i].d_un.d_val >= sz) continue;                 \
            const char *lib = (const char *)m + stroff + d[i].d_un.d_val;                            \
            if (memchr(lib, 0, sz - (stroff + d[i].d_un.d_val)) && gui_library(lib)) return true;    \
        }                                                                                            \
        return false;                                                                                \
    }

static bool elf_links_gui(const unsigned char *m, size_t sz)
{
    if (sz < sizeof(Elf64_Ehdr) || m[EI_DATA] != ELFDATA2LSB) return false;
    if (m[EI_CLASS] == ELFCLASS64) ELF_GUI(Elf64_Ehdr, Elf64_Phdr, Elf64_Dyn)
    if (m[EI_CLASS] == ELFCLASS32) ELF_GUI(Elf32_Ehdr, Elf32_Phdr, Elf32_Dyn)
    return false;
}

bool wps_program_needs_terminal(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return false;
    struct stat st;
    char head[2] = { 0 };
    if (fstat(fd, &st) != 0 || pread(fd, head, 2, 0) != 2) { close(fd); return false; }
    if (head[0] == '#' && head[1] == '!') { close(fd); return true; }   /* scripts get a window, as .CMD files did */
    size_t sz = (size_t)st.st_size;
    if (sz > (size_t)1 << 31) sz = (size_t)1 << 31;
    void *m = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return false;
    bool gui = elf_links_gui(m, sz);
    munmap(m, sz);
    return !gui;   /* a program without a window library is a text-mode one */
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
