/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * osd-plughost - runs one plug-in package's classes outside osdd
 * (design: Plug-in isolation; decided 2026-11-18: never in-process,
 * always sandboxed).
 *
 * Started by osdd with a socketpair on fd 3. Before loading the plug-in it
 * sandboxes itself: no new privileges, an empty network namespace (or,
 * failing that, Landlock TCP rules), and a Landlock filesystem ruleset that
 * allows reading system libraries and the plug-in's own folder, and
 * writing only the plug-in's data folder. File work beyond that goes
 * through the service, which applies the user's permissions.
 *
 * Protocol (SOCK_SEQPACKET, one CBOR map per message):
 *   host -> osdd   {hello:true, classes:[{name, parent, methods:[{name,sig}],
 *                  overrides:[name], props:[{name, type, ..., flags}],
 *                  events:[name], types:[mime]}], replaces:[{original, replacement}],
 *                  sandbox:{network, filesystem}, error?}
 *   osdd -> host   {call:N, class, method, args:[...], object:{path, id,
 *                  props:{Class:{key:value}}}}
 *   host -> osdd   {reply:N, ok, result, changes:{Class:{key:value|null}}}
 *
 * usage: osd-plughost PLUGIN.so DATA_DIR    (fd 3: the socket to osdd)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "cbor.h"
#include "osdo.h"
#include "osd_classes.h"
#include "osd_state.h"
#include "OsdFileSystem.h"
#include "OsdObject.h"

static int sock = 3;
static const char *net_mode = "unrestricted", *fs_mode = "unrestricted";

/* ------------------------------------------------------------- sandbox */

static void landlock_allow(int ruleset, const char *path, __u64 access)
{
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return;
    struct landlock_path_beneath_attr pb = { .allowed_access = access, .parent_fd = fd };
    struct stat st;
    if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode))
        pb.allowed_access &= LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                             LANDLOCK_ACCESS_FS_TRUNCATE;
    syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    close(fd);
}

static void sandbox(const char *plugin, const char *data)
{
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    /* No network: an empty network namespace (via an unprivileged user
     * namespace). */
    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0) net_mode = "none (empty network namespace)";

    int abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) { fs_mode = "unrestricted (Landlock unavailable)"; return; }
    __u64 read = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    __u64 write = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                  LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                  LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                  LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2) write |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) write |= LANDLOCK_ACCESS_FS_TRUNCATE;
    struct landlock_ruleset_attr ra = { .handled_access_fs = read | write };
    bool tcp = abi >= 4 && strcmp(net_mode, "unrestricted") == 0;
    if (tcp) ra.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
    int rs = (int)syscall(SYS_landlock_create_ruleset, &ra, sizeof ra, 0);
    if (rs < 0) { fs_mode = "unrestricted (ruleset refused)"; return; }
    /* System libraries and the plug-in's own folder: read and execute. */
    static const char *ro[] = { "/usr", "/lib", "/lib64", "/etc/ld.so.cache", "/proc/self", NULL };
    for (int i = 0; ro[i]; i++) landlock_allow(rs, ro[i], read);
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", plugin);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    landlock_allow(rs, dir, read);
    /* The plug-in's data folder: read and write. */
    mkdir(data, 0700);
    landlock_allow(rs, data, read | write);
    if (syscall(SYS_landlock_restrict_self, rs, 0) == 0) {
        fs_mode = "limited to system libraries, the plug-in and its data folder";
        if (tcp) net_mode = "no TCP (Landlock)";
    }
    close(rs);
}

/* ------------------------------------------------------------ messages */

static void send_cbuf(cbuf *b)
{
    if (!b->oom) send(sock, b->d, b->n, MSG_NOSIGNAL);
}

static size_t base_classes;   /* classes registered before the plug-in */

static void hello(const char *error)
{
    size_t n = osdo_class_count(), nplug = 0, nrep = 0;
    for (size_t i = base_classes; i < n; i++) nplug += !osdo_class_at(i)->is_meta;
    for (size_t i = 0; i < base_classes; i++) nrep += osdo_class_at(i)->replaced_by != NULL;
    cbuf b;
    cb_init(&b);
    cb_map(&b, error ? 5 : 4);
    cb_text(&b, "hello"); cb_bool(&b, true);
    cb_text(&b, "classes");
    cb_array(&b, error ? 0 : nplug);
    for (size_t i = base_classes; !error && i < n; i++) {
        osdo_class *c = osdo_class_at(i);
        if (c->is_meta) continue;
        size_t novr = 0;
        for (uint32_t s = 0; s < c->mbase; s++) novr += c->mtab[s] != c->parent->mtab[s];
        cb_map(&b, 8);
        cb_text(&b, "name"); cb_text(&b, c->name);
        char *title = OsdObject_osdclsQueryTitle(c);   /* (its class method: run here, said once) */
        cb_text(&b, "title"); cb_text(&b, title ? title : c->name);
        free(title);
        cb_text(&b, "parent"); cb_text(&b, c->parent->name);
        /* its properties, events and file types (object-properties-design.md 2.7) */
        cb_text(&b, "props");
        cb_array(&b, c->nprops);
        for (size_t k = 0; k < c->nprops; k++) {
            const osdo_property_def *p = &c->props[k];
            const char *f[12][2] = { { "name", p->name }, { "type", p->type }, { "def", p->def }, { "page", p->page },
                                     { "label", p->label }, { "choices", p->choices }, { "get", p->get }, { "set", p->set },
                                     { "show", p->show }, { "store", p->store }, { "options", p->options }, { "backing", p->backing } };
            size_t nf = 1;
            for (int i = 0; i < 12; i++) nf += f[i][1] != NULL;
            cb_map(&b, nf);
            for (int i = 0; i < 12; i++) if (f[i][1]) { cb_text(&b, f[i][0]); cb_text(&b, f[i][1]); }
            cb_text(&b, "flags"); cb_int(&b, p->flags);
        }
        cb_text(&b, "events");
        cb_array(&b, c->nevents);
        for (size_t k = 0; k < c->nevents; k++) cb_text(&b, c->events[k]);
        cb_text(&b, "types");
        cb_array(&b, c->ntypes);
        for (size_t k = 0; k < c->ntypes; k++) cb_text(&b, c->types[k]);
        cb_text(&b, "methods");
        cb_array(&b, c->nmethods);
        for (uint32_t s = c->mbase; s < c->nslots; s++) {
            cb_map(&b, 2);
            cb_text(&b, "name"); cb_text(&b, c->minfo[s]->name);
            cb_text(&b, "sig"); cb_text(&b, c->minfo[s]->sig);
        }
        cb_text(&b, "overrides");
        cb_array(&b, novr);
        for (uint32_t s = 0; s < c->mbase; s++)
            if (c->mtab[s] != c->parent->mtab[s]) cb_text(&b, c->minfo[s]->name);
    }
    cb_text(&b, "replaces");
    cb_array(&b, error ? 0 : nrep);
    for (size_t i = 0; !error && i < base_classes; i++) {
        osdo_class *c = osdo_class_at(i);
        if (!c->replaced_by) continue;
        cb_map(&b, 2);
        cb_text(&b, "original"); cb_text(&b, c->name);
        cb_text(&b, "replacement"); cb_text(&b, c->replaced_by->name);
    }
    cb_text(&b, "sandbox");
    cb_map(&b, 2);
    cb_text(&b, "network"); cb_text(&b, net_mode);
    cb_text(&b, "filesystem"); cb_text(&b, fs_mode);
    if (error) { cb_text(&b, "error"); cb_text(&b, error); }
    send_cbuf(&b);
    cb_free(&b);
}

static void put_value(cbuf *b, const osdo_value *v)
{
    switch (v->type) {
    case 'b': cb_bool(b, v->u.b); break;
    case 'i': cb_int(b, v->u.i); break;
    case 'd': cb_double(b, v->u.d); break;
    case 's': v->u.s ? cb_text(b, v->u.s) : cb_null(b); break;
    default: cb_null(b); break;
    }
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

static void handle_call(const cv *m)
{
    int64_t id = 0;
    cv_int(m, "call", &id);
    const char *clsname = cv_text(m, "class"), *method = cv_text(m, "method");
    const cv *args = cv_get(m, "args"), *object = cv_get(m, "object");
    osdo_class *cls = clsname ? osdo_find_class(clsname) : NULL;
    cbuf b;
    cb_init(&b);
    if (!cls || !method || !object) {
        cb_map(&b, 3);
        cb_text(&b, "reply"); cb_int(&b, id);
        cb_text(&b, "ok"); cb_bool(&b, false);
        cb_text(&b, "result"); cb_text(&b, "bad call");
        send_cbuf(&b);
        cb_free(&b);
        return;
    }
    /* A fresh instance carrying the object's stored state. */
    osdo_object *o = osdo_instantiate(cls);
    const char *path = cv_text(object, "path"), *oid = cv_text(object, "id");
    if (path && osdo_is_a(o, OsdFileSystemClass)) OsdFileSystem_osdSetPath(o, path);
    if (oid) snprintf(OsdObject_data(o)->id, sizeof OsdObject_data(o)->id, "%s", oid);
    const cv *props = cv_get(object, "props");
    for (size_t i = 0; props && props->k == CV_MAP && i < props->n; i++) {
        const cv *cn = &props->items[2 * i], *kv = &props->items[2 * i + 1];
        if (cn->k != CV_TEXT || kv->k != CV_MAP) continue;
        osd_props ps;
        osd_props_init(&ps);
        for (size_t k = 0; k < kv->n; k++) {
            const cv *key = &kv->items[2 * k], *val = &kv->items[2 * k + 1];
            if (key->k != CV_TEXT) continue;
            if (val->k == CV_TEXT) osd_props_set_str(&ps, key->s, val->s);
            else if (val->k == CV_INT) osd_props_set_int(&ps, key->s, val->i);
            else if (val->k == CV_BOOL) osd_props_set_bool(&ps, key->s, val->b);
        }
        osd_state_load(o, cn->s, &ps);
        osd_props_free(&ps);
    }
    OsdObject_osdRestoreProperties(o);
    osdo_value av[16], ret;
    size_t na = 0;
    for (size_t i = 0; args && args->k == CV_ARRAY && i < args->n && na < 16; i++) {
        const cv *a = &args->items[i];
        av[na++] = a->k == CV_BOOL ? osdo_v_bool(a->b) : a->k == CV_INT ? osdo_v_int(a->i) :
                   a->k == CV_FLOAT ? osdo_v_double(a->d) : a->k == CV_TEXT ? osdo_v_string(a->s) : osdo_v_void();
    }
    osdo_status st = osdo_send(o, method, av, na, &ret);
    for (size_t i = 0; i < na; i++) osdo_value_clear(&av[i]);
    /* What the method changed: the touched keys of each class. */
    OsdObject_osdSaveState(o);
    osd_state *bag = OsdObject_data(o)->state;
    size_t nch = 0;
    for (size_t c = 0; bag && c < bag->n; c++) nch += bag->c[c].ntouched > 0;
    cb_map(&b, 4);
    cb_text(&b, "reply"); cb_int(&b, id);
    cb_text(&b, "ok"); cb_bool(&b, st == OSDO_OK);
    cb_text(&b, "result");
    if (st == OSDO_OK) put_value(&b, &ret);
    else cb_text(&b, osdo_strerror(st));
    cb_text(&b, "changes");
    cb_map(&b, nch);
    for (size_t c = 0; bag && c < bag->n; c++) {
        osd_cstate *cs = &bag->c[c];
        if (!cs->ntouched) continue;
        cb_text(&b, cs->cls);
        cb_map(&b, cs->ntouched);
        for (size_t k = 0; k < cs->ntouched; k++) {
            cb_text(&b, cs->touched[k]);
            const osd_prop *p = osd_props_get(&cs->props, cs->touched[k]);
            if (p) put_prop(&b, p);
            else cb_null(&b);   /* removed */
        }
    }
    send_cbuf(&b);
    cb_free(&b);
    osdo_value_clear(&ret);
    osdo_unref(o);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: osd-plughost PLUGIN.so DATA_DIR\n"); return 2; }
    char plugin[4096];
    if (!realpath(argv[1], plugin)) snprintf(plugin, sizeof plugin, "%s", argv[1]);
    sandbox(plugin, argv[2]);
    if (osdo_init() != OSDO_OK || osd_classes_register() != OSDO_OK) return 1;
    base_classes = osdo_class_count();
    char err[512] = "";
    osdo_status st = osdo_load_plugin(plugin, err, sizeof err);
    hello(st == OSDO_OK ? NULL : err);
    if (st != OSDO_OK) return 1;
    static unsigned char buf[1 << 20];
    for (;;) {
        ssize_t n = recv(sock, buf, sizeof buf, 0);
        if (n <= 0) break;
        cv m;
        if (cv_decode(buf, (size_t)n, &m) != 0) continue;
        if (cv_get(&m, "call")) handle_call(&m);
        cv_free(&m);
    }
    return 0;
}
