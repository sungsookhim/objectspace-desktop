/*
 * Screen readers (G4): AT-SPI over D-Bus, with sd-bus.
 *
 * The application object (the root) has the windows as children; a
 * window has one child, the element with the keyboard, as its owner (or
 * the toolkit) described it with ui_a11y_focus. Each new description is
 * a new object (its path has a sequence number), and it is announced
 * with object:state-changed:focused, as toolkits do; when a window is
 * activated, window:activate, then its element again. That is what a
 * screen reader such as Orca follows to speak where the keyboard is.
 * The rest of a window's contents is not exposed (yet): a reader can
 * follow the focus, not explore the window.
 *
 * The AT-SPI bus is the one org.a11y.Bus gives on the session bus; with
 * none (or OSD_NO_A11Y), nothing is done.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#include "ui_internal.h"

#define PREFIX "/org/a11y/atspi/accessible"
#define ROOT PREFIX "/root"
#define NULL_PATH "/org/a11y/atspi/null"

struct ui_a11y_node {
    ui_a11y_info info;         /* strings owned */
    unsigned seq;
};

static ui_app *app;
static sd_bus *bus;
static const char *me;         /* our unique name on the AT-SPI bus */
static unsigned next_id = 1, next_seq = 1;
static bool debug;

/* AT-SPI's numbers (atspi-constants.h). */
enum { R_APPLICATION = 75, R_FRAME = 23, R_DIALOG = 16, R_WINDOW = 69, R_POPUP_MENU = 41, R_MENU_ITEM = 35,
       R_LIST_ITEM = 32, R_TREE_ITEM = 91, R_PUSH_BUTTON = 43, R_CHECK_BOX = 7, R_COMBO_BOX = 11, R_ENTRY = 79,
       R_PAGE_TAB = 37, R_SPIN_BUTTON = 52, R_ICON = 26, R_DESKTOP_FRAME = 14, R_TOOL_BAR = 63, R_UNKNOWN = 67 };
enum { S_ACTIVE = 1, S_CHECKED = 4, S_EDITABLE = 7, S_ENABLED = 8, S_EXPANDABLE = 9, S_EXPANDED = 10, S_FOCUSABLE = 11,
       S_FOCUSED = 12, S_RESIZABLE = 21, S_SELECTABLE = 22, S_SELECTED = 23, S_SENSITIVE = 24, S_SHOWING = 25,
       S_SINGLE_LINE = 26, S_VISIBLE = 30, S_CHECKABLE = 41 };

static unsigned atspi_role(ui_role r)
{
    switch (r) {
    case UI_ROLE_LIST_ITEM: return R_LIST_ITEM;
    case UI_ROLE_TREE_ITEM: return R_TREE_ITEM;
    case UI_ROLE_ENTRY: return R_ENTRY;
    case UI_ROLE_BUTTON: return R_PUSH_BUTTON;
    case UI_ROLE_CHECK_BOX: return R_CHECK_BOX;
    case UI_ROLE_COMBO_BOX: return R_COMBO_BOX;
    case UI_ROLE_MENU_ITEM: return R_MENU_ITEM;
    case UI_ROLE_PAGE_TAB: return R_PAGE_TAB;
    case UI_ROLE_SPIN_BUTTON: return R_SPIN_BUTTON;
    case UI_ROLE_ICON: return R_ICON;
    case UI_ROLE_FRAME: return R_FRAME;
    case UI_ROLE_DIALOG: return R_DIALOG;
    case UI_ROLE_MENU: return R_POPUP_MENU;
    case UI_ROLE_DESKTOP: return R_DESKTOP_FRAME;
    case UI_ROLE_TOOL_BAR: return R_TOOL_BAR;
    default: return R_UNKNOWN;
    }
}

static const char *role_name(unsigned r)
{
    switch (r) {
    case R_APPLICATION: return "application";
    case R_FRAME: return "frame";
    case R_DIALOG: return "dialog";
    case R_POPUP_MENU: return "popup menu";
    case R_MENU_ITEM: return "menu item";
    case R_LIST_ITEM: return "list item";
    case R_TREE_ITEM: return "tree item";
    case R_PUSH_BUTTON: return "push button";
    case R_CHECK_BOX: return "check box";
    case R_COMBO_BOX: return "combo box";
    case R_ENTRY: return "entry";
    case R_PAGE_TAB: return "page tab";
    case R_SPIN_BUTTON: return "spin button";
    case R_ICON: return "icon";
    case R_DESKTOP_FRAME: return "desktop frame";
    case R_TOOL_BAR: return "tool bar";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------- objects */

/* What a path names: the root, a window, or a window's element. */
struct ref { int kind; ui_surface *s; };   /* kind 0 root, 1 window, 2 element; -1 none */

static bool listed(ui_surface *s) { return !s->destroyed && (s->top || s->popup) && s->configured; }

static struct ref resolve(const char *path)
{
    struct ref r = { -1, NULL };
    if (!strcmp(path, ROOT)) { r.kind = 0; return r; }
    unsigned id = 0, seq = 0;
    int n = 0;
    if (sscanf(path, PREFIX "/w%u%n", &id, &n) != 1) return r;
    const char *rest = path + n;
    for (ui_surface *s = app->surfaces; s; s = s->next) {
        if (s->a11y_id != id || !listed(s)) continue;
        if (!*rest) { r.kind = 1; r.s = s; }
        else if (sscanf(rest, "f%u", &seq) == 1 && s->a11y_focus && s->a11y_focus->seq == seq) { r.kind = 2; r.s = s; }
        break;
    }
    return r;
}

static void path_of(struct ref r, char *out, size_t n)
{
    if (r.kind == 0) snprintf(out, n, ROOT);
    else if (r.kind == 1) snprintf(out, n, PREFIX "/w%u", r.s->a11y_id);
    else if (r.kind == 2) snprintf(out, n, PREFIX "/w%uf%u", r.s->a11y_id, r.s->a11y_focus->seq);
    else snprintf(out, n, NULL_PATH);
}

static int append_ref(sd_bus_message *m, struct ref r)
{
    char p[128];
    path_of(r, p, sizeof p);
    return sd_bus_message_append(m, "(so)", r.kind < 0 ? "" : me, p);
}

static unsigned role_of(struct ref r)
{
    if (r.kind == 0) return R_APPLICATION;
    if (r.kind == 2) return atspi_role(r.s->a11y_focus->info.role);
    if (r.s->a11y_role) return atspi_role(r.s->a11y_role);
    return r.s->top ? R_FRAME : R_POPUP_MENU;
}

static const char *name_of(struct ref r)
{
    if (r.kind == 0) return program_invocation_short_name;
    if (r.kind == 2) return r.s->a11y_focus->info.name ? r.s->a11y_focus->info.name : "";
    return r.s->title;
}

static size_t windows(ui_surface **out, size_t max)
{
    size_t n = 0;
    for (ui_surface *s = app->surfaces; s; s = s->next)
        if (listed(s)) { if (out && n < max) out[n] = s; n++; }
    return n;
}

static int find_obj(sd_bus *b, const char *path, const char *iface, void *ud, void **found, sd_bus_error *e)
{
    (void)b; (void)iface; (void)ud; (void)e;
    struct ref r = resolve(path);
    if (r.kind < 0) return 0;
    static struct ref cur;   /* the handlers read it back (one call at a time) */
    cur = r;
    *found = &cur;
    return 1;
}

static int find_root(sd_bus *b, const char *path, const char *iface, void *ud, void **found, sd_bus_error *e)
{
    (void)b; (void)iface; (void)ud; (void)e;
    if (strcmp(path, ROOT)) return 0;
    *found = (void *)1;
    return 1;
}

static int find_element(sd_bus *b, const char *path, const char *iface, void *ud, void **found, sd_bus_error *e)
{
    int k = find_obj(b, path, iface, ud, found, e);
    return k > 0 && ((struct ref *)*found)->kind == 2 ? 1 : 0;
}

static int find_entry(sd_bus *b, const char *path, const char *iface, void *ud, void **found, sd_bus_error *e)
{
    int k = find_element(b, path, iface, ud, found, e);
    return k > 0 && ((struct ref *)*found)->s->a11y_focus->info.text ? 1 : 0;
}

/* ---------------------------------------------------------- Accessible */

static int p_name(sd_bus *b, const char *path, const char *i, const char *p, sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)b; (void)path; (void)i; (void)p; (void)e;
    return sd_bus_message_append(m, "s", name_of(*(struct ref *)ud));
}

static int p_desc(sd_bus *b, const char *path, const char *i, const char *p, sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)b; (void)path; (void)i; (void)p; (void)e;
    struct ref r = *(struct ref *)ud;
    const char *d = r.kind == 2 ? r.s->a11y_focus->info.description : r.kind == 1 ? r.s->a11y_desc : NULL;
    return sd_bus_message_append(m, "s", d ? d : "");
}

static struct ref parent_of(struct ref r)
{
    if (r.kind == 2) return (struct ref){ 1, r.s };
    if (r.kind == 1) return (struct ref){ 0, NULL };
    return (struct ref){ -1, NULL };
}

static int p_parent(sd_bus *b, const char *path, const char *i, const char *p, sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)b; (void)path; (void)i; (void)p; (void)e;
    struct ref r = *(struct ref *)ud;
    if (r.kind == 0) return sd_bus_message_append(m, "(so)", "org.a11y.atspi.Registry", ROOT);   /* the desktop */
    return append_ref(m, parent_of(r));
}

static int nchildren(struct ref r)
{
    if (r.kind == 0) return (int)windows(NULL, 0);
    if (r.kind == 1) return r.s->a11y_focus ? 1 : 0;
    return 0;
}

static struct ref child(struct ref r, int i)
{
    if (r.kind == 0) {
        ui_surface *w[256];
        size_t n = windows(w, 256);
        if (i >= 0 && (size_t)i < n && i < 256) return (struct ref){ 1, w[i] };
    } else if (r.kind == 1 && i == 0 && r.s->a11y_focus) {
        return (struct ref){ 2, r.s };
    }
    return (struct ref){ -1, NULL };
}

static int p_count(sd_bus *b, const char *path, const char *i, const char *p, sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)b; (void)path; (void)i; (void)p; (void)e;
    return sd_bus_message_append(m, "i", nchildren(*(struct ref *)ud));
}

static int p_empty(sd_bus *b, const char *path, const char *i, const char *p, sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)b; (void)path; (void)i; (void)p; (void)e; (void)ud;
    return sd_bus_message_append(m, "s", "");
}

static int m_child_at(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    int32_t i;
    int k = sd_bus_message_read(call, "i", &i);
    if (k < 0) return k;
    sd_bus_message *rep;
    k = sd_bus_message_new_method_return(call, &rep);
    if (k < 0) return k;
    append_ref(rep, child(*(struct ref *)ud, i));
    k = sd_bus_send(NULL, rep, NULL);
    sd_bus_message_unref(rep);
    return k;
}

static int m_children(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    struct ref r = *(struct ref *)ud;
    sd_bus_message *rep;
    int k = sd_bus_message_new_method_return(call, &rep);
    if (k < 0) return k;
    sd_bus_message_open_container(rep, 'a', "(so)");
    for (int i = 0, n = nchildren(r); i < n; i++) append_ref(rep, child(r, i));
    sd_bus_message_close_container(rep);
    k = sd_bus_send(NULL, rep, NULL);
    sd_bus_message_unref(rep);
    return k;
}

static int m_index(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    struct ref r = *(struct ref *)ud;
    int idx = r.kind == 2 ? 0 : -1;
    if (r.kind == 1) {
        ui_surface *w[256];
        size_t n = windows(w, 256);
        for (size_t i = 0; i < n && i < 256; i++) if (w[i] == r.s) idx = (int)i;
    }
    return sd_bus_reply_method_return(call, "i", idx);
}

static int m_relations(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)ud; (void)e;
    return sd_bus_reply_method_return(call, "a(ua(so))", 0);
}

static int m_role(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    return sd_bus_reply_method_return(call, "u", role_of(*(struct ref *)ud));
}

static int m_role_name(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    return sd_bus_reply_method_return(call, "s", role_name(role_of(*(struct ref *)ud)));
}

static int m_state(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    struct ref r = *(struct ref *)ud;
    uint64_t st = 0;
#define SET(b) (st |= (uint64_t)1 << (b))
    if (r.kind >= 1) { SET(S_SHOWING); SET(S_VISIBLE); }
    if (r.kind == 1) {
        SET(S_ENABLED); SET(S_SENSITIVE);
        if (r.s->activated || r.s->popup) SET(S_ACTIVE);
        if (r.s->top) SET(S_RESIZABLE);
    } else if (r.kind == 2) {
        const ui_a11y_info *f = &r.s->a11y_focus->info;
        if (!f->disabled) { SET(S_ENABLED); SET(S_SENSITIVE); }
        SET(S_FOCUSABLE);
        if (r.s->activated || r.s->popup) SET(S_FOCUSED);
        if (f->role == UI_ROLE_LIST_ITEM || f->role == UI_ROLE_TREE_ITEM || f->role == UI_ROLE_ICON) {
            SET(S_SELECTABLE);
            if (f->selected) SET(S_SELECTED);
        }
        if (f->role == UI_ROLE_CHECK_BOX) { SET(S_CHECKABLE); if (f->checked) SET(S_CHECKED); }
        if (f->expandable) { SET(S_EXPANDABLE); if (f->expanded) SET(S_EXPANDED); }
        if (f->role == UI_ROLE_ENTRY) { SET(S_EDITABLE); SET(S_SINGLE_LINE); }
    } else {
        SET(S_ENABLED); SET(S_SENSITIVE);
    }
#undef SET
    sd_bus_message *rep;
    int k = sd_bus_message_new_method_return(call, &rep);
    if (k < 0) return k;
    uint32_t v[2] = { (uint32_t)st, (uint32_t)(st >> 32) };
    sd_bus_message_append_array(rep, 'u', v, sizeof v);
    k = sd_bus_send(NULL, rep, NULL);
    sd_bus_message_unref(rep);
    return k;
}

static int m_attributes(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    struct ref r = *(struct ref *)ud;
    sd_bus_message *rep;
    int k = sd_bus_message_new_method_return(call, &rep);
    if (k < 0) return k;
    sd_bus_message_open_container(rep, 'a', "{ss}");
    sd_bus_message_append(rep, "{ss}", "toolkit", "wps-ui");
    if (r.kind == 2 && r.s->a11y_focus->info.count > 0) {
        char a[16], b[16];
        snprintf(a, sizeof a, "%d", r.s->a11y_focus->info.index + 1);
        snprintf(b, sizeof b, "%d", r.s->a11y_focus->info.count);
        sd_bus_message_append(rep, "{ss}", "posinset", a);
        sd_bus_message_append(rep, "{ss}", "setsize", b);
    }
    sd_bus_message_close_container(rep);
    k = sd_bus_send(NULL, rep, NULL);
    sd_bus_message_unref(rep);
    return k;
}

static int m_application(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)ud; (void)e;
    return sd_bus_reply_method_return(call, "(so)", me, ROOT);
}

static int m_interfaces(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    struct ref r = *(struct ref *)ud;
    sd_bus_message *rep;
    int k = sd_bus_message_new_method_return(call, &rep);
    if (k < 0) return k;
    sd_bus_message_open_container(rep, 'a', "s");
    sd_bus_message_append(rep, "s", "org.a11y.atspi.Accessible");
    if (r.kind == 0) sd_bus_message_append(rep, "s", "org.a11y.atspi.Application");
    if (r.kind >= 1) sd_bus_message_append(rep, "s", "org.a11y.atspi.Component");
    if (r.kind == 2 && r.s->a11y_focus->info.text) sd_bus_message_append(rep, "s", "org.a11y.atspi.Text");
    sd_bus_message_close_container(rep);
    k = sd_bus_send(NULL, rep, NULL);
    sd_bus_message_unref(rep);
    return k;
}

static const sd_bus_vtable accessible_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Name", "s", p_name, 0, 0),
    SD_BUS_PROPERTY("Description", "s", p_desc, 0, 0),
    SD_BUS_PROPERTY("Parent", "(so)", p_parent, 0, 0),
    SD_BUS_PROPERTY("ChildCount", "i", p_count, 0, 0),
    SD_BUS_PROPERTY("Locale", "s", p_empty, 0, 0),
    SD_BUS_PROPERTY("AccessibleId", "s", p_empty, 0, 0),
    SD_BUS_PROPERTY("HelpText", "s", p_empty, 0, 0),
    SD_BUS_METHOD("GetChildAtIndex", "i", "(so)", m_child_at, 0),
    SD_BUS_METHOD("GetChildren", "", "a(so)", m_children, 0),
    SD_BUS_METHOD("GetIndexInParent", "", "i", m_index, 0),
    SD_BUS_METHOD("GetRelationSet", "", "a(ua(so))", m_relations, 0),
    SD_BUS_METHOD("GetRole", "", "u", m_role, 0),
    SD_BUS_METHOD("GetRoleName", "", "s", m_role_name, 0),
    SD_BUS_METHOD("GetLocalizedRoleName", "", "s", m_role_name, 0),
    SD_BUS_METHOD("GetState", "", "au", m_state, 0),
    SD_BUS_METHOD("GetAttributes", "", "a{ss}", m_attributes, 0),
    SD_BUS_METHOD("GetApplication", "", "(so)", m_application, 0),
    SD_BUS_METHOD("GetInterfaces", "", "as", m_interfaces, 0),
    SD_BUS_VTABLE_END
};

/* --------------------------------------------------------- Application */

static int p_toolkit(sd_bus *b, const char *path, const char *i, const char *p, sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)b; (void)path; (void)i; (void)p; (void)ud; (void)e;
    return sd_bus_message_append(m, "s", "wps-ui");
}
static int p_version(sd_bus *b, const char *path, const char *i, const char *p, sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)b; (void)path; (void)i; (void)ud; (void)e;
    return sd_bus_message_append(m, "s", !strcmp(p, "AtspiVersion") ? "2.1" : "1.0");
}
static int32_t app_id;
static int p_id(sd_bus *b, const char *path, const char *i, const char *p, sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)b; (void)path; (void)i; (void)p; (void)ud; (void)e;
    return sd_bus_message_append(m, "i", app_id);
}
static int s_id(sd_bus *b, const char *path, const char *i, const char *p, sd_bus_message *v, void *ud, sd_bus_error *e)
{
    (void)b; (void)path; (void)i; (void)p; (void)ud; (void)e;
    return sd_bus_message_read(v, "i", &app_id);
}
static int m_locale(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)ud; (void)e;
    const char *l = getenv("LANG");
    return sd_bus_reply_method_return(call, "s", l ? l : "C");
}

static const sd_bus_vtable application_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("ToolkitName", "s", p_toolkit, 0, 0),
    SD_BUS_PROPERTY("Version", "s", p_version, 0, 0),
    SD_BUS_PROPERTY("AtspiVersion", "s", p_version, 0, 0),
    SD_BUS_WRITABLE_PROPERTY("Id", "i", p_id, s_id, 0, 0),
    SD_BUS_METHOD("GetLocale", "u", "s", m_locale, 0),
    SD_BUS_VTABLE_END
};

/* ----------------------------------------------------------- Component */

static ui_rect extent_of(struct ref r)
{
    if (r.kind == 2) return r.s->a11y_focus->info.at;
    return (ui_rect){ 0, 0, r.s->w, r.s->h };
}

static int m_extents(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    ui_rect x = extent_of(*(struct ref *)ud);   /* window coordinates: Wayland gives no screen positions */
    return sd_bus_reply_method_return(call, "(iiii)", (int32_t)x.x, (int32_t)x.y, (int32_t)x.w, (int32_t)x.h);
}
static int m_position(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    ui_rect x = extent_of(*(struct ref *)ud);
    return sd_bus_reply_method_return(call, "ii", (int32_t)x.x, (int32_t)x.y);
}
static int m_size(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    ui_rect x = extent_of(*(struct ref *)ud);
    return sd_bus_reply_method_return(call, "ii", (int32_t)x.w, (int32_t)x.h);
}
static int m_false(sd_bus_message *call, void *ud, sd_bus_error *e) { (void)ud; (void)e; return sd_bus_reply_method_return(call, "b", 0); }
static int m_at_point(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)ud; (void)e;
    return sd_bus_reply_method_return(call, "(so)", "", NULL_PATH);
}
static int m_layer(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    return sd_bus_reply_method_return(call, "u", ((struct ref *)ud)->kind == 1 ? 7u : 3u);   /* window, widget */
}
static int m_zorder(sd_bus_message *call, void *ud, sd_bus_error *e) { (void)ud; (void)e; return sd_bus_reply_method_return(call, "n", (int16_t)0); }
static int m_alpha(sd_bus_message *call, void *ud, sd_bus_error *e) { (void)ud; (void)e; return sd_bus_reply_method_return(call, "d", 1.0); }

static const sd_bus_vtable component_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Contains", "iiu", "b", m_false, 0),
    SD_BUS_METHOD("GetAccessibleAtPoint", "iiu", "(so)", m_at_point, 0),
    SD_BUS_METHOD("GetExtents", "u", "(iiii)", m_extents, 0),
    SD_BUS_METHOD("GetPosition", "u", "ii", m_position, 0),
    SD_BUS_METHOD("GetSize", "", "ii", m_size, 0),
    SD_BUS_METHOD("GetLayer", "", "u", m_layer, 0),
    SD_BUS_METHOD("GetMDIZOrder", "", "n", m_zorder, 0),
    SD_BUS_METHOD("GrabFocus", "", "b", m_false, 0),
    SD_BUS_METHOD("GetAlpha", "", "d", m_alpha, 0),
    SD_BUS_VTABLE_END
};

/* ---------------------------------------------------------------- Text */

/* Offsets are in characters. */
static size_t nchars(const char *s)
{
    size_t n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xc0) != 0x80;
    return n;
}

static const char *at_char(const char *s, size_t c)
{
    for (; *s && c; s++) if (((unsigned char)s[1] & 0xc0) != 0x80) c--;
    return s;
}

static int p_chars(sd_bus *b, const char *path, const char *i, const char *p, sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)b; (void)path; (void)i; (void)p; (void)e;
    return sd_bus_message_append(m, "i", (int32_t)nchars(((struct ref *)ud)->s->a11y_focus->info.text));
}
static int p_caret(sd_bus *b, const char *path, const char *i, const char *p, sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)b; (void)path; (void)i; (void)p; (void)e;
    return sd_bus_message_append(m, "i", (int32_t)nchars(((struct ref *)ud)->s->a11y_focus->info.text));   /* at the end */
}
static int m_text(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)e;
    int32_t a, z;
    int k = sd_bus_message_read(call, "ii", &a, &z);
    if (k < 0) return k;
    const char *t = ((struct ref *)ud)->s->a11y_focus->info.text;
    size_t n = nchars(t);
    if (a < 0) a = 0;
    if (z < 0 || (size_t)z > n) z = (int32_t)n;
    if (z < a) z = a;
    const char *p = at_char(t, (size_t)a), *q = at_char(t, (size_t)z);
    char *s = strndup(p, (size_t)(q - p));
    k = sd_bus_reply_method_return(call, "s", s);
    free(s);
    return k;
}

static const sd_bus_vtable text_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("CharacterCount", "i", p_chars, 0, 0),
    SD_BUS_PROPERTY("CaretOffset", "i", p_caret, 0, 0),
    SD_BUS_METHOD("GetText", "ii", "s", m_text, 0),
    SD_BUS_VTABLE_END
};

/* --------------------------------------------------------------- Cache */

/* libatspi asks for the whole tree at once; it is small and changes with
 * the focus, so it gets nothing here and asks object by object. */
static int m_items(sd_bus_message *call, void *ud, sd_bus_error *e)
{
    (void)ud; (void)e;
    return sd_bus_reply_method_return(call, "a((so)(so)(so)iiassusau)", 0);
}

static const sd_bus_vtable cache_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetItems", "", "a((so)(so)(so)iiassusau)", m_items, 0),
    SD_BUS_VTABLE_END
};

/* -------------------------------------------------------------- events */

static void emit(struct ref r, const char *iface, const char *member, const char *detail, int d1, const char *svalue)
{
    if (!bus || r.kind < 0) return;
    char path[128];
    path_of(r, path, sizeof path);
    sd_bus_message *m;
    if (sd_bus_message_new_signal(bus, &m, path, iface, member) < 0) return;
    sd_bus_message_append(m, "sii", detail, d1, 0);
    if (svalue) sd_bus_message_append(m, "v", "s", svalue);
    else sd_bus_message_append(m, "v", "i", 0);
    sd_bus_message_append(m, "a{sv}", 0);
    sd_bus_send(bus, m, NULL);
    sd_bus_message_unref(m);
    sd_bus_flush(bus);
    if (debug) fprintf(stderr, "wps-ui: a11y %s:%s %s %s\n", member, detail, path, name_of(r));
}

static void announce(ui_surface *s)
{
    if (s->a11y_focus && (s->activated || s->popup))
        emit((struct ref){ 2, s }, "org.a11y.atspi.Event.Object", "StateChanged", "focused", 1, NULL);
}

static void node_free(struct ui_a11y_node *n)
{
    if (!n) return;
    free((char *)n->info.name);
    free((char *)n->info.description);
    free((char *)n->info.text);
    free(n);
}

void ui_a11y_focus(ui_surface *s, const ui_a11y_info *info)
{
    if (!s) return;
    struct ui_a11y_node *old = s->a11y_focus;
    if (!info) {
        s->a11y_focus = NULL;
        node_free(old);
        return;
    }
    /* The same element (an entry being typed in, a check box ticked):
     * updated where it is, nothing said (the reader echoes keys itself). */
    if (old && old->info.role == info->role && old->info.index == info->index &&
        !strcmp(old->info.name ? old->info.name : "", info->name ? info->name : "")) {
        old->info.at = info->at;
        struct ref me_ = { 2, s };
        bool tell = bus && listed(s);
        if (tell && old->info.checked != info->checked)
            emit(me_, "org.a11y.atspi.Event.Object", "StateChanged", "checked", info->checked, NULL);
        if (tell && old->info.selected != info->selected)
            emit(me_, "org.a11y.atspi.Event.Object", "StateChanged", "selected", info->selected, NULL);
        if (tell && old->info.expanded != info->expanded)
            emit(me_, "org.a11y.atspi.Event.Object", "StateChanged", "expanded", info->expanded, NULL);
        old->info.selected = info->selected;
        old->info.checked = info->checked;
        old->info.expanded = info->expanded;
        if (strcmp(old->info.text ? old->info.text : "", info->text ? info->text : "")) {
            free((char *)old->info.text);
            old->info.text = info->text ? strdup(info->text) : NULL;
        }
        if (strcmp(old->info.description ? old->info.description : "", info->description ? info->description : "")) {
            free((char *)old->info.description);
            old->info.description = info->description ? strdup(info->description) : NULL;
            if (tell) emit(me_, "org.a11y.atspi.Event.Object", "PropertyChange", "accessible-description", 0, old->info.description);
        }
        return;
    }
    struct ui_a11y_node *n = calloc(1, sizeof *n);
    n->info = *info;
    n->info.name = info->name ? strdup(info->name) : NULL;
    n->info.description = info->description ? strdup(info->description) : NULL;
    n->info.text = info->text ? strdup(info->text) : NULL;
    n->seq = next_seq++;
    s->a11y_focus = n;
    node_free(old);
    if (bus && listed(s)) {
        emit((struct ref){ 1, s }, "org.a11y.atspi.Event.Object", "ChildrenChanged", "add", 0, NULL);
        announce(s);
    }
}

void ui_a11y_window(ui_surface *s, ui_role role, const char *description)
{
    s->a11y_role = role;
    free(s->a11y_desc);
    s->a11y_desc = description ? strdup(description) : NULL;
}

void ui_a11y_surface_new(ui_surface *s) { s->a11y_id = next_id++; }

void ui_a11y_surface_gone(ui_surface *s)
{
    if (bus && s->configured && (s->top || s->popup)) {
        emit((struct ref){ 1, s }, "org.a11y.atspi.Event.Object", "StateChanged", "defunct", 1, NULL);
        emit((struct ref){ 0, NULL }, "org.a11y.atspi.Event.Object", "ChildrenChanged", "remove", 0, NULL);
    }
    node_free(s->a11y_focus);
    s->a11y_focus = NULL;
    free(s->a11y_desc);
    s->a11y_desc = NULL;
}

void ui_a11y_activated(ui_surface *s, bool on)
{
    if (!bus) return;
    emit((struct ref){ 1, s }, "org.a11y.atspi.Event.Window", on ? "Activate" : "Deactivate", "", 0, s->title);
    if (on) announce(s);
}

/* A popup (a menu) is shown: announce it like an activated window. */
void ui_a11y_shown(ui_surface *s)
{
    if (!bus) return;
    emit((struct ref){ 0, NULL }, "org.a11y.atspi.Event.Object", "ChildrenChanged", "add", 0, NULL);
    announce(s);
}

/* ---------------------------------------------------------------- setup */

static void on_bus(void *arg, int fd, short rev)
{
    (void)arg; (void)fd; (void)rev;
    while (bus && sd_bus_process(bus, NULL) > 0) {}
}

static int embedded(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)ud; (void)e;
    if (sd_bus_message_is_method_error(m, NULL))
        fprintf(stderr, "wps-ui: accessibility: the registry refused us: %s\n", sd_bus_message_get_error(m)->message);
    else if (debug)
        fprintf(stderr, "wps-ui: accessibility: registered as %s\n", me);
    return 0;
}

void ui_a11y_init(ui_app *a)
{
    app = a;
    debug = getenv("OSD_DEBUG_A11Y") != NULL;
    if (getenv("OSD_NO_A11Y")) return;
    sd_bus *session = NULL;
    sd_bus_message *rep = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    const char *addr = NULL;
    if (sd_bus_open_user(&session) < 0) return;
    sd_bus_set_method_call_timeout(session, 2 * 1000000ull);
    if (sd_bus_call_method(session, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress", &err, &rep, "") < 0 ||
        sd_bus_message_read(rep, "s", &addr) < 0) {
        if (debug) fprintf(stderr, "wps-ui: accessibility: no AT-SPI bus: %s\n", err.message ? err.message : "?");
        goto out;
    }
    if (sd_bus_new(&bus) < 0) goto out;
    sd_bus_set_address(bus, addr);
    sd_bus_set_bus_client(bus, 1);
    if (sd_bus_start(bus) < 0) { sd_bus_unref(bus); bus = NULL; goto out; }
    sd_bus_get_unique_name(bus, &me);
    sd_bus_add_fallback_vtable(bus, NULL, PREFIX, "org.a11y.atspi.Accessible", accessible_vt, find_obj, NULL);
    sd_bus_add_fallback_vtable(bus, NULL, PREFIX, "org.a11y.atspi.Application", application_vt, find_root, NULL);
    sd_bus_add_fallback_vtable(bus, NULL, PREFIX, "org.a11y.atspi.Component", component_vt, find_obj, NULL);
    sd_bus_add_fallback_vtable(bus, NULL, PREFIX, "org.a11y.atspi.Text", text_vt, find_entry, NULL);
    sd_bus_add_object_vtable(bus, NULL, "/org/a11y/atspi/cache", "org.a11y.atspi.Cache", cache_vt, NULL);
    (void)find_element;
    sd_bus_call_method_async(bus, NULL, "org.a11y.atspi.Registry", ROOT, "org.a11y.atspi.Socket", "Embed", embedded, NULL,
                             "(so)", me, ROOT);
    sd_bus_flush(bus);
    ui_app_watch_fd(a, sd_bus_get_fd(bus), POLLIN, on_bus, NULL);
out:
    sd_bus_error_free(&err);
    sd_bus_message_unref(rep);
    sd_bus_unref(session);
}
