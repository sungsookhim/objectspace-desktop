/* The WarpCenter's tray: a StatusNotifierWatcher and host (tray.h). */
#define _GNU_SOURCE
#include "tray.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include "appicons.h"

#define WPATH "/StatusNotifierWatcher"
#define WIFACE "org.kde.StatusNotifierWatcher"
#define IIFACE "org.kde.StatusNotifierItem"
#define MIFACE "com.canonical.dbusmenu"
#define MAX_ITEMS 64
#define WANT_PX 22              /* the pixmap size wanted */

struct item {
    char key[600];              /* "service/path", as registered */
    char service[256], owner[256], path[300];
    char id[128], title[256], tip[256], status[16];
    char icon_name[512], att_name[512], theme_path[512];
    char menu[300];
    bool item_is_menu;
    cairo_surface_t *pixmap, *att_pixmap;
};

static ui_app *app;
static sd_bus *bus;
static void (*on_changed)(void);
static struct item *items[MAX_ITEMS];
static size_t nitems;

static void changed(void) { if (on_changed) on_changed(); }

static struct item *by_key(const char *key)
{
    for (size_t i = 0; i < nitems; i++)
        if (!strcmp(items[i]->key, key)) return items[i];
    return NULL;
}

static void item_free(struct item *it)
{
    if (it->pixmap) cairo_surface_destroy(it->pixmap);
    if (it->att_pixmap) cairo_surface_destroy(it->att_pixmap);
    free(it);
}

static void emit(const char *signal, const char *arg)
{
    if (arg) sd_bus_emit_signal(bus, WPATH, WIFACE, signal, "s", arg);
    else sd_bus_emit_signal(bus, WPATH, WIFACE, signal, "");
}

static void remove_at(size_t i)
{
    struct item *it = items[i];
    emit("StatusNotifierItemUnregistered", it->key);
    memmove(&items[i], &items[i + 1], (nitems - i - 1) * sizeof *items);
    nitems--;
    item_free(it);
    changed();
}

/* ---------------------------------------------------------- properties */

/* An a(iiay) (ARGB32, big-endian), the size nearest WANT_PX (larger ones
 * before smaller), as a cairo surface. */
static int read_pixmap(sd_bus_message *m, cairo_surface_t **out)
{
    int r = sd_bus_message_enter_container(m, 'a', "(iiay)");
    if (r < 0) return r;
    int bw = 0, bh = 0;
    uint8_t *best = NULL;
    while ((r = sd_bus_message_enter_container(m, 'r', "iiay")) > 0) {
        int32_t w, h;
        const void *data;
        size_t size;
        if ((r = sd_bus_message_read(m, "ii", &w, &h)) < 0) break;
        if ((r = sd_bus_message_read_array(m, 'y', &data, &size)) < 0) break;
        if ((r = sd_bus_message_exit_container(m)) < 0) break;
        if (w <= 0 || h <= 0 || w > 512 || h > 512 || size < (size_t)w * (size_t)h * 4) continue;
        bool better = !best || (w >= WANT_PX && (bw < WANT_PX || w < bw)) || (bw < WANT_PX && w > bw);
        if (!better) continue;
        free(best);
        best = malloc(size);
        memcpy(best, data, size);
        bw = w;
        bh = h;
    }
    if (r < 0) { free(best); return r; }
    r = sd_bus_message_exit_container(m);
    if (*out) { cairo_surface_destroy(*out); *out = NULL; }
    if (best) {
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
        unsigned char *px = cairo_image_surface_get_data(s);
        int stride = cairo_image_surface_get_stride(s);
        cairo_surface_flush(s);
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                const uint8_t *p = best + ((size_t)y * (size_t)bw + (size_t)x) * 4;
                uint32_t a = p[0], rr = p[1] * a / 255, g = p[2] * a / 255, b = p[3] * a / 255;
                *(uint32_t *)(px + y * stride + x * 4) = a << 24 | rr << 16 | g << 8 | b;
            }
        cairo_surface_mark_dirty(s);
        *out = s;
        free(best);
    }
    return r;
}

static int read_string_variant(sd_bus_message *m, char *out, size_t n)
{
    const char *contents = NULL;
    char type;
    int r = sd_bus_message_peek_type(m, &type, &contents);
    if (r < 0) return r;
    if (!contents || (strcmp(contents, "s") && strcmp(contents, "o"))) return sd_bus_message_skip(m, "v");
    if ((r = sd_bus_message_enter_container(m, 'v', contents)) < 0) return r;
    const char *s = NULL;
    if ((r = sd_bus_message_read_basic(m, contents[0], &s)) < 0) return r;
    snprintf(out, n, "%s", s ? s : "");
    return sd_bus_message_exit_container(m);
}

static int read_prop(sd_bus_message *m, struct item *it, const char *key)
{
    const char *contents = NULL;
    char type;
    int r;
    if (!strcmp(key, "Id")) return read_string_variant(m, it->id, sizeof it->id);
    if (!strcmp(key, "Title")) return read_string_variant(m, it->title, sizeof it->title);
    if (!strcmp(key, "Status")) return read_string_variant(m, it->status, sizeof it->status);
    if (!strcmp(key, "IconName")) return read_string_variant(m, it->icon_name, sizeof it->icon_name);
    if (!strcmp(key, "AttentionIconName")) return read_string_variant(m, it->att_name, sizeof it->att_name);
    if (!strcmp(key, "IconThemePath")) return read_string_variant(m, it->theme_path, sizeof it->theme_path);
    if (!strcmp(key, "Menu")) return read_string_variant(m, it->menu, sizeof it->menu);
    if ((r = sd_bus_message_peek_type(m, &type, &contents)) < 0) return r;
    if (!strcmp(key, "ItemIsMenu") && contents && !strcmp(contents, "b")) {
        int b = 0;
        if ((r = sd_bus_message_enter_container(m, 'v', "b")) < 0 || (r = sd_bus_message_read(m, "b", &b)) < 0) return r;
        it->item_is_menu = b;
        return sd_bus_message_exit_container(m);
    }
    if ((!strcmp(key, "IconPixmap") || !strcmp(key, "AttentionIconPixmap")) && contents && !strcmp(contents, "a(iiay)")) {
        if ((r = sd_bus_message_enter_container(m, 'v', "a(iiay)")) < 0) return r;
        if ((r = read_pixmap(m, key[0] == 'I' ? &it->pixmap : &it->att_pixmap)) < 0) return r;
        return sd_bus_message_exit_container(m);
    }
    if (!strcmp(key, "ToolTip") && contents && !strcmp(contents, "(sa(iiay)ss)")) {
        const char *title = NULL;
        if ((r = sd_bus_message_enter_container(m, 'v', "(sa(iiay)ss)")) < 0) return r;
        if ((r = sd_bus_message_enter_container(m, 'r', "sa(iiay)ss")) < 0) return r;
        if ((r = sd_bus_message_skip(m, "sa(iiay)")) < 0) return r;
        if ((r = sd_bus_message_read(m, "s", &title)) < 0) return r;
        snprintf(it->tip, sizeof it->tip, "%s", title ? title : "");
        if ((r = sd_bus_message_skip(m, "s")) < 0) return r;
        if ((r = sd_bus_message_exit_container(m)) < 0) return r;
        return sd_bus_message_exit_container(m);
    }
    return sd_bus_message_skip(m, "v");
}

static int got_props(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)err;
    char *key = ud;
    struct item *it = by_key(key);
    free(key);
    if (!it) return 0;
    if (sd_bus_message_is_method_error(m, NULL)) {
        fprintf(stderr, "wps-shell: tray: %s: %s\n", it->key, sd_bus_message_get_error(m)->message);
        return 0;
    }
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *k;
        if ((r = sd_bus_message_read(m, "s", &k)) < 0) break;
        if ((r = read_prop(m, it, k)) < 0) break;
        r = sd_bus_message_exit_container(m);
    }
    if (r < 0) fprintf(stderr, "wps-shell: tray: %s: bad properties (%s)\n", it->key, strerror(-r));
    changed();
    return 0;
}

static void fetch(struct item *it)
{
    sd_bus_call_method_async(bus, NULL, it->service, it->path, "org.freedesktop.DBus.Properties", "GetAll", got_props,
                             strdup(it->key), "s", IIFACE);
}

/* ----------------------------------------------------------- the watcher */

static int m_register_item(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    const char *arg;
    int r = sd_bus_message_read(m, "s", &arg);
    if (r < 0) return r;
    const char *sender = sd_bus_message_get_sender(m);
    struct item it = { 0 };
    /* a bus name (its item at /StatusNotifierItem), or (Ayatana's) a path */
    if (arg[0] == '/') {
        snprintf(it.service, sizeof it.service, "%s", sender ? sender : "");
        snprintf(it.path, sizeof it.path, "%s", arg);
    } else {
        snprintf(it.service, sizeof it.service, "%s", arg);
        snprintf(it.path, sizeof it.path, "/StatusNotifierItem");
    }
    snprintf(it.owner, sizeof it.owner, "%s", sender ? sender : it.service);
    snprintf(it.key, sizeof it.key, "%s%s", it.service, it.path);
    if (!by_key(it.key) && nitems < MAX_ITEMS) {
        struct item *n = malloc(sizeof *n);
        *n = it;
        items[nitems++] = n;
        emit("StatusNotifierItemRegistered", n->key);
        fetch(n);
    }
    return sd_bus_reply_method_return(m, "");
}

static int m_register_host(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    return sd_bus_reply_method_return(m, "");   /* (we are the host) */
}

static int p_items(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud,
                   sd_bus_error *err)
{
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    int r = sd_bus_message_open_container(reply, 'a', "s");
    for (size_t i = 0; r >= 0 && i < nitems; i++) r = sd_bus_message_append(reply, "s", items[i]->key);
    return r < 0 ? r : sd_bus_message_close_container(reply);
}

static int p_host(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud,
                  sd_bus_error *err)
{
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    return sd_bus_message_append(reply, "b", 1);
}

static int p_version(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud,
                     sd_bus_error *err)
{
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    return sd_bus_message_append(reply, "i", 0);
}

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", m_register_item, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", m_register_host, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", p_items, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", p_host, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ProtocolVersion", "i", p_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
    SD_BUS_SIGNAL("StatusNotifierHostUnregistered", "", 0),
    SD_BUS_VTABLE_END
};

/* An item's New* signals: NewStatus carries the status; the others, read
 * again. */
static int on_item_signal(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    const char *sender = sd_bus_message_get_sender(m), *path = sd_bus_message_get_path(m);
    const char *member = sd_bus_message_get_member(m);
    if (!sender || !path || !member) return 0;
    for (size_t i = 0; i < nitems; i++) {
        struct item *it = items[i];
        if (strcmp(it->owner, sender) || strcmp(it->path, path)) continue;
        const char *st;
        if (!strcmp(member, "NewStatus") && sd_bus_message_read(m, "s", &st) > 0) {
            snprintf(it->status, sizeof it->status, "%s", st);
            changed();
        } else fetch(it);
    }
    return 0;
}

/* A program left the bus: its items go. */
static int on_owner_changed(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    const char *name, *old, *now;
    if (sd_bus_message_read(m, "sss", &name, &old, &now) < 0 || (now && *now)) return 0;
    for (size_t i = nitems; i-- > 0;)
        if (!strcmp(items[i]->owner, name) || !strcmp(items[i]->service, name)) remove_at(i);
    return 0;
}

static void bus_process(void)
{
    if (!bus) return;
    int r;
    while ((r = sd_bus_process(bus, NULL)) > 0) {}
    if (r < 0) {
        fprintf(stderr, "wps-shell: tray: the bus went away (%s)\n", strerror(-r));
        ui_app_unwatch_fd(app, sd_bus_get_fd(bus));
        bus = sd_bus_flush_close_unref(bus);
        while (nitems) item_free(items[--nitems]);
        changed();
        return;
    }
    sd_bus_flush(bus);
}

static void on_bus(void *arg, int fd, short rev) { (void)arg; (void)fd; (void)rev; bus_process(); }

/* (replies to calls made outside the bus's own callbacks are read when
 * its fd is ready; what is queued is sent at once) */
static void after_call(void) { if (bus) sd_bus_flush(bus); }

bool tray_init(ui_app *a, void (*fn)(void))
{
    if (bus) return true;
    app = a;
    on_changed = fn;
    int r = sd_bus_open_user(&bus);
    if (r < 0) { fprintf(stderr, "wps-shell: tray: no session bus (%s)\n", strerror(-r)); bus = NULL; return false; }
    if ((r = sd_bus_add_object_vtable(bus, NULL, WPATH, WIFACE, vtable, NULL)) < 0 ||
        (r = sd_bus_request_name(bus, WIFACE, 0)) < 0) {
        fprintf(stderr, "wps-shell: tray: cannot serve %s (%s): another tray?\n", WIFACE, strerror(-r));
        bus = sd_bus_flush_close_unref(bus);
        return false;
    }
    char host[96];
    snprintf(host, sizeof host, "org.kde.StatusNotifierHost-%d", (int)getpid());
    sd_bus_request_name(bus, host, 0);
    sd_bus_match_signal(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                        "NameOwnerChanged", on_owner_changed, NULL);
    sd_bus_match_signal(bus, NULL, NULL, NULL, IIFACE, NULL, on_item_signal, NULL);
    ui_app_watch_fd(app, sd_bus_get_fd(bus), POLLIN, on_bus, NULL);
    emit("StatusNotifierHostRegistered", NULL);
    bus_process();
    fprintf(stderr, "wps-shell: tray: serving %s\n", WIFACE);
    return true;
}

bool tray_running(void) { return bus != NULL; }

/* ------------------------------------------------------------- drawing */

static bool shown(const struct item *it) { return strcmp(it->status, "Passive") != 0; }

static struct item *shown_at(size_t i)
{
    for (size_t k = 0; k < nitems; k++)
        if (shown(items[k]) && i-- == 0) return items[k];
    return NULL;
}

size_t tray_count(void)
{
    size_t n = 0;
    for (size_t k = 0; k < nitems; k++) n += shown(items[k]);
    return n;
}

/* A theme name in the item's own IconThemePath, as a file. */
static bool in_theme_path(const char *dir, const char *name, char *out, size_t n)
{
    static const char *const sub[] = { "", "/hicolor/22x22/apps", "/hicolor/22x22/status", "/hicolor/24x24/apps",
                                       "/hicolor/24x24/status", "/hicolor/32x32/apps", "/hicolor/32x32/status",
                                       "/hicolor/48x48/apps", "/hicolor/48x48/status", "/hicolor/16x16/apps",
                                       "/hicolor/16x16/status", "/hicolor/scalable/apps", "/hicolor/scalable/status" };
    static const char *const ext[] = { ".png", ".svg" };
    for (size_t i = 0; i < sizeof sub / sizeof *sub; i++)
        for (size_t e = 0; e < 2; e++) {
            snprintf(out, n, "%s%s/%s%s", dir, sub[i], name, ext[e]);
            if (access(out, R_OK) == 0) return true;
        }
    return false;
}

static cairo_surface_t *named(const struct item *it, const char *name)
{
    if (!name[0]) return NULL;
    char p[PATH_MAX];
    if (it->theme_path[0] && name[0] != '/' && in_theme_path(it->theme_path, name, p, sizeof p)) return appicon_named(p);
    return appicon_named(name);
}

cairo_surface_t *tray_icon(size_t i)
{
    struct item *it = shown_at(i);
    if (!it) return NULL;
    cairo_surface_t *s = NULL;
    if (!strcmp(it->status, "NeedsAttention")) {
        if ((s = named(it, it->att_name))) return s;
        if (it->att_pixmap) return it->att_pixmap;
    }
    if ((s = named(it, it->icon_name))) return s;
    return it->pixmap;
}

const char *tray_title(size_t i)
{
    struct item *it = shown_at(i);
    return !it ? NULL : it->tip[0] ? it->tip : it->title[0] ? it->title : it->id;
}

/* ---------------------------------------------------------------- menus */

struct pending_menu {
    char key[600];
    ui_surface *on;
    ui_rect at;
    uint32_t serial;
};

static char menu_service[256], menu_path[300];   /* the menu shown */

/* "_File" -> "File"; "__" -> "_" */
static char *unmnemonic(const char *s)
{
    char *o = malloc(strlen(s) + 1), *w = o;
    for (; *s; s++) {
        if (*s == '_') { if (s[1] == '_') *w++ = *s++; continue; }
        *w++ = *s;
    }
    *w = 0;
    return o;
}

/* A dbusmenu node (ia{sv}av): its children as WPS menu items. */
static int read_node(sd_bus_message *m, int32_t *id, ui_menu_item *self, bool *visible, ui_menu_item **kids, size_t *nkids)
{
    int r = sd_bus_message_enter_container(m, 'r', "ia{sv}av");
    if (r <= 0) return r < 0 ? r : -EINVAL;
    if ((r = sd_bus_message_read(m, "i", id)) < 0) return r;
    char label[512] = "", ty[32] = "", toggle[32] = "";
    int enabled = 1, vis = 1, state = 0;
    if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *k, *contents = NULL;
        char t;
        if ((r = sd_bus_message_read(m, "s", &k)) < 0) return r;
        if ((r = sd_bus_message_peek_type(m, &t, &contents)) < 0) return r;
        if (!strcmp(k, "label")) r = read_string_variant(m, label, sizeof label);
        else if (!strcmp(k, "type")) r = read_string_variant(m, ty, sizeof ty);
        else if (!strcmp(k, "toggle-type")) r = read_string_variant(m, toggle, sizeof toggle);
        else if ((!strcmp(k, "enabled") || !strcmp(k, "visible")) && contents && !strcmp(contents, "b")) {
            int b;
            if ((r = sd_bus_message_enter_container(m, 'v', "b")) < 0 || (r = sd_bus_message_read(m, "b", &b)) < 0) return r;
            if (k[0] == 'e') enabled = b; else vis = b;
            r = sd_bus_message_exit_container(m);
        } else if (!strcmp(k, "toggle-state") && contents && !strcmp(contents, "i")) {
            if ((r = sd_bus_message_enter_container(m, 'v', "i")) < 0 || (r = sd_bus_message_read(m, "i", &state)) < 0) return r;
            r = sd_bus_message_exit_container(m);
        } else r = sd_bus_message_skip(m, "v");
        if (r < 0) return r;
        if ((r = sd_bus_message_exit_container(m)) < 0) return r;
    }
    if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0) return r;
    /* children */
    if ((r = sd_bus_message_enter_container(m, 'a', "v")) < 0) return r;
    ui_menu_item *list = NULL;
    size_t n = 0;
    while ((r = sd_bus_message_enter_container(m, 'v', "(ia{sv}av)")) > 0) {
        int32_t cid;
        ui_menu_item c = { 0 };
        bool cvis = true;
        ui_menu_item *ck = NULL;
        size_t nck = 0;
        if ((r = read_node(m, &cid, &c, &cvis, &ck, &nck)) < 0) { ui_menu_items_free(list, n); return r; }
        if ((r = sd_bus_message_exit_container(m)) < 0) { ui_menu_items_free(list, n); return r; }
        if (!cvis) { free(c.label); ui_menu_items_free(ck, nck); continue; }
        c.items = ck;
        c.nitems = nck;
        list = realloc(list, (n + 1) * sizeof *list);
        list[n++] = c;
    }
    if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0 || (r = sd_bus_message_exit_container(m)) < 0) {
        ui_menu_items_free(list, n);
        return r;
    }
    bool sep = !strcmp(ty, "separator");
    self->id = *id;
    self->label = sep ? strdup("-") : unmnemonic(label);
    self->enabled = enabled && !sep;
    self->checked = toggle[0] && state == 1;
    *visible = vis;
    *kids = list;
    *nkids = n;
    return 1;
}

static void menu_chosen(void *arg, int64_t id)
{
    (void)arg;
    if (id < 0 || !bus || !menu_path[0]) return;
    sd_bus_call_method_async(bus, NULL, menu_service, menu_path, MIFACE, "Event", NULL, NULL, "isvu", (int32_t)id,
                             "clicked", "i", (int32_t)0, (uint32_t)time(NULL));
    after_call();
}

static int got_layout(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)err;
    struct pending_menu *pm = ud;
    struct item *it = by_key(pm->key);
    if (!it || sd_bus_message_is_method_error(m, NULL)) {
        if (it) fprintf(stderr, "wps-shell: tray: %s: no menu (%s)\n", it->key, sd_bus_message_get_error(m)->message);
        free(pm);
        return 0;
    }
    uint32_t rev;
    int32_t id;
    ui_menu_item root = { 0 }, *kids = NULL;
    size_t nkids = 0;
    bool vis;
    int r = sd_bus_message_read(m, "u", &rev);
    if (r >= 0) r = read_node(m, &id, &root, &vis, &kids, &nkids);
    free(root.label);
    if (r < 0) fprintf(stderr, "wps-shell: tray: %s: bad menu (%s)\n", it->key, strerror(-r));
    else if (nkids) {
        snprintf(menu_service, sizeof menu_service, "%s", it->service);
        snprintf(menu_path, sizeof menu_path, "%s", it->menu);
        ui_menu_popup(pm->on, kids, nkids, pm->at.x, pm->at.y, pm->serial, menu_chosen, NULL);
    }
    ui_menu_items_free(kids, nkids);
    free(pm);
    return 0;
}

static void show_menu(struct item *it, ui_surface *on, ui_rect at, uint32_t serial)
{
    if (!it->menu[0]) {   /* the program shows its own */
        sd_bus_call_method_async(bus, NULL, it->service, it->path, IIFACE, "ContextMenu", NULL, NULL, "ii", 0, 0);
        after_call();
        return;
    }
    struct pending_menu *pm = calloc(1, sizeof *pm);
    snprintf(pm->key, sizeof pm->key, "%s", it->key);
    pm->on = on;
    pm->at = at;
    pm->serial = serial;
    sd_bus_call_method_async(bus, NULL, it->service, it->menu, MIFACE, "AboutToShow", NULL, NULL, "i", 0);
    sd_bus_call_method_async(bus, NULL, it->service, it->menu, MIFACE, "GetLayout", got_layout, pm, "iias", 0, -1, 0);
    after_call();
}

struct pending_activate { char key[600]; ui_surface *on; ui_rect at; uint32_t serial; };

/* Activate failed (not offered): the menu instead. */
static int activated(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)err;
    struct pending_activate *pa = ud;
    struct item *it = by_key(pa->key);
    if (it && sd_bus_message_is_method_error(m, NULL)) show_menu(it, pa->on, pa->at, pa->serial);
    free(pa);
    return 0;
}

void tray_press(size_t i, int button, ui_surface *on, ui_rect at, uint32_t serial)
{
    struct item *it = shown_at(i);
    if (!it || !bus) return;
    if (button == UI_BTN2 || (button == UI_BTN1 && it->item_is_menu)) { show_menu(it, on, at, serial); return; }
    if (button == UI_BTN3) {
        sd_bus_call_method_async(bus, NULL, it->service, it->path, IIFACE, "SecondaryActivate", NULL, NULL, "ii", 0, 0);
    } else {
        struct pending_activate *pa = calloc(1, sizeof *pa);
        snprintf(pa->key, sizeof pa->key, "%s", it->key);
        pa->on = on;
        pa->at = at;
        pa->serial = serial;
        sd_bus_call_method_async(bus, NULL, it->service, it->path, IIFACE, "Activate", activated, pa, "ii", 0, 0);
    }
    after_call();
}
