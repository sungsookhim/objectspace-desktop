/* wps-shell: Menus: object and folder menus, their choices, Drives' actions, emptying the Trash Can. */
#define _GNU_SOURCE
#include "shell.h"

static void drive_action(struct folder *f, const char *target, const char *action, bool reopen);
static void empty_trash_ask(struct folder *f);

/* Self-tests: the menu given here instead of shown (it chooses with menu_chosen). */
void (*menu_hook)(struct menu_ctx *m, const ui_menu_item *items, size_t n);

static void menu_ctx_free(struct menu_ctx *m)
{
    for (size_t i = 0; i < m->ntpl; i++) { free(m->tpl[i]); free(m->tpl_title[i]); }
    free(m->tpl);
    free(m->tpl_title);
    free(m);
}

char setup_folder[PATH_MAX];   /* System Setup's (the LaunchPad's first buttons) */

static void convert(const cv *arr, ui_menu_item **out, size_t *n)
{
    *n = arr && arr->k == CV_ARRAY ? arr->n : 0;
    *out = calloc(*n ? *n : 1, sizeof **out);
    for (size_t i = 0; i < *n; i++) {
        const cv *it = &arr->items[i];
        int64_t id = 0;
        cv_int(it, "id", &id);
        const cv *en = cv_get(it, "enabled"), *ck = cv_get(it, "checked");
        (*out)[i].id = id;
        (*out)[i].label = (char *)(cv_text(it, "label") ? cv_text(it, "label") : "");
        (*out)[i].enabled = !en || (en->k == CV_BOOL && en->b);
        (*out)[i].checked = ck && ck->k == CV_BOOL && ck->b;
        convert(cv_get(it, "items"), &(*out)[i].items, &(*out)[i].nitems);
    }
}

static void free_converted(ui_menu_item *m, size_t n)
{
    for (size_t i = 0; i < n; i++) free_converted(m[i].items, m[i].nitems);
    free(m);
}

static void selected_done(void *arg, const cv *reply) { opened(arg, reply); }

/* Other program... chose one (the window may have closed meanwhile). */
void open_with_chosen(long owner, const char *target, const char *app_id, bool always)
{
    struct folder *f = folder_by_id(owner);
    svc_open_with(target, app_id, -1, always, opened, f ? as_arg(f) : NULL);
}

void menu_chosen(void *arg, int64_t id)
{
    struct menu_ctx *m = arg;
    struct folder *f = folder_by_id(m->folder);
    if (f && id >= 0) {
        char t[PATH_MAX];
        target_of(f, m->item, t, sizeof t);
        switch (id) {
        case WPS_MI_OPEN: open_item(f, m->item, UI_VIEW_ICON); break;
        case WPS_MI_OPEN_ICON: open_item(f, m->item, UI_VIEW_ICON); break;
        case WPS_MI_OPEN_DETAILS: open_item(f, m->item, UI_VIEW_DETAILS); break;
        case WPS_MI_PROPERTIES: {
            /* a shadow's are its original's (OS/2): renaming there renames it */
            struct row *rw = is_tree(f) ? NULL : row_at(f, m->item);
            if (rw && rw->shadow && rw->target && *rw->target) snprintf(t, sizeof t, "%s", rw->target);
            open_properties(t);   /* (or the open one to the front) */
            break;
        }
        case WPS_MI_OPEN_OTHER: {   /* Open > Other program... */
            struct row *rw = is_tree(f) ? NULL : row_at(f, m->item);
            apppick_open(rw ? (rw->title ? rw->title : rw->name) : t, t, open_with_chosen, f->id);
            break;
        }
        case WPS_MI_VIEW_ICON: set_view(f, UI_VIEW_ICON); break;
        case WPS_MI_VIEW_DETAILS: set_view(f, UI_VIEW_DETAILS); break;
        case WPS_MI_VIEW_TREE: set_view(f, UI_VIEW_TREE); break;
        case WPS_MI_OPEN_TREE: open_item(f, m->item, UI_VIEW_TREE); break;
        case WPS_MI_ARRANGE: arrange(f); break;
        case WPS_MI_SORT_NAME: case WPS_MI_SORT_TYPE: case WPS_MI_SORT_SIZE: case WPS_MI_SORT_DATE:
            sort_icons(f, (int)(id - WPS_MI_SORT_NAME));
            break;
        case WPS_MI_FIND: findwin_open(f->path, NULL); break;
        case WPS_MI_CLOSE: win_close(f->win); break;
        case WPS_MI_OPEN_PARENT: {
            char parent[PATH_MAX];
            snprintf(parent, sizeof parent, "%s", f->path);
            char *sl = strrchr(parent, '/');
            if (sl == parent) sl[1] = 0;
            else if (sl) *sl = 0;
            if (strcmp(parent, f->path)) open_folder(parent, UI_VIEW_ICON);
            break;
        }
        case WPS_MI_DELETE: delete_request(f, m->item, false); break;
        case WPS_MI_EMPTY_TRASH: empty_trash_ask(f); break;
        case WPS_MI_MOUNT: case WPS_MI_UNMOUNT: case WPS_MI_EJECT: case WPS_MI_POWER_OFF: case WPS_MI_LOCK: case WPS_MI_UNLOCK: {
            char t[PATH_MAX];
            target_of(f, m->item, t, sizeof t);
            if (id == WPS_MI_UNLOCK) unlock_ask(f, t, false);
            else drive_action(f, t, id == WPS_MI_MOUNT ? "mount" : id == WPS_MI_UNMOUNT ? "unmount" : id == WPS_MI_EJECT ? "eject"
                                    : id == WPS_MI_POWER_OFF ? "poweroff" : "lock", false);
            break;
        }
        case WPS_MI_RESTORE: if (m->item != SIZE_MAX) restore_items(f, m->item); break;
        case WPS_MI_LOCK_POS: if (m->item != SIZE_MAX) lock_items(f, m->item); break;
        case WPS_MI_DELETE_FOREVER: delete_request(f, m->item, true); break;
        case WPS_MI_CREATE_ANOTHER: create_another(f, m->item); break;
        case WPS_MI_WINDOW_LIST: winlist_open(false); break;
        case WPS_MI_LOCKUP: session_lock("desktop"); break;
        case WPS_MI_SYSTEM_SETUP: if (setup_folder[0]) open_folder(setup_folder, UI_VIEW_ICON); break;
        case WPS_MI_SHUTDOWN: session_ask_end(app, "desktop"); break;
        case WPS_MI_MINIMIZED: winlist_open(true); break;
        default:
            if (id >= WPS_MI_OPEN_APP && id < WPS_MI_OPEN_APP + WPS_MI_OPEN_APPS) {   /* Open > one of its type's programs */
                svc_open_with(t, NULL, (int)(id - WPS_MI_OPEN_APP), false, opened, as_arg(f));
                break;
            }
            if (id >= WPS_MI_NEW && id < WPS_MI_NEW + WPS_MI_NEWS && (size_t)(id - WPS_MI_NEW) < m->ntpl) {
                /* New > a template: an object made from it here, at the click */
                struct land *l = calloc(1, sizeof *l);
                l->folder = f->id;
                snprintf(l->dest, sizeof l->dest, "%s", f->path);
                l->place = ui_container_view(f->cont) == UI_VIEW_ICON && !f->filter[0];
                if (l->place) {
                    double lx, ly;
                    ui_container_to_list(f->cont, m->x, m->y, &lx, &ly);
                    l->x = lx < 0 ? 0 : (int64_t)lx;
                    l->y = ly < 0 ? 0 : (int64_t)ly;
                }
                svc_from_template(m->tpl[id - WPS_MI_NEW], f->path, NULL, made_new, l);
                break;
            }
            svc_menu_select(t, id, selected_done, as_arg(f));
            break;
        }
    }
    menu_ctx_free(m);
}

static void drive_acted(void *arg, const cv *reply)
{
    struct drive_ctx *x = arg;
    struct folder *f = folder_by_id(x->folder);
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (ok && ok->k == CV_BOOL && ok->b) {
        const char *mp = cv_text(reply, "result");
        char m[PATH_MAX + 64];
        if (!strcmp(x->action, "mount")) snprintf(m, sizeof m, "Mounted at %s", mp && *mp ? mp : "?");
        else if (!strcmp(x->action, "unmount")) snprintf(m, sizeof m, "Unmounted");
        else if (!strcmp(x->action, "eject") || !strcmp(x->action, "poweroff")) snprintf(m, sizeof m, "It can be removed now");
        else if (!strcmp(x->action, "unlock")) snprintf(m, sizeof m, "Unlocked");
        else snprintf(m, sizeof m, "Locked");
        if (f) set_status(f->id, m);
        if (x->reopen && f) {   /* unlocked to open it: open it now */
            snprintf(f->pending_open, sizeof f->pending_open, "%s", x->target);
            svc_open(x->target, false, opened, as_arg(f));
        }
    } else {
        const char *e = reply ? cv_text(reply, "error") : NULL, *msg = reply ? cv_text(reply, "message") : NULL;
        char m[600];
        if (e && !strcmp(e, "busy"))
            snprintf(m, sizeof m, "The drive is in use. Close the windows and programs using it, then try again.");
        else if (e && !strcmp(e, "not authorized"))
            snprintf(m, sizeof m, "This needs an administrator's permission, which WPS cannot ask for yet. "
                                  "Removable drives work; for this one, use GNOME's Disks or Files.");
        else snprintf(m, sizeof m, "%s", msg && *msg ? msg : reply ? "it failed" : "lost the connection to wpsd");
        if (!strcmp(x->action, "unlock") && e && !strcmp(e, "failed") && f) {
            unlock_ask(f, x->target, x->reopen);   /* (a wrong passphrase: again) */
            set_status(f->id, "Wrong passphrase?");
        } else {
            const char *b[] = { "OK" };
            ask("Drive", m, b, 1, 0, NULL, NULL);
        }
    }
    free(x);
}

static void drive_action(struct folder *f, const char *target, const char *action, bool reopen)
{
    struct drive_ctx *x = calloc(1, sizeof *x);
    x->folder = f->id;
    x->reopen = reopen;
    snprintf(x->target, sizeof x->target, "%s", target);
    snprintf(x->action, sizeof x->action, "%s", action);
    svc_drive(target, action, NULL, drive_acted, x);
}

static void unlock_chosen(void *arg, int choice)
{
    struct drive_ctx *x = arg;
    if (choice != 0) { free(x); return; }
    snprintf(x->action, sizeof x->action, "unlock");
    svc_drive(x->target, "unlock", ui_dialog_text(x->dlg), drive_acted, x);   /* (wiped when the dialog goes) */
}

void unlock_ask(struct folder *f, const char *target, bool reopen)
{
    struct drive_ctx *x = calloc(1, sizeof *x);
    x->folder = f->id;
    x->reopen = reopen;
    snprintf(x->target, sizeof x->target, "%s", target);
    const char *b[] = { "Unlock", "Cancel" };
    events_raise("alert.warning", "", "", (ui_rect){ 0, 0, 0, 0 });
    x->dlg = ui_dialog_open_entry(app, "Unlock", "This volume is encrypted. Its passphrase:", true, b, 2, 0, unlock_chosen, x);
    last_dialog = x->dlg;
}

/* Drives came, went, were mounted or unmounted: drive objects and
 * shadows are read again (their icons and titles). */
void drives_changed_event(void)
{
    for (struct folder *f = folders; f; f = f->next) {
        if (f->closing || is_tree(f) || !f->m.complete) continue;
        bool any = false;
        for (size_t i = 0; i < f->m.total; i++) {
            struct row *rw = row_at(f, i);
            if (!rw || !rw->name || !(rw->shadow || rw->name[0] == '\x01')) continue;
            row_stale(&f->m, i);
            any = true;
        }
        if (any) fetch_visible(f, f->vis_first, f->vis_last);
    }
}

/* A trash's files folder (the Trash Can): .../files beside .../info. */
bool is_trash_dir(const char *p)
{
    size_t l = p ? strlen(p) : 0;
    if (l < 7 || strcmp(p + l - 6, "/files")) return false;
    char info[PATH_MAX];
    snprintf(info, sizeof info, "%.*s/info", (int)(l - 6), p);
    struct stat st;
    return stat(info, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Startup (v0.2-design 7.1): the XDG autostart folder. */
static void startup_dir(char *out, size_t n)
{
    const char *xc = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xc && *xc) snprintf(out, n, "%s/autostart", xc);
    else snprintf(out, n, "%s/.config/autostart", home ? home : "");
}

bool is_startup_dir(const char *p)
{
    char d[PATH_MAX], a[PATH_MAX], b[PATH_MAX];
    startup_dir(d, sizeof d);
    return p && realpath(d, a) && realpath(p, b) && !strcmp(a, b);
}

/* At login (once a session: XDG_SESSION_ID), the desktop opens the
 * shadows in Startup, as OS/2's Startup folder did; its program entries
 * the session itself runs (systemd's XDG autostart). */
static void startup_opened(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res) return;
    if (cv_text(res, "folder")) open_folder(cv_text(res, "folder"), UI_VIEW_ICON);
    else svc_reply_view(reply);
}

static void got_startup(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    for (size_t i = 0; items && items->k == CV_ARRAY && i < items->n; i++) {
        const cv *it = &items->items[i], *sh = cv_get(it, "shadow");
        const char *nm = cv_text(it, "name");
        if (!nm || nm[0] != '\x01' || !sh || sh->k != CV_BOOL || !sh->b) continue;
        svc_open(nm + 1, false, startup_opened, NULL);
    }
}

void startup_at_login(void)
{
    const char *rt = getenv("WPS_RUNTIME_DIR");
    if (!rt || !*rt) rt = getenv("XDG_RUNTIME_DIR");
    const char *sid = getenv("XDG_SESSION_ID");
    char marker[PATH_MAX], was[64] = "";
    snprintf(marker, sizeof marker, "%s/wps-startup-opened", rt && *rt ? rt : "/tmp");
    FILE *m = fopen(marker, "r");
    if (m) { if (!fgets(was, sizeof was, m)) was[0] = 0; fclose(m); }
    was[strcspn(was, "\n")] = 0;
    if (m && !strcmp(was, sid ? sid : "")) return;   /* (the desktop started again in this session) */
    m = fopen(marker, "w");
    if (m) { fprintf(m, "%s\n", sid ? sid : ""); fclose(m); }
    char d[PATH_MAX];
    startup_dir(d, sizeof d);
    svc_list(d, 0, 500, true, got_startup, NULL);
}

static void emptied(void *arg, const cv *reply)
{
    long folder = (long)(intptr_t)arg;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (!(ok && ok->k == CV_BOOL && ok->b)) {
        char m[300];
        snprintf(m, sizeof m, "Cannot empty the trash: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        set_status(folder, m);
    } else set_status(folder, "Emptying the trash...");
}

static void empty_confirmed(void *arg, int choice)
{
    last_dialog = NULL;
    if (choice == 0) svc_empty_trash(emptied, arg);
}

static void empty_trash_ask(struct folder *f)
{
    const char *buttons[] = { "Empty", "Cancel" };
    last_dialog = ask("Empty trash", "Delete everything in the trash for good?\nThis cannot be undone.", buttons, 2, 1,
                      empty_confirmed, (void *)(intptr_t)f->id);
}

static void restored(void *arg, const cv *reply)
{
    long folder = (long)(intptr_t)arg;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    char m[PATH_MAX + 64];
    if (ok && ok->k == CV_BOOL && ok->b) snprintf(m, sizeof m, "Restored to %s", cv_text(reply, "result") ? cv_text(reply, "result") : "its folder");
    else snprintf(m, sizeof m, "Cannot restore: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
    set_status(folder, m);
}

/* Restore item i (the selection if i is in it): each back where it was. */
void restore_items(struct folder *f, size_t i)
{
    bool whole = ui_container_is_selected(f->cont, i);
    for (size_t k = whole ? 0 : i; whole ? k < f->m.total : k == i; k++) {
        if (whole && !ui_container_is_selected(f->cont, k)) continue;
        const char *nm = model_name(&f->m, k);
        if (!nm) continue;
        char p[PATH_MAX];
        if (nm[0] == '\x01') snprintf(p, sizeof p, "%s", nm + 1);   /* a central object: its address */
        else snprintf(p, sizeof p, "%s/%s", f->path, nm);
        svc_restore(p, restored, (void *)(intptr_t)f->id);
    }
}

/* Lock position: the item (and the others selected with it) locked where
 * it is, or unlocked. */
void lock_items(struct folder *f, size_t i)
{
    const char *n0 = model_name(&f->m, i);
    if (!n0 || is_tree(f)) return;
    bool on = !layout_is_locked(f->lay, n0);
    bool whole = ui_container_is_selected(f->cont, i);
    for (size_t k = whole ? 0 : i; whole ? k < f->m.total : k == i; k++) {
        if (whole && !ui_container_is_selected(f->cont, k)) continue;
        const char *nm = model_name(&f->m, k);
        if (!nm) continue;
        char t[PATH_MAX];
        target_of(f, k, t, sizeof t);
        double x, y;
        layout_pos(f->lay, k, &x, &y);
        layout_place(f->lay, nm, (int)x, (int)y);
        layout_lock(f->lay, nm, on);
        svc_place_lock(f->path, t, (int64_t)x, (int64_t)y, on, placed_reply, as_arg(f));
    }
    layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
}

static void got_menu(void *arg, const cv *reply)
{
    struct menu_ctx *m = arg;
    struct folder *f = folder_by_id(m->folder);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!f || !res) { menu_ctx_free(m); return; }
    ui_menu_item *items;
    size_t n;
    convert(res, &items, &n);
    if (m->item != SIZE_MAX && is_trash_dir(f->path) && !is_tree(f)) {
        /* in the Trash Can: put back, or delete for good (not into the
         * trash again, nor Create another) */
        size_t w = 0;
        for (size_t k = 0; k < n; k++) {
            if (items[k].id == WPS_MI_DELETE || items[k].id == WPS_MI_CREATE_ANOTHER) continue;
            items[w++] = items[k];
        }
        n = w;
        items = realloc(items, (n + 3) * sizeof *items);
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_RESTORE, "Restore", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_DELETE_FOREVER, "Delete permanently...", true, false, NULL, 0 };
    }
    if (m->item != SIZE_MAX && !is_tree(f)) {
        /* a shadow of the Trash Can (the desktop's): Empty trash too */
        struct row *rw = row_at(f, m->item);
        if (rw && rw->shadow && rw->target && is_trash_dir(rw->target)) {
            items = realloc(items, (n + 1) * sizeof *items);
            items[n++] = (ui_menu_item){ WPS_MI_EMPTY_TRASH, "Empty trash", true, false, NULL, 0 };
        }
    }
    if (m->item != SIZE_MAX && !is_tree(f) && ui_container_view(f->cont) == UI_VIEW_ICON && !f->filter[0]) {
        /* Lock position (v0.3-design 5): its spot in this folder */
        const char *nm = model_name(&f->m, m->item);
        items = realloc(items, (n + 2) * sizeof *items);
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_LOCK_POS, "Lock position", nm && !f->lock_all, layout_is_locked(f->lay, nm), NULL, 0 };
    }
    if (m->item == SIZE_MAX) {
        /* Every folder's (the desktop's too), as OS/2's: Open parent after
         * Open, then Find, Sort; Close for a window (the desktop stays). */
        bool icons = ui_container_view(f->cont) == UI_VIEW_ICON;
        ui_menu_item *sorts = calloc(4, sizeof *sorts);
        sorts[0] = (ui_menu_item){ WPS_MI_SORT_NAME, "By name", icons, false, NULL, 0 };
        sorts[1] = (ui_menu_item){ WPS_MI_SORT_TYPE, "By type", icons, false, NULL, 0 };
        sorts[2] = (ui_menu_item){ WPS_MI_SORT_SIZE, "By size", icons, false, NULL, 0 };
        sorts[3] = (ui_menu_item){ WPS_MI_SORT_DATE, "By last write", icons, false, NULL, 0 };
        items = realloc(items, (n + 6) * sizeof *items);
        size_t at = n ? 1 : 0;
        memmove(items + at + 1, items + at, (n - at) * sizeof *items);
        items[at] = (ui_menu_item){ WPS_MI_OPEN_PARENT, "Open parent", strcmp(f->path, "/") != 0, false, NULL, 0 };
        n++;
        /* New > the Templates folder's templates (v0.3-design 9) */
        ui_menu_item *news = calloc(m->ntpl ? m->ntpl : 1, sizeof *news);
        for (size_t k = 0; k < m->ntpl; k++) news[k] = (ui_menu_item){ WPS_MI_NEW + (int64_t)k, m->tpl_title[k], true, false, NULL, 0 };
        items = realloc(items, (n + 7) * sizeof *items);
        memmove(items + at + 2, items + at + 1, (n - at - 1) * sizeof *items);
        items[at + 1] = (ui_menu_item){ WPS_MI_NEW_MENU, "New", m->ntpl > 0, false, news, m->ntpl };
        n++;
        items[n++] = (ui_menu_item){ WPS_MI_FIND, "Find...", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_SORT, "Sort", icons, false, sorts, 4 };
        if (!f->desktop) {
            items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
            items[n++] = (ui_menu_item){ WPS_MI_CLOSE, "Close", true, false, NULL, 0 };
        }
    }
    if (m->item == SIZE_MAX && f->desktop) {
        /* The desktop's own menu, after the folder's items, as OS/2's:
         * System setup (the folder: it will hold many objects), Lockup now,
         * Shut down (v0.3-design 2); then the window list and the minimized
         * windows (G4). */
        items = realloc(items, (n + 7) * sizeof *items);
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_SYSTEM_SETUP, "System setup", setup_folder[0] != 0, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_LOCKUP, "Lockup now", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_SHUTDOWN, "Shut down...", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_WINDOW_LIST, "Window list", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_MINIMIZED, "Minimized windows", true, false, NULL, 0 };
    }
    if (m->item == SIZE_MAX && !f->desktop) {
        /* The folder window's own menu: a View cascade after Open, to switch
         * this window's view (the Open cascade opens new windows). */
        ui_view v = ui_container_view(f->cont);
        ui_menu_item *views = calloc(3, sizeof *views);
        views[0] = (ui_menu_item){ WPS_MI_VIEW_ICON, "Icon view\tCtrl+1", true, v == UI_VIEW_ICON, NULL, 0 };
        views[1] = (ui_menu_item){ WPS_MI_VIEW_TREE, "Tree view\tCtrl+2", true, v == UI_VIEW_TREE, NULL, 0 };
        views[2] = (ui_menu_item){ WPS_MI_VIEW_DETAILS, "Details view\tCtrl+3", true, v == UI_VIEW_DETAILS, NULL, 0 };
        items = realloc(items, (n + 1) * sizeof *items);
        size_t at = n ? 1 : 0;
        memmove(items + at + 1, items + at, (n - at) * sizeof *items);
        items[at] = (ui_menu_item){ WPS_MI_VIEW, "View", true, false, views, 3 };
        n++;
    }
    /* No grab (serial 0): the button was released before the service
     * answered, and GNOME refuses a grab then, dismissing the menu at once.
     * The shell closes its own menus on clicks elsewhere. */
    if (menu_hook) { menu_hook(m, items, n); free_converted(items, n); return; }   /* (self-tests) */
    ui_menu_popup(f->win, items, n, m->x, m->y, 0, menu_chosen, m);
    free_converted(items, n);
}

static int tpl_cmp(const void *a, const void *b, void *ctx)
{
    char **t = ctx;
    return strcoll(t[*(const size_t *)a], t[*(const size_t *)b]);
}

/* A background menu: the templates first (New >), then the menu. */
static void got_templates(void *arg, const cv *reply)
{
    struct menu_ctx *m = arg;
    struct folder *f = folder_by_id(m->folder);
    if (!f) { menu_ctx_free(m); return; }
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    char tdir[PATH_MAX];
    templates_dir(tdir, sizeof tdir);
    size_t n = items && items->k == CV_ARRAY ? items->n : 0;
    if (n > WPS_MI_NEWS) n = WPS_MI_NEWS;
    char **addr = calloc(n ? n : 1, sizeof *addr), **title = calloc(n ? n : 1, sizeof *title);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *nm = cv_text(&items->items[i], "name"), *ti = cv_text(&items->items[i], "title");
        if (!nm) continue;
        if (nm[0] == '\x01') addr[k] = strdup(nm + 1);   /* a central one (Folder, Data File, Program) */
        else if (asprintf(&addr[k], "%s/%s", tdir, nm) < 0) continue;
        title[k] = strdup(ti && *ti ? ti : nm[0] == '\x01' ? "?" : nm);
        k++;
    }
    /* by title */
    size_t *o = malloc((k ? k : 1) * sizeof *o);
    for (size_t i = 0; i < k; i++) o[i] = i;
    qsort_r(o, k, sizeof *o, tpl_cmp, title);
    m->tpl = calloc(k ? k : 1, sizeof *m->tpl);
    m->tpl_title = calloc(k ? k : 1, sizeof *m->tpl_title);
    for (size_t i = 0; i < k; i++) { m->tpl[i] = addr[o[i]]; m->tpl_title[i] = title[o[i]]; }
    m->ntpl = k;
    free(o);
    free(addr);
    free(title);
    char t[PATH_MAX];
    target_of(f, m->item, t, sizeof t);
    svc_menu(t, got_menu, m);
}

void src_menu(void *ctx, size_t i, double x, double y, uint32_t serial)
{
    struct folder *f = ctx;
    struct menu_ctx *m = calloc(1, sizeof *m);
    m->folder = f->id;
    m->item = i;
    m->x = x;
    m->y = y;
    m->serial = serial;
    if (i == SIZE_MAX) {
        char tdir[PATH_MAX];
        templates_dir(tdir, sizeof tdir);
        svc_list_v(tdir, "", false, 0, WPS_MI_NEWS, true, got_templates, m);
        return;
    }
    char t[PATH_MAX];
    target_of(f, i, t, sizeof t);
    svc_menu(t, got_menu, m);
}

static void renamed(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    if (!f) return;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (ok && ok->k == CV_BOOL && ok->b) {
        const cv *res = cv_get(reply, "result");
        if (res && res->k == CV_BOOL && !res->b)
            snprintf(f->error, sizeof f->error, "No permission to rename the file: the new name is kept as its title");
        else f->error[0] = 0;
    } else {
        const char *msg = reply ? cv_text(reply, "message") : "lost the connection to wpsd";
        snprintf(f->error, sizeof f->error, "Cannot rename: %s", msg ? msg : "refused");
    }
    damage_status(f);
    reload(f);   /* events follow too; this covers a title-only change */
}

void src_rename(void *ctx, size_t i, const char *name)
{
    struct folder *f = ctx;
    char t[PATH_MAX];
    target_of(f, i, t, sizeof t);
    const char *old = row_name(f, i);
    /* A shadow has no name of its own (OS/2): renaming it renames the
     * original, and the shadow shows the new name with it. */
    struct row *rw = is_tree(f) ? NULL : row_at(f, i);
    if (rw && rw->shadow && rw->target && *rw->target) {
        if (rw->title && !strcmp(rw->title, name)) return;
        svc_rename(rw->target, name, renamed, as_arg(f));
        return;
    }
    /* its extension hidden (Show file extensions off): kept, unless typed
     * again ("x.txt" typed for x.odt is x.txt.odt: its type stays) */
    char full[NAME_MAX + 64];
    if (rw && rw->noext && row_shown(f, rw) == rw->noext) {
        const char *ext = rw->name + strlen(rw->noext);
        size_t nl = strlen(name), el = strlen(ext);
        if (nl < el || strcasecmp(name + nl - el, ext)) {
            snprintf(full, sizeof full, "%s%s", name, ext);
            name = full;
        }
    }
    if (old && !strcmp(old, name)) return;
    /* Shown at once (the service's answer and the change's event follow
     * in a moment; a refused rename reads the folder again, which puts
     * the old title back). */
    if (rw) {
        free(rw->title);
        rw->title = strdup(name);
        ui_container_changed(f->cont, i, i);
    }
    /* its place goes with it (the file's new name; a line break is a space there) */
    if (old) {
        char fsname[NAME_MAX + 1];
        snprintf(fsname, sizeof fsname, "%s", name);
        for (char *q = fsname; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
        layout_rename(f->lay, old, fsname);
    }
    svc_rename(t, name, renamed, as_arg(f));
}
