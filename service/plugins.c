#define _GNU_SOURCE
#ifndef OSD_PLUGINDIR
#define OSD_PLUGINDIR "/usr/lib/wps/plugins"
#endif
#include "plugins.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "OsdObject.h"
#include "osd_state.h"

#define MAX_HOSTS 32
#define MISSES_BEFORE_RESTART 3

static osdo_loop *loop;
static const osd_env *env;

struct pcall {
    struct pcall *next;
    int64_t id;
    osdo_object *o;            /* referenced while the call is out */
    osd_remote_fn done;
    void *arg;
    double deadline;
};

struct host {
    char plugin[PATH_MAX];
    char name[NAME_MAX + 1];
    int fd;
    pid_t pid;
    bool ready;
    char **classes;           /* proxy class names this host serves */
    size_t nclasses;
    char **remote;            /* "Class.method" implemented in the plug-in */
    size_t nremote;
    struct pcall *calls;
    int misses;               /* consecutive deadlines missed */
    int restarts;
    char sandbox[256];
    char error[256];
};

static struct host hosts[MAX_HOSTS];
/* Proxy class definitions: the runtime points into them until it shuts down. */
static osdo_class_def **defs_kept;
static size_t ndefs_kept;
static int nhosts;
static int64_t next_call = 1;
static int timer_fd = -1;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* --------------------------------------------------------- proxy classes */

static osdo_status apply_unavailable(osdo_fn fn, osdo_object *self, const osdo_value *a, osdo_value *ret)
{
    (void)fn; (void)self; (void)a;
    *ret = osdo_v_void();
    return OSDO_ENOMETHOD;   /* only reachable through the plug-in's host */
}

static void impl_unavailable(void) {}

/* A proxy class's title (its plug-in's osdclsQueryTitle, said in its hello). */
static struct { char *cls, *title; } *titles;
static size_t ntitles;

static char *proxy_cls_title(osdo_object *self)
{
    const osdo_class *k = (const osdo_class *)self;
    for (size_t i = 0; i < ntitles; i++)
        if (!strcmp(titles[i].cls, k->name)) return strdup(titles[i].title);
    return strdup(k->name);
}

static const osdo_override_def proxy_coverrides[] = { { "osdclsQueryTitle", (osdo_fn)proxy_cls_title } };

bool osd_plugins_is_host(pid_t pid)
{
    for (int h = 0; h < nhosts; h++)
        if (hosts[h].pid == pid) return true;
    return false;
}

static struct host *host_of_class(const char *cls)
{
    for (int h = 0; h < nhosts; h++)
        for (size_t i = 0; i < hosts[h].nclasses; i++)
            if (!strcmp(hosts[h].classes[i], cls)) return &hosts[h];
    return NULL;
}

/* Register a proxy for a class a host reported: inside wpsd it behaves like
 * its parent; its own and overridden methods run remotely. */
static void register_proxy(struct host *h, const cv *c)
{
    const char *name = cv_text(c, "name"), *parent = cv_text(c, "parent");
    osdo_class *pc = parent ? osdo_find_class(parent) : NULL;
    if (!name || !pc) return;
    const cv *ms = cv_get(c, "methods"), *ov = cv_get(c, "overrides");
    for (size_t i = 0; ov && ov->k == CV_ARRAY && i < ov->n; i++) {
        char r[256];
        snprintf(r, sizeof r, "%s.%s", name, ov->items[i].s);
        h->remote = realloc(h->remote, (h->nremote + 1) * sizeof *h->remote);
        h->remote[h->nremote++] = strdup(r);
    }
    for (size_t i = 0; ms && ms->k == CV_ARRAY && i < ms->n; i++) {
        char r[256];
        snprintf(r, sizeof r, "%s.%s", name, cv_text(&ms->items[i], "name"));
        h->remote = realloc(h->remote, (h->nremote + 1) * sizeof *h->remote);
        h->remote[h->nremote++] = strdup(r);
    }
    h->classes = realloc(h->classes, (h->nclasses + 1) * sizeof *h->classes);
    h->classes[h->nclasses++] = strdup(name);
    if (osdo_find_class(name)) return;   /* a restarted host: already registered */

    size_t nm = ms && ms->k == CV_ARRAY ? ms->n : 0;
    osdo_method_def *defs = calloc(nm + 1, sizeof *defs);
    for (size_t i = 0; i < nm; i++) {
        defs[i].name = strdup(cv_text(&ms->items[i], "name"));
        defs[i].sig = strdup(cv_text(&ms->items[i], "sig"));
        defs[i].apply = apply_unavailable;
        defs[i].impl = (osdo_fn)impl_unavailable;
    }
    /* its properties, events and file types: the proxy declares them, so
     * pages, get, set, props, the chain and actions work in wpsd (a
     * property's value kept by WPS; methods named for it run nowhere here:
     * left out) */
    const cv *ps = cv_get(c, "props"), *es = cv_get(c, "events"), *ts = cv_get(c, "types");
    size_t np = ps && ps->k == CV_ARRAY ? ps->n : 0, ne = es && es->k == CV_ARRAY ? es->n : 0, nt = ts && ts->k == CV_ARRAY ? ts->n : 0;
    osdo_property_def *props = calloc(np + 1, sizeof *props);
    for (size_t i = 0; i < np; i++) {
        const cv *q = &ps->items[i];
        #define DUP(f) (cv_text(q, #f) ? strdup(cv_text(q, #f)) : NULL)
        props[i] = (osdo_property_def){ DUP(name), DUP(type), DUP(def), DUP(page), DUP(label), DUP(choices), NULL, NULL, NULL,
                                       DUP(store), NULL, DUP(backing), 0 };
        #undef DUP
        int64_t fl = 0;
        cv_int(q, "flags", &fl);
        props[i].flags = (uint32_t)fl;
        if (!props[i].name || !props[i].type) { props[i].name = props[i].type = NULL; }
    }
    const char **events = calloc(ne + 1, sizeof *events), **types = calloc(nt + 1, sizeof *types);
    for (size_t i = 0; i < ne; i++) events[i] = strdup(es->items[i].k == CV_TEXT ? es->items[i].s : "");
    for (size_t i = 0; i < nt; i++) types[i] = strdup(ts->items[i].k == CV_TEXT ? ts->items[i].s : "");
    osdo_class_def *def = calloc(1, sizeof *def);
    def->props = props;
    def->nprops = np;
    def->events = events;
    def->nevents = ne;
    def->types = types;
    def->ntypes = nt;
    def->name = strdup(name);
    def->parent = pc->name;
    def->major = 1;
    def->parent_major = pc->major;
    def->parent_minor = pc->minor;
    def->methods = defs;
    def->nmethods = nm;
    const char *title = cv_text(c, "title");
    if (title) {
        titles = realloc(titles, (ntitles + 1) * sizeof *titles);
        titles[ntitles].cls = strdup(name);
        titles[ntitles++].title = strdup(title);
        def->coverrides = proxy_coverrides;
        def->ncoverrides = 1;
    }
    defs_kept = realloc(defs_kept, (ndefs_kept + 1) * sizeof *defs_kept);
    defs_kept[ndefs_kept++] = def;
    osdo_class *out;
    osdo_status st = osdo_register_class(def, &out);
    if (st != OSDO_OK) snprintf(h->error, sizeof h->error, "proxy for %s: %s", name, osdo_strerror(st));
}

/* ------------------------------------------------------------- hosts */

static void on_host(int fd, short rev, void *arg);

static void fail_calls(struct host *h)
{
    while (h->calls) {
        struct pcall *c = h->calls;
        h->calls = c->next;
        c->done(c->arg, NULL);
        osdo_unref(c->o);
        free(c);
    }
}

static void host_start(struct host *h)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) return;
    char exe[PATH_MAX], data[PATH_MAX + NAME_MAX + 16];
    const char *hp = getenv("OSD_PLUGHOST");
    if (hp && *hp) snprintf(exe, sizeof exe, "%s", hp);
    else {
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 32);
        exe[n < 0 ? 0 : n] = 0;
        char *slash = strrchr(exe, '/');
        snprintf(slash ? slash + 1 : exe, 32, "wps-plughost");
    }
    snprintf(data, sizeof data, "%s/plugins/%s", env->state_dir, h->name);
    osd_mkdirs(data, 0700);   /* the only place the sandboxed host may write */
    pid_t pid = fork();
    if (pid == 0) {
        dup2(sv[1], 3);   /* clears CLOEXEC on the copy */
        execl(exe, "wps-plughost", h->plugin, data, (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    if (pid < 0) { close(sv[0]); return; }
    h->pid = pid;
    h->fd = sv[0];
    h->ready = false;
    h->misses = 0;
    osdo_loop_watch_fd(loop, h->fd, POLLIN, on_host, h);
}

static void host_stop(struct host *h)
{
    if (h->fd >= 0) {
        osdo_loop_unwatch_fd(loop, h->fd);
        close(h->fd);
        h->fd = -1;
    }
    if (h->pid > 0) {
        kill(h->pid, SIGKILL);
        waitpid(h->pid, NULL, 0);
        h->pid = 0;
    }
    h->ready = false;
    fail_calls(h);
}

static void host_restart(struct host *h, const char *why)
{
    fprintf(stderr, "wpsd: plug-in %s: %s; restarting its host\n", h->name, why);
    host_stop(h);
    h->restarts++;
    for (size_t i = 0; i < h->nremote; i++) free(h->remote[i]);
    h->nremote = 0;
    for (size_t i = 0; i < h->nclasses; i++) free(h->classes[i]);
    h->nclasses = 0;
    host_start(h);
}

static void apply_changes(osdo_object *o, const cv *changes)
{
    bool any = false;
    for (size_t i = 0; changes && changes->k == CV_MAP && i < changes->n; i++) {
        const cv *cn = &changes->items[2 * i], *kv = &changes->items[2 * i + 1];
        if (cn->k != CV_TEXT || kv->k != CV_MAP) continue;
        for (size_t k = 0; k < kv->n; k++) {
            const cv *key = &kv->items[2 * k], *val = &kv->items[2 * k + 1];
            if (key->k != CV_TEXT) continue;
            if (val->k == CV_TEXT) osd_state_put_str(o, cn->s, key->s, val->s);
            else if (val->k == CV_INT) osd_state_put_int(o, cn->s, key->s, val->i);
            else if (val->k == CV_BOOL) osd_state_put_int(o, cn->s, key->s, val->b);
            else if (val->k == CV_NULL) osd_state_remove(o, cn->s, key->s);
            any = true;
        }
    }
    if (any) {
        OsdObject_osdRestoreProperties(o);   /* the fields follow the new values */
        OsdObject_osdSaveDeferred(o);
        OsdObject_osdObjectChanged(o);
    }
}

static void on_host(int fd, short rev, void *arg)
{
    struct host *h = arg;
    unsigned char buf[1 << 16];
    ssize_t n = (rev & POLLIN) ? recv(fd, buf, sizeof buf, MSG_DONTWAIT) : 0;
    if (n <= 0) {
        if (n < 0 && errno == EAGAIN) return;
        if (!h->ready && h->error[0]) { host_stop(h); return; }   /* refused at start: stay stopped */
        host_restart(h, "its host stopped");
        return;
    }
    cv m;
    if (cv_decode(buf, (size_t)n, &m)) return;
    if (cv_get(&m, "hello")) {
        const char *err = cv_text(&m, "error");
        const cv *sb = cv_get(&m, "sandbox");
        snprintf(h->sandbox, sizeof h->sandbox, "network: %s; files: %s",
                 sb ? cv_text(sb, "network") : "?", sb ? cv_text(sb, "filesystem") : "?");
        if (err) {
            snprintf(h->error, sizeof h->error, "%s", err);
            fprintf(stderr, "wpsd: plug-in %s refused: %s\n", h->name, err);
        } else {
            const cv *cs = cv_get(&m, "classes"), *rs = cv_get(&m, "replaces");
            for (size_t i = 0; cs && cs->k == CV_ARRAY && i < cs->n; i++) register_proxy(h, &cs->items[i]);
            for (size_t i = 0; rs && rs->k == CV_ARRAY && i < rs->n; i++)
                osdo_replace_class(cv_text(&rs->items[i], "original"), cv_text(&rs->items[i], "replacement"));
            h->ready = true;
            fprintf(stderr, "wpsd: plug-in %s ready (%zu classes; %s)\n", h->name, h->nclasses, h->sandbox);
        }
    } else {
        int64_t id;
        if (cv_int(&m, "reply", &id))
            for (struct pcall **pp = &h->calls; *pp; pp = &(*pp)->next)
                if ((*pp)->id == id) {
                    struct pcall *c = *pp;
                    *pp = c->next;
                    const cv *ok = cv_get(&m, "ok");
                    h->misses = 0;
                    if (ok && ok->k == CV_BOOL && ok->b) {
                        apply_changes(c->o, cv_get(&m, "changes"));
                        c->done(c->arg, cv_get(&m, "result"));
                    } else {
                        c->done(c->arg, NULL);
                    }
                    osdo_unref(c->o);
                    free(c);
                    break;
                }
    }
    cv_free(&m);
}

/* Deadlines: a call that takes too long gets the parent's behaviour; a
 * host that keeps missing them is restarted. */
static void on_timer(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    uint64_t x;
    ssize_t r = read(fd, &x, sizeof x);
    (void)r;
    double now = now_ms();
    for (int i = 0; i < nhosts; i++) {
        struct host *h = &hosts[i];
        bool missed = false;
        for (struct pcall **pp = &h->calls; *pp;) {
            struct pcall *c = *pp;
            if (c->deadline > now) { pp = &c->next; continue; }
            *pp = c->next;
            c->done(c->arg, NULL);
            osdo_unref(c->o);
            free(c);
            missed = true;
        }
        if (missed && ++h->misses >= MISSES_BEFORE_RESTART) host_restart(h, "missed its deadlines");
    }
}

/* ------------------------------------------------------------- calls */

bool osd_plugin_routes(osdo_object *o, const char *method)
{
    for (osdo_class *c = o->cls; c; c = c->parent) {
        struct host *h = host_of_class(c->name);
        if (!h) continue;
        char key[256];
        snprintf(key, sizeof key, "%s.%s", c->name, method);
        for (size_t i = 0; i < h->nremote; i++)
            if (!strcmp(h->remote[i], key)) return h->ready;
    }
    return false;
}

static void put_prop(cbuf *b, const osd_prop *p)
{
    if (p->type == 's') cb_textn(b, (const char *)p->val, p->len);
    else if (p->type == 'i' && p->len == 8) {
        int64_t v = 0;
        for (int k = 0; k < 8; k++) v |= (int64_t)((uint64_t)p->val[k] << (8 * k));
        cb_int(b, v);
    } else if (p->type == 'b' && p->len == 1) cb_bool(b, p->val[0]);
    else cb_null(b);
}

static void put_cv(cbuf *b, const cv *v)
{
    switch (v->k) {
    case CV_BOOL: cb_bool(b, v->b); break;
    case CV_INT: cb_int(b, v->i); break;
    case CV_FLOAT: cb_double(b, v->d); break;
    case CV_TEXT: cb_textn(b, v->s, v->len); break;
    default: cb_null(b); break;
    }
}

void osd_plugin_call(osdo_object *o, const char *path, const char *method, const cv *args,
                     int deadline_ms, osd_remote_fn done, void *arg)
{
    /* The host serving the most derived plug-in class that implements it. */
    struct host *h = NULL;
    const char *cls = NULL;
    for (osdo_class *c = o->cls; c && !h; c = c->parent) {
        struct host *x = host_of_class(c->name);
        if (!x) continue;
        char key[256];
        snprintf(key, sizeof key, "%s.%s", c->name, method);
        for (size_t i = 0; i < x->nremote && !h; i++)
            if (!strcmp(x->remote[i], key)) { h = x; cls = o->cls->name; }
    }
    if (!h || !h->ready) { done(arg, NULL); return; }
    struct pcall *pc = calloc(1, sizeof *pc);
    pc->id = next_call++;
    pc->o = osdo_ref(o);
    pc->done = done;
    pc->arg = arg;
    pc->deadline = now_ms() + deadline_ms;
    pc->next = h->calls;
    h->calls = pc;

    OsdObject_osdSaveState(o);   /* the bag holds the current values */
    osd_state *bag = OsdObject_data(o)->state;
    cbuf b;
    cb_init(&b);
    cb_map(&b, 5);
    cb_text(&b, "call"); cb_int(&b, pc->id);
    cb_text(&b, "class"); cb_text(&b, cls);
    cb_text(&b, "method"); cb_text(&b, method);
    cb_text(&b, "args");
    size_t na = args && args->k == CV_ARRAY ? args->n : 0;
    cb_array(&b, na);
    for (size_t i = 0; i < na; i++) put_cv(&b, &args->items[i]);
    cb_text(&b, "object");
    cb_map(&b, 3);
    cb_text(&b, "path"); path ? cb_text(&b, path) : cb_null(&b);
    cb_text(&b, "id"); cb_text(&b, OsdObject_data(o)->id);
    cb_text(&b, "props");
    cb_map(&b, bag ? bag->n : 0);
    for (size_t c = 0; bag && c < bag->n; c++) {
        cb_text(&b, bag->c[c].cls);
        cb_map(&b, bag->c[c].props.n);
        for (size_t k = 0; k < bag->c[c].props.n; k++) {
            cb_text(&b, bag->c[c].props.p[k].key);
            put_prop(&b, &bag->c[c].props.p[k]);
        }
    }
    if (b.oom || send(h->fd, b.d, b.n, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
        h->calls = pc->next;
        osdo_unref(pc->o);
        free(pc);
        done(arg, NULL);
    }
    cb_free(&b);
}

/* ------------------------------------------------------------- init */

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d)) && nhosts < MAX_HOSTS) {
        size_t l = strlen(de->d_name);
        if (l < 4 || strcmp(de->d_name + l - 3, ".so")) continue;
        struct host *h = &hosts[nhosts++];
        memset(h, 0, sizeof *h);
        h->fd = -1;
        snprintf(h->plugin, sizeof h->plugin, "%s/%s", dir, de->d_name);
        snprintf(h->name, sizeof h->name, "%.*s", (int)(l - 3), de->d_name);
        host_start(h);
    }
    if (d) closedir(d);
}

void osd_plugins_init(osdo_loop *l, const osd_env *e)
{
    loop = l;
    env = e;
    const char *dirs = getenv("OSD_PLUGIN_DIRS");
    if (dirs) {
        char *copy = strdup(dirs), *save = NULL;
        for (char *t = strtok_r(copy, ":", &save); t; t = strtok_r(NULL, ":", &save)) scan_dir(t);
        free(copy);
    } else {
        char p[PATH_MAX + 16];
        snprintf(p, sizeof p, "%s/plugins", env->config_dir);
        scan_dir(p);
        scan_dir(OSD_PLUGINDIR);   /* the installed ones (make install: $(LIBDIR)/wps/plugins) */
    }
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec its = { .it_interval = { 0, 100 * 1000 * 1000 }, .it_value = { 0, 100 * 1000 * 1000 } };
    timerfd_settime(timer_fd, 0, &its, NULL);
    osdo_loop_watch_fd(loop, timer_fd, POLLIN, on_timer, NULL);
}

void osd_plugins_shutdown(void)
{
    for (int i = 0; i < nhosts; i++) {
        host_stop(&hosts[i]);
        for (size_t k = 0; k < hosts[i].nremote; k++) free(hosts[i].remote[k]);
        for (size_t k = 0; k < hosts[i].nclasses; k++) free(hosts[i].classes[k]);
        free(hosts[i].remote);
        free(hosts[i].classes);
    }
    nhosts = 0;
    if (timer_fd >= 0) close(timer_fd);
}

void osd_plugins_status(cbuf *b)
{
    cb_array(b, (size_t)nhosts);
    for (int i = 0; i < nhosts; i++) {
        struct host *h = &hosts[i];
        cb_map(b, 6);
        cb_text(b, "name"); cb_text(b, h->name);
        cb_text(b, "ready"); cb_bool(b, h->ready);
        cb_text(b, "pid"); cb_int(b, h->pid);
        cb_text(b, "restarts"); cb_int(b, h->restarts);
        cb_text(b, "sandbox"); cb_text(b, h->sandbox);
        cb_text(b, "error"); h->error[0] ? cb_text(b, h->error) : cb_null(b);
    }
}

void osd_plugins_free_defs(void)
{
    for (size_t i = 0; i < ndefs_kept; i++) {
        osdo_class_def *d = defs_kept[i];
        for (size_t m = 0; m < d->nmethods; m++) {
            free((char *)d->methods[m].name);
            free((char *)d->methods[m].sig);
        }
        free((void *)d->methods);
        for (size_t k = 0; k < d->nprops; k++) {
            const osdo_property_def *p = &d->props[k];
            free((char *)p->name); free((char *)p->type); free((char *)p->def); free((char *)p->page);
            free((char *)p->label); free((char *)p->choices); free((char *)p->store); free((char *)p->backing);
        }
        free((void *)d->props);
        for (size_t k = 0; k < d->nevents; k++) free((char *)d->events[k]);
        free((void *)d->events);
        for (size_t k = 0; k < d->ntypes; k++) free((char *)d->types[k]);
        free((void *)d->types);
        free((char *)d->name);
        free(d);
    }
    free(defs_kept);
    defs_kept = NULL;
    ndefs_kept = 0;
}
