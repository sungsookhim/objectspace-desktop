/*
 * Drives: osdd's udisks2 client (see udisks.h).
 *
 * The bus thread owns the connection: it polls the bus and an eventfd
 * (requests from the loop thread), reads every udisks2 object
 * (GetManagedObjects) at start and again shortly after any signal from
 * udisks2, and makes the calls asked for. The list is shared under a
 * mutex; results and "changed" are posted to the loop thread.
 */
#define _GNU_SOURCE
#include "udisks.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define UD "org.freedesktop.UDisks2"
#define UD_PATH "/org/freedesktop/UDisks2"

static osdo_loop *loop;
static void (*on_changed)(void);
static pthread_t thread;
static bool running, available;
static int wake_fd = -1;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static osd_drive_info *drives;
static size_t ndrives;

/* ------------------------------------------------------------ requests */

struct req {
    struct req *next;
    char uuid[80], action[16];
    char *pass;
    osd_udisks_fn done;
    void *arg;
    /* the result */
    int err;
    char msg[256], mount[PATH_MAX];
    /* in flight */
    int stage;              /* 0: the unmount first (if mounted); 1: the drive's or container's call */
    osd_drive_info d;
};
static struct req *queue;

static void post_result_task(void *arg)
{
    struct req *r = arg;
    if (r->done) r->done(r->arg, r->err, r->msg, r->mount);
    if (r->pass) { explicit_bzero(r->pass, strlen(r->pass)); free(r->pass); }
    free(r);
}

static void finish(struct req *r, int err, const char *msg)
{
    r->err = err;
    snprintf(r->msg, sizeof r->msg, "%s", msg ? msg : "");
    osdo_loop_post(loop, OSDO_Q_RESULTS, "udisks result", post_result_task, r);
}

static void changed_task(void *arg) { (void)arg; if (on_changed) on_changed(); }

/* ------------------------------------------------- reading the objects */

struct raw {
    char path[256];
    bool block, fs, enc, drive;
    /* Block */
    char device[64], uuid[80], label[128], idtype[32], idusage[32], drive_obj[256], backing[256];
    uint64_t size;
    bool ignore, system;
    /* Filesystem */
    char mount[PATH_MAX];
    bool mounted_root;
    /* Drive */
    bool removable, ejectable, can_poweroff, optical, sd;
    char bus[16];
};

static void read_str(sd_bus_message *m, char *out, size_t n, char type)
{
    const char *s = NULL;
    if (sd_bus_message_read_basic(m, type, &s) >= 0 && s) snprintf(out, n, "%s", s);
}

static void read_bytes_str(sd_bus_message *m, char *out, size_t n)
{
    const void *p = NULL;
    size_t sz = 0;
    if (sd_bus_message_read_array(m, 'y', &p, &sz) >= 0 && p) {
        size_t k = sz < n - 1 ? sz : n - 1;
        memcpy(out, p, k);
        out[k] = 0;
    }
}

/* One property of interface `iface`, its value in a variant. */
static void read_prop(sd_bus_message *m, struct raw *r, const char *iface, const char *key)
{
    char type;
    const char *contents = NULL;
    if (sd_bus_message_peek_type(m, &type, &contents) < 0 || type != 'v') { sd_bus_message_skip(m, NULL); return; }
    if (sd_bus_message_enter_container(m, 'v', contents) < 0) return;
    bool done = false;
    int b = 0;
    if (!strcmp(iface, UD ".Block")) {
        done = true;
        if (!strcmp(key, "Device") && !strcmp(contents, "ay")) read_bytes_str(m, r->device, sizeof r->device);
        else if (!strcmp(key, "IdUUID")) read_str(m, r->uuid, sizeof r->uuid, 's');
        else if (!strcmp(key, "IdLabel")) read_str(m, r->label, sizeof r->label, 's');
        else if (!strcmp(key, "IdType")) read_str(m, r->idtype, sizeof r->idtype, 's');
        else if (!strcmp(key, "IdUsage")) read_str(m, r->idusage, sizeof r->idusage, 's');
        else if (!strcmp(key, "Drive")) read_str(m, r->drive_obj, sizeof r->drive_obj, 'o');
        else if (!strcmp(key, "CryptoBackingDevice")) read_str(m, r->backing, sizeof r->backing, 'o');
        else if (!strcmp(key, "Size")) sd_bus_message_read_basic(m, 't', &r->size);
        else if (!strcmp(key, "HintIgnore")) { sd_bus_message_read_basic(m, 'b', &b); r->ignore = b; }
        else if (!strcmp(key, "HintSystem")) { sd_bus_message_read_basic(m, 'b', &b); r->system = b; }
        else done = false;
    } else if (!strcmp(iface, UD ".Filesystem") && !strcmp(key, "MountPoints") && !strcmp(contents, "aay")) {
        done = true;
        if (sd_bus_message_enter_container(m, 'a', "ay") >= 0) {
            char mp[PATH_MAX];
            for (;;) {
                mp[0] = 0;
                const void *p = NULL;
                size_t sz = 0;
                int k = sd_bus_message_read_array(m, 'y', &p, &sz);
                if (k <= 0) break;
                size_t c = sz < sizeof mp - 1 ? sz : sizeof mp - 1;
                memcpy(mp, p, c);
                mp[c] = 0;
                if (!strcmp(mp, "/")) { r->mounted_root = true; snprintf(r->mount, sizeof r->mount, "/"); }
                else if (!r->mount[0]) snprintf(r->mount, sizeof r->mount, "%s", mp);
            }
            sd_bus_message_exit_container(m);
        }
    } else if (!strcmp(iface, UD ".Drive")) {
        done = true;
        if (!strcmp(key, "Removable")) { sd_bus_message_read_basic(m, 'b', &b); r->removable = b; }
        else if (!strcmp(key, "Ejectable")) { sd_bus_message_read_basic(m, 'b', &b); r->ejectable = b; }
        else if (!strcmp(key, "CanPowerOff")) { sd_bus_message_read_basic(m, 'b', &b); r->can_poweroff = b; }
        else if (!strcmp(key, "ConnectionBus")) read_str(m, r->bus, sizeof r->bus, 's');
        else if (!strcmp(key, "MediaCompatibility") && !strcmp(contents, "as")) {
            if (sd_bus_message_enter_container(m, 'a', "s") >= 0) {
                const char *s;
                while (sd_bus_message_read_basic(m, 's', &s) > 0) {
                    if (!strncmp(s, "optical", 7)) r->optical = true;
                    if (!strcmp(s, "flash_sd") || !strncmp(s, "flash_sd", 8)) r->sd = true;
                }
                sd_bus_message_exit_container(m);
            }
        } else done = false;
    }
    if (!done) sd_bus_message_skip(m, contents);
    sd_bus_message_exit_container(m);
}

static struct raw *find_raw(struct raw *rs, size_t n, const char *path)
{
    for (size_t i = 0; i < n; i++) if (!strcmp(rs[i].path, path)) return &rs[i];
    return NULL;
}

static bool hidden_mount(const char *mp)
{
    return !strcmp(mp, "/boot") || !strncmp(mp, "/boot/", 6);
}

static void build(struct raw *rs, size_t n, osd_drive_info **out, size_t *nout)
{
    *out = NULL;
    *nout = 0;
    for (size_t i = 0; i < n; i++) {
        struct raw *r = &rs[i];
        if (!r->block || r->ignore || !r->uuid[0]) continue;
        if (r->backing[0] && strcmp(r->backing, "/")) continue;   /* a LUKS volume's open side: shown as the container */
        bool crypto = !strcmp(r->idusage, "crypto") || r->enc;
        if (!r->fs && !crypto) continue;   /* swap, LVM, raid members, empty */
        osd_drive_info d;
        memset(&d, 0, sizeof d);
        snprintf(d.uuid, sizeof d.uuid, "%s", r->uuid);
        snprintf(d.object, sizeof d.object, "%s", r->path);
        if (r->fs) snprintf(d.fs_object, sizeof d.fs_object, "%s", r->path);
        if (r->drive_obj[0] && strcmp(r->drive_obj, "/")) snprintf(d.drive_object, sizeof d.drive_object, "%s", r->drive_obj);
        snprintf(d.device, sizeof d.device, "%s", r->device);
        snprintf(d.label, sizeof d.label, "%s", r->label);
        snprintf(d.fstype, sizeof d.fstype, "%s", r->idtype);
        d.size = r->size;
        d.system = r->system;
        snprintf(d.mount, sizeof d.mount, "%s", r->mount);
        if (crypto) {
            d.encrypted = true;
            for (size_t k = 0; k < n; k++)
                if (!strcmp(rs[k].backing, r->path)) {
                    d.unlocked = true;
                    snprintf(d.fs_object, sizeof d.fs_object, "%s", rs[k].path);
                    snprintf(d.mount, sizeof d.mount, "%s", rs[k].mount);
                    snprintf(d.fstype, sizeof d.fstype, "%s", rs[k].idtype);
                    if (!d.label[0]) snprintf(d.label, sizeof d.label, "%s", rs[k].label);
                }
        }
        if (d.mount[0] && hidden_mount(d.mount)) continue;   /* /boot */
        struct raw *dr = r->drive_obj[0] ? find_raw(rs, n, r->drive_obj) : NULL;
        if (dr) {
            d.removable = dr->removable;
            d.ejectable = dr->ejectable;
            d.can_poweroff = dr->can_poweroff;
        }
        const char *kind = "disk";
        if (dr && dr->optical) kind = "optical";
        else if (dr && (dr->sd || !strcmp(dr->bus, "sdio"))) kind = "sd";
        else if (dr && !strcmp(dr->bus, "usb")) kind = "usb";
        if (!strcmp(d.mount, "/")) kind = "root";
        snprintf(d.kind, sizeof d.kind, "%s", kind);
        *out = realloc(*out, (*nout + 1) * sizeof **out);
        (*out)[(*nout)++] = d;
    }
}

static int read_all(sd_bus *bus, osd_drive_info **out, size_t *nout)
{
    sd_bus_error e = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int k = sd_bus_call_method(bus, UD, UD_PATH, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &e, &m, "");
    sd_bus_error_free(&e);
    if (k < 0) return k;
    struct raw *rs = NULL;
    size_t n = 0;
    if (sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}") >= 0) {
        while (sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}") > 0) {
            const char *path;
            sd_bus_message_read_basic(m, 'o', &path);
            rs = realloc(rs, (n + 1) * sizeof *rs);
            struct raw *r = &rs[n++];
            memset(r, 0, sizeof *r);
            snprintf(r->path, sizeof r->path, "%s", path);
            sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
            while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
                const char *iface;
                sd_bus_message_read_basic(m, 's', &iface);
                if (!strcmp(iface, UD ".Block")) r->block = true;
                else if (!strcmp(iface, UD ".Filesystem")) r->fs = true;
                else if (!strcmp(iface, UD ".Encrypted")) r->enc = true;
                else if (!strcmp(iface, UD ".Drive")) r->drive = true;
                sd_bus_message_enter_container(m, 'a', "{sv}");
                while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
                    const char *key;
                    sd_bus_message_read_basic(m, 's', &key);
                    read_prop(m, r, iface, key);
                    sd_bus_message_exit_container(m);
                }
                sd_bus_message_exit_container(m);
                sd_bus_message_exit_container(m);
            }
            sd_bus_message_exit_container(m);
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_unref(m);
    build(rs, n, out, nout);
    free(rs);
    return 0;
}

/* Without udisks2: the mounted block-device file systems (mountinfo). */
static void read_mountinfo(osd_drive_info **out, size_t *nout)
{
    *out = NULL;
    *nout = 0;
    FILE *f = fopen("/proc/self/mountinfo", "r");
    char line[4096];
    while (f && fgets(line, sizeof line, f)) {
        char mp[PATH_MAX], root[PATH_MAX], fstype[32], src[PATH_MAX];
        char *dash = strstr(line, " - ");
        if (!dash || sscanf(line, "%*d %*d %*s %4095s %4095s", root, mp) != 2) continue;
        if (sscanf(dash + 3, "%31s %4095s", fstype, src) != 2 || strncmp(src, "/dev/", 5) || strcmp(root, "/")) continue;
        if (hidden_mount(mp)) continue;
        char uuid[80] = "";
        DIR *d = opendir("/dev/disk/by-uuid");
        for (struct dirent *e; d && (e = readdir(d));) {
            char l[PATH_MAX + 32], t[PATH_MAX];
            snprintf(l, sizeof l, "/dev/disk/by-uuid/%s", e->d_name);
            if (e->d_name[0] != '.' && realpath(l, t) && !strcmp(t, src)) snprintf(uuid, sizeof uuid, "%s", e->d_name);
        }
        if (d) closedir(d);
        if (!uuid[0]) continue;
        bool dup = false;
        for (size_t i = 0; i < *nout; i++) dup |= !strcmp((*out)[i].uuid, uuid);
        if (dup) continue;
        osd_drive_info v;
        memset(&v, 0, sizeof v);
        snprintf(v.uuid, sizeof v.uuid, "%s", uuid);
        snprintf(v.device, sizeof v.device, "%s", src);
        snprintf(v.fstype, sizeof v.fstype, "%s", fstype);
        snprintf(v.mount, sizeof v.mount, "%s", mp);
        snprintf(v.kind, sizeof v.kind, "%s", !strcmp(mp, "/") ? "root" : "disk");
        *out = realloc(*out, (*nout + 1) * sizeof **out);
        (*out)[(*nout)++] = v;
    }
    if (f) fclose(f);
}

static void publish(osd_drive_info *list, size_t n)
{
    pthread_mutex_lock(&mu);
    bool same = n == ndrives && (!n || !memcmp(list, drives, n * sizeof *list));
    free(drives);
    drives = list;
    ndrives = n;
    pthread_mutex_unlock(&mu);
    if (!same) osdo_loop_post(loop, OSDO_Q_EVENTS, "drives changed", changed_task, NULL);
}

/* --------------------------------------------------------------- calls */

static int errno_of(const sd_bus_error *e)
{
    if (!e || !e->name) return EIO;
    if (strstr(e->name, "NotAuthorized")) return EACCES;
    if (strstr(e->name, "DeviceBusy")) return EBUSY;
    if (strstr(e->name, "AlreadyMounted")) return EALREADY;
    if (strstr(e->name, "NotMounted")) return EINVAL;
    if (strstr(e->name, "Cancelled")) return ECANCELED;
    return EIO;
}

static int start(sd_bus *bus, struct req *r);
static int read_all(sd_bus *bus, osd_drive_info **out, size_t *nout);
static void publish(osd_drive_info *list, size_t n);

static int call_done(sd_bus_message *m, void *arg, sd_bus_error *ret)
{
    (void)ret;
    struct req *r = arg;
    const sd_bus_error *e = sd_bus_message_get_error(m);
    bool more = r->stage == 0 && strcmp(r->action, "unmount") && strcmp(r->action, "mount") && strcmp(r->action, "unlock");
    if (e && !(more && strstr(e->name, "NotMounted"))) {
        finish(r, errno_of(e), e->message ? e->message : e->name);
        return 0;
    }
    if (more) {   /* unmounted: now the eject, power off or lock */
        r->stage = 1;
        if (start(sd_bus_message_get_bus(m), r) < 0) finish(r, EIO, "cannot reach udisks2");
        return 0;
    }
    if (!strcmp(r->action, "mount")) {
        const char *p = NULL;
        if (sd_bus_message_read_basic(m, 's', &p) >= 0 && p) snprintf(r->mount, sizeof r->mount, "%s", p);
    }
    /* the list as it is now, before the answer (an open after an unlock
     * must find it unlocked) */
    osd_drive_info *list = NULL;
    size_t n = 0;
    if (read_all(sd_bus_message_get_bus(m), &list, &n) >= 0) publish(list, n);
    finish(r, 0, NULL);
    return 0;
}

/* One call for r: the unmount first (stage 0) for unmount, eject, power
 * off and lock when mounted; then (stage 1) the drive's or container's. */
static int start(sd_bus *bus, struct req *r)
{
    sd_bus_message *m = NULL;
    const char *a = r->action;
    const char *fsobj = r->d.fs_object;
    bool unmounts = !strcmp(a, "unmount") || !strcmp(a, "eject") || !strcmp(a, "poweroff") || !strcmp(a, "lock");
    if (r->stage == 0 && unmounts && !(fsobj[0] && r->d.mount[0])) r->stage = 1;   /* not mounted */
    int k;
    if (!strcmp(a, "mount")) {
        if (!fsobj[0]) return finish(r, EPERM, "the volume is locked: unlock it first"), 0;
        k = sd_bus_message_new_method_call(bus, &m, UD, fsobj, UD ".Filesystem", "Mount");
        if (k >= 0) k = sd_bus_message_append(m, "a{sv}", 0);
    } else if (unmounts && r->stage == 0) {
        k = sd_bus_message_new_method_call(bus, &m, UD, fsobj, UD ".Filesystem", "Unmount");
        if (k >= 0) k = sd_bus_message_append(m, "a{sv}", 0);
    } else if (!strcmp(a, "unmount")) {
        return finish(r, 0, NULL), 0;   /* (it was not mounted) */
    } else if (!strcmp(a, "eject") || !strcmp(a, "poweroff")) {
        if (!r->d.drive_object[0]) return finish(r, ENODEV, "no drive to eject"), 0;
        k = sd_bus_message_new_method_call(bus, &m, UD, r->d.drive_object, UD ".Drive", !strcmp(a, "eject") ? "Eject" : "PowerOff");
        if (k >= 0) k = sd_bus_message_append(m, "a{sv}", 0);
    } else if (!strcmp(a, "lock")) {
        if (!fsobj[0]) return finish(r, 0, NULL), 0;   /* locked already */
        k = sd_bus_message_new_method_call(bus, &m, UD, r->d.object, UD ".Encrypted", "Lock");
        if (k >= 0) k = sd_bus_message_append(m, "a{sv}", 0);
    } else if (!strcmp(a, "unlock")) {
        k = sd_bus_message_new_method_call(bus, &m, UD, r->d.object, UD ".Encrypted", "Unlock");
        if (k >= 0) k = sd_bus_message_append(m, "sa{sv}", r->pass ? r->pass : "", 0);
    } else {
        return finish(r, EINVAL, "unknown action"), 0;
    }
    if (k < 0) { sd_bus_message_unref(m); return k; }
    /* long: a password asked by polkit, a slow drive */
    k = sd_bus_call_async(bus, NULL, m, call_done, r, 120ULL * 1000000);
    sd_bus_message_unref(m);
    return k;
}

/* ---------------------------------------------------------- the thread */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint64_t refresh_at;   /* 0: none due */

static int on_signal(sd_bus_message *m, void *arg, sd_bus_error *ret)
{
    (void)m; (void)arg; (void)ret;
    if (!refresh_at) refresh_at = now_us() + 150000;   /* gathered: a plug sends many */
    return 0;
}

static sd_bus *open_bus(void)
{
    sd_bus *bus = NULL;
    const char *addr = getenv("OSD_UDISKS_BUS");
    if (addr && *addr) {
        if (sd_bus_new(&bus) < 0) return NULL;
        sd_bus_set_address(bus, addr);
        sd_bus_set_bus_client(bus, 1);
        if (sd_bus_start(bus) < 0) { sd_bus_unref(bus); return NULL; }
        return bus;
    }
    if (sd_bus_open_system(&bus) < 0) return NULL;
    return bus;
}

static void *run(void *arg)
{
    (void)arg;
    sd_bus *bus = open_bus();
    osd_drive_info *list = NULL;
    size_t n = 0;
    if (bus) sd_bus_match_signal(bus, NULL, UD, NULL, NULL, NULL, on_signal, NULL);
    if (bus && read_all(bus, &list, &n) >= 0) available = true;
    else {
        if (bus) fprintf(stderr, "osdd: udisks2 not reachable: drives from mountinfo, no mounting\n");
        read_mountinfo(&list, &n);
    }
    publish(list, n);
    while (running) {
        struct pollfd p[2] = { { wake_fd, POLLIN, 0 }, { bus ? sd_bus_get_fd(bus) : -1, 0, 0 } };
        int timeout = available ? -1 : 2000;   /* without udisks2: mountinfo now and then */
        if (bus) {
            p[1].events = (short)sd_bus_get_events(bus);
            uint64_t t = 0;
            if (sd_bus_get_timeout(bus, &t) >= 0 && t != UINT64_MAX) {
                uint64_t nw = now_us();
                int ms = t > nw ? (int)((t - nw + 999) / 1000) : 0;
                if (timeout < 0 || ms < timeout) timeout = ms;
            }
        }
        if (refresh_at) {
            uint64_t nw = now_us();
            int ms = refresh_at > nw ? (int)((refresh_at - nw + 999) / 1000) : 0;
            if (timeout < 0 || ms < timeout) timeout = ms;
        }
        poll(p, bus ? 2 : 1, timeout);
        if (p[0].revents & POLLIN) {
            uint64_t v;
            if (read(wake_fd, &v, sizeof v) < 0) {}
        }
        if (bus) while (sd_bus_process(bus, NULL) > 0) {}
        pthread_mutex_lock(&mu);
        struct req *q = queue;
        queue = NULL;
        pthread_mutex_unlock(&mu);
        while (q) {
            struct req *nx = q->next;
            if (!available || !bus) finish(q, ENOTSUP, "udisks2 is not running: drives cannot be mounted");
            else if (start(bus, q) < 0) finish(q, EIO, "cannot reach udisks2");
            q = nx;
        }
        if (available && refresh_at && now_us() >= refresh_at) {
            refresh_at = 0;
            if (read_all(bus, &list, &n) >= 0) publish(list, n);
        } else if (!available) {
            read_mountinfo(&list, &n);
            publish(list, n);
        }
    }
    if (bus) sd_bus_flush_close_unref(bus);
    return NULL;
}

void osd_udisks_init(osdo_loop *l, void (*changed)(void))
{
    loop = l;
    on_changed = changed;
    wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    running = true;
    pthread_create(&thread, NULL, run, NULL);
}

void osd_udisks_shutdown(void)
{
    if (!running) return;
    running = false;
    uint64_t one = 1;
    if (write(wake_fd, &one, sizeof one) < 0) {}
    pthread_join(thread, NULL);
    close(wake_fd);
    free(drives);
    drives = NULL;
    ndrives = 0;
}

bool osd_udisks_available(void) { return available; }

size_t osd_udisks_list(osd_drive_info **out)
{
    pthread_mutex_lock(&mu);
    size_t n = ndrives;
    *out = n ? malloc(n * sizeof **out) : NULL;
    if (n) memcpy(*out, drives, n * sizeof **out);
    pthread_mutex_unlock(&mu);
    return n;
}

bool osd_udisks_find(const char *uuid, osd_drive_info *out)
{
    bool hit = false;
    pthread_mutex_lock(&mu);
    for (size_t i = 0; i < ndrives && !hit; i++)
        if (!strcmp(drives[i].uuid, uuid)) { *out = drives[i]; hit = true; }
    pthread_mutex_unlock(&mu);
    return hit;
}

void osd_udisks_call(const char *uuid, const char *action, const char *pass, osd_udisks_fn done, void *arg)
{
    struct req *r = calloc(1, sizeof *r);
    snprintf(r->uuid, sizeof r->uuid, "%s", uuid);
    snprintf(r->action, sizeof r->action, "%s", action);
    r->pass = pass ? strdup(pass) : NULL;
    r->done = done;
    r->arg = arg;
    if (!osd_udisks_find(uuid, &r->d)) { finish(r, ENODEV, "the drive is not there"); return; }
    if (!running) { finish(r, ENOTSUP, "udisks2 is not running"); return; }
    pthread_mutex_lock(&mu);
    r->next = queue;
    queue = r;
    pthread_mutex_unlock(&mu);
    uint64_t one = 1;
    if (write(wake_fd, &one, sizeof one) < 0) {}
}

void osd_drive_title(const osd_drive_info *d, char *out, size_t n)
{
    if (!strcmp(d->kind, "root")) { snprintf(out, n, "Root"); return; }
    if (d->label[0]) { snprintf(out, n, "%s", d->label); return; }
    /* "500 GB Volume", as GNOME's Files (decimal units) */
    const char *u[] = { "bytes", "kB", "MB", "GB", "TB", "PB" };
    double s = (double)d->size;
    int i = 0;
    while (s >= 1000 && i < 5) { s /= 1000; i++; }
    const char *what = d->encrypted ? "Encrypted Volume" : "Volume";
    if (!d->size) snprintf(out, n, "%s", what);
    else if (s < 10 && i) snprintf(out, n, "%.1f %s %s", s, u[i], what);
    else snprintf(out, n, "%.0f %s %s", s, u[i], what);
}
