/* wps-shell: The desktop: the user's folders, the Trash corner, the first start, windows restored at login, opening windows. */
#define _GNU_SOURCE
#include "shell.h"

/* An XDG user folder (XDG_<key>_DIR in user-dirs.dirs), else ~/<fallback>. */
void user_dir(const char *key, const char *fallback, char *out, size_t n)
{
    const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
    char cfg[PATH_MAX], line[PATH_MAX + 64], want[64];
    if (xdg && *xdg) snprintf(cfg, sizeof cfg, "%s/user-dirs.dirs", xdg);
    else snprintf(cfg, sizeof cfg, "%s/.config/user-dirs.dirs", home ? home : "");
    snprintf(out, n, "%s/%s", home ? home : "", fallback);
    snprintf(want, sizeof want, "XDG_%s_DIR=\"", key);
    size_t wl = strlen(want);
    FILE *fp = fopen(cfg, "r");
    while (fp && fgets(line, sizeof line, fp)) {
        if (strncmp(line, want, wl)) continue;
        char *v = line + wl, *e = strrchr(v, '"');
        if (!e) break;
        *e = 0;
        if (!strncmp(v, "$HOME", 5)) snprintf(out, n, "%s%s", home ? home : "", v + 5);
        else if (v[0] == '/') snprintf(out, n, "%s", v);
        break;
    }
    if (fp) fclose(fp);
    mkdir(out, 0755);   /* a fresh account may not have one yet */
}

/* The desktop folder: XDG_DESKTOP_DIR, else ~/Desktop. */
static void desktop_dir(char *out, size_t n) { user_dir("DESKTOP", "Desktop", out, n); }

/* The desktop's first start (the Templates and System folders' shadows,
 * and 0.2's objects): wpsd does it, once each step, versioned. */
/* First-start step 7 (0.2): the Trash Can in the desktop's lower right
 * corner, as on OS/2. Done by the desktop, which knows its size: a moment
 * after the first start (the WarpCenter has taken its edge by then, and
 * the shadow is made), tried again a few times while it is not there. */
static int corner_timer = -1, corner_tries;

static bool trash_to_corner(void)
{
    struct folder *f = NULL;
    for (struct folder *o = folders; o && !f; o = o->next) if (o->desktop && !o->closing) f = o;
    if (!f || !f->m.complete) return false;
    for (size_t i = 0; i < f->m.total; i++) {
        struct row *rw = row_at(f, i);
        const char *nm = model_name(&f->m, i);
        if (!rw || !nm || !rw->shadow || !rw->target || !is_trash_dir(rw->target)) continue;
        ui_rect c = ui_frame_client(f->win);
        const ui_icon_set *set = folder_icons(f);
        int64_t x = (int64_t)(c.w - set->cell_w - 8), y = (int64_t)(c.h - set->cell_h - 8);
        if (x < 0 || y < 0) return false;
        char addr[PATH_MAX];
        if (nm[0] == '\x01') snprintf(addr, sizeof addr, "%s", nm + 1);
        else snprintf(addr, sizeof addr, "%s/%s", f->path, nm);
        const char *t[1] = { addr };
        layout_place(f->lay, nm, (int)x, (int)y);
        folder_place(f, f->path, t, &x, &y, 1, placed_reply, as_arg(f));
        layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
        ui_container_layout_changed(f->cont);
        ui_surface_damage_all(f->win);
        return true;
    }
    return false;
}

static void on_corner_timer(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    uint64_t x;
    if (read(fd, &x, sizeof x) < 0) {}
    if (trash_to_corner() || ++corner_tries >= 8) {
        ui_app_unwatch_fd(app, fd);
        close(fd);
        corner_timer = -1;
    }
}

static void first_started(void *arg, const cv *reply)
{
    (void)arg;
    for (struct folder *f = folders; f; f = f->next)
        if (f->desktop && !f->closing) bg_follow(f);   /* its Background (its own, or GNOME's) */
    if (!getenv("WPS_NO_STARTUP")) startup_at_login();
    int64_t from = -1;
    if (!reply || !cv_int(reply, "result", &from) || from >= 7 || corner_timer >= 0) return;
    corner_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (corner_timer < 0) return;
    struct itimerspec its = { { 1, 500000000 }, { 2, 0 } };
    timerfd_settime(corner_timer, 0, &its, NULL);
    ui_app_watch_fd(app, corner_timer, POLLIN, on_corner_timer, NULL);
}

static void desktop_first_start(const char *desk) { svc_first_start(desk, first_started, NULL); }

/* ---- open windows restored at login (v0.2-design 7.2)
 *
 * The desktop's own windows - folder windows (folder, view, size, place),
 * Find windows (folder, name), notebooks (object, page) - are written to
 * <state>/open-windows when they change (looked at every 2 s, so a crash
 * loses little; the end of a session closes none of them, so the list
 * stands as it was). At the next login (once a session) they open again;
 * in the WPS session the compositor puts each where it was ("place"),
 * inside GNOME GNOME places them. Lines: F path view w h x y maximized,
 * S folder name x y, N target page x y (x -1: not known). */
static int rec_timer = -1;

static char *rec_last;

static void open_windows_file(char *out, size_t n)
{
    const char *sd = getenv("WPS_STATE_DIR"), *xs = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char dir[PATH_MAX];
    if (sd && *sd) snprintf(dir, sizeof dir, "%s", sd);
    else if (xs && *xs) snprintf(dir, sizeof dir, "%s/wps", xs);
    else snprintf(dir, sizeof dir, "%s/.local/state/wps", home ? home : "");
    char *q = dir;
    while ((q = strchr(q + 1, '/'))) { *q = 0; mkdir(dir, 0700); *q = '/'; }
    mkdir(dir, 0700);
    snprintf(out, n, "%s/open-windows", dir);
}

static bool plain(const char *s) { return s && !strpbrk(s, "\t\n"); }

static void rec_add(struct rec *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char line[PATH_MAX * 2 + 128];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0 || (size_t)n >= sizeof line) return;
    if (r->len + (size_t)n + 1 > r->cap) {
        r->cap = (r->len + (size_t)n + 1) * 2;
        r->buf = realloc(r->buf, r->cap);
    }
    memcpy(r->buf + r->len, line, (size_t)n + 1);
    r->len += (size_t)n;
}

static void where_is(ui_surface *s, const char *app_id, int *x, int *y, bool *max)
{
    *x = *y = -1;
    *max = false;
    if (!winlist_where(app_id, ui_window_title(s), x, y, max)) *x = *y = -1;
}

static void rec_find(void *ctx, const char *folder, const char *name, ui_surface *s)
{
    int x, y;
    bool mx;
    where_is(s, "org.wps.Find", &x, &y, &mx);
    if (plain(folder) && plain(name)) rec_add(ctx, "S\t%s\t%s\t%d\t%d\n", folder, name, x, y);
}

static char *describe_windows(void)
{
    struct rec r = { 0 };
    rec_add(&r, "# WPS's open windows (the desktop writes this; they reopen at login)\n");
    /* oldest first: they reopen in the same order (the list is newest first) */
    size_t n = 0;
    for (struct folder *f = folders; f; f = f->next) n++;
    struct folder **all = calloc(n ? n : 1, sizeof *all);
    n = 0;
    for (struct folder *f = folders; f; f = f->next) all[n++] = f;
    for (size_t i = n; i-- > 0;) {
        struct folder *f = all[i];
        if (f->desktop || f->closing || !plain(f->path)) continue;
        int x, y;
        bool mx;
        where_is(f->win, "org.wps.shell", &x, &y, &mx);
        rec_add(&r, "F\t%s\t%d\t%d\t%d\t%d\t%d\t%d\n", f->path, (int)ui_container_view(f->cont), (int)ui_surface_width(f->win),
                (int)ui_surface_height(f->win), x, y, mx || ui_window_maximized(f->win));
    }
    free(all);
    findwin_each(rec_find, &r);
    for (struct props *p = props_open; p; p = p->next) {
        if (!p->nb || !plain(p->target)) continue;
        int x, y;
        bool mx;
        where_is(ui_notebook_surface(p->nb), "org.wps.notebook", &x, &y, &mx);
        rec_add(&r, "N\t%s\t%zu\t%d\t%d\n", p->target, ui_notebook_page(p->nb), x, y);
    }
    return r.buf;
}

static void on_rec_timer(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    uint64_t x;
    if (read(fd, &x, sizeof x) < 0) {}
    char *now = describe_windows();
    if (!now || (rec_last && !strcmp(now, rec_last))) { free(now); return; }
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    open_windows_file(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (f && fputs(now, f) >= 0 && fclose(f) == 0 && rename(tmp, path) == 0) { free(rec_last); rec_last = now; return; }
    if (f) unlink(tmp);
    free(now);
}

static void start_recording(void)
{
    if (rec_timer >= 0) return;
    rec_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (rec_timer < 0) return;
    struct itimerspec its = { { 2, 0 }, { 2, 0 } };
    timerfd_settime(rec_timer, 0, &its, NULL);
    ui_app_watch_fd(app, rec_timer, POLLIN, on_rec_timer, NULL);
}

static void place_next(const char *app_id, const char *title, int x, int y)
{
    if (x < 0 && y < 0) return;
    char arg[600];
    snprintf(arg, sizeof arg, "%s\t%s\t%d\t%d", app_id, plain(title) ? title : "", x, y);
    winlist_ext("place", arg);   /* (the WPS session's compositor; GNOME's extension ignores it) */
}

/* At login: what was open opens again (once a session), then the
 * recording goes on. */
void windows_at_login(void)
{
    const char *rt = getenv("WPS_RUNTIME_DIR");
    if (!rt || !*rt) rt = getenv("XDG_RUNTIME_DIR");
    const char *sid = getenv("XDG_SESSION_ID");
    char marker[PATH_MAX], was[64] = "";
    snprintf(marker, sizeof marker, "%s/wps-windows-restored", rt && *rt ? rt : "/tmp");
    FILE *m = fopen(marker, "r");
    if (m) { if (!fgets(was, sizeof was, m)) was[0] = 0; fclose(m); }
    was[strcspn(was, "\n")] = 0;
    bool again = m && !strcmp(was, sid ? sid : "");   /* (the desktop started again in this session) */
    if ((m = fopen(marker, "w"))) { fprintf(m, "%s\n", sid ? sid : ""); fclose(m); }
    char path[PATH_MAX];
    open_windows_file(path, sizeof path);
    FILE *f = again || !restore_windows || getenv("WPS_NO_RESTORE") ? NULL : fopen(path, "r");
    char line[PATH_MAX * 2 + 128];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *fl[9] = { 0 };
        int k = 0;
        for (char *p = line, *t; k < 9 && (t = strsep(&p, "\t"));) fl[k++] = t;
        struct stat st;
        if (k >= 8 && !strcmp(fl[0], "F") && stat(fl[1], &st) == 0 && S_ISDIR(st.st_mode)) {
            int v = atoi(fl[2]);
            next_w = atof(fl[3]);
            next_h = atof(fl[4]);
            struct folder *nf = open_window(fl[1], v == 1 ? UI_VIEW_DETAILS : v == 2 ? UI_VIEW_TREE : UI_VIEW_ICON, false);
            /* (asked once it has its title; it shows only after a round trip) */
            if (nf) place_next("org.wps.shell", ui_window_title(nf->win), atoi(fl[5]), atoi(fl[6]));
            if (nf && atoi(fl[7])) ui_window_set_maximized(nf->win);
        } else if (k >= 5 && !strcmp(fl[0], "S")) {
            place_next("org.wps.Find", "Find", atoi(fl[3]), atoi(fl[4]));
            findwin_open(fl[1], fl[2]);
        } else if (k >= 5 && !strcmp(fl[0], "N")) {
            place_next("org.wps.notebook", "", atoi(fl[3]), atoi(fl[4]));   /* (its title comes with its pages) */
            if (open_properties(fl[1])) props_open->want_page = (size_t)atoll(fl[2]);
        }
    }
    if (f) fclose(f);
    start_recording();   /* (also with restoring off: turned on again, it has the list) */
}

/* ---- folder windows as they were last closed (0.4 step 5)
 *
 * Each view's window is kept on the folder (per user: wpsd's properties,
 * with no field): win_icon, win_details, win_tree as "x,y,w,h,maximized,
 * scroll" (x -1: not known, as inside GNOME), and win_view, the view it
 * was last closed in (what a plain Open opens). In the WPS session the
 * compositor puts it back in its place ("place"); GNOME places it itself. */
static const char *const view_keys[3] = { "icon", "details", "tree" };

static struct folder *open_in(const char *path, ui_view view)
{
    for (struct folder *o = folders; o; o = o->next)
        if (!o->closing && !o->desktop && !strcmp(o->path, path) && (view == VIEW_LAST || ui_container_view(o->cont) == view)) return o;
    return NULL;
}

struct win_req { struct win_req *next; char path[PATH_MAX]; ui_view view; };
static struct win_req *win_reqs;

static void got_win(void *arg, const cv *reply)
{
    struct win_req *r = arg;
    for (struct win_req **pp = &win_reqs; *pp; pp = &(*pp)->next)
        if (*pp == r) { *pp = r->next; break; }
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    char geo[3][128] = { "", "", "" }, last[16] = "";
    for (size_t i = 0; res && res->k == CV_ARRAY && i < res->n; i++) {
        const char *k = cv_text(&res->items[i], "name"), *v = cv_text(&res->items[i], "value");
        if (!k || !v) continue;
        if (!strcmp(k, "win_view")) snprintf(last, sizeof last, "%s", v);
        for (int j = 0; j < 3; j++)
            if (!strncmp(k, "win_", 4) && !strcmp(k + 4, view_keys[j])) snprintf(geo[j], sizeof geo[j], "%s", v);
    }
    ui_view v = r->view;
    if (v == VIEW_LAST) v = !strcmp(last, "details") ? UI_VIEW_DETAILS : !strcmp(last, "tree") ? UI_VIEW_TREE : UI_VIEW_ICON;
    struct folder *f = open_in(r->path, v);
    if (f) { ui_window_activate(f->win); free(r); return; }   /* (opened meanwhile) */
    int x = -1, y = -1, mx = 0;
    double w = 0, h = 0, sc = 0;
    bool have = sscanf(geo[v], "%d,%d,%lf,%lf,%d,%lf", &x, &y, &w, &h, &mx, &sc) >= 4 && w >= 100 && h >= 80;
    if (have) { next_w = w; next_h = h; }
    f = open_window(r->path, v, false);
    if (f && have) {
        place_next("org.wps.shell", ui_window_title(f->win), x, y);
        if (mx) ui_window_set_maximized(f->win);
        f->want_scroll = v == UI_VIEW_TREE ? 0 : sc;
    }
    free(r);
}

struct folder *open_folder(const char *path, ui_view view)
{
    char real[PATH_MAX];
    const char *want = realpath(path, real) ? real : path;
    struct folder *o = open_in(want, view);
    if (o) { ui_window_activate(o->win); return o; }   /* open already: switch to it, as OS/2 did */
    for (struct win_req *r = win_reqs; r; r = r->next)
        if (!strcmp(r->path, want) && r->view == view) return NULL;   /* (asked already: a second click) */
    struct win_req *r = calloc(1, sizeof *r);
    snprintf(r->path, sizeof r->path, "%s", want);
    r->view = view;
    r->next = win_reqs;
    win_reqs = r;
    svc_props(want, got_win, r);
    return NULL;
}

/* Closing: where it is, how big, how far scrolled, for the next Open. */
void folder_window_save(struct folder *f)
{
    if (f->desktop || !f->win || !plain(f->path)) return;
    ui_view v = ui_container_view(f->cont);
    int x = -1, y = -1;
    bool mx = false;
    if (!winlist_where("org.wps.shell", ui_window_title(f->win), &x, &y, &mx)) x = y = -1;
    mx = mx || ui_window_maximized(f->win);
    double w = ui_surface_width(f->win), h = ui_surface_height(f->win);
    if (mx && f->open_w > 0) { w = f->open_w; h = f->open_h; }   /* (its size when not maximized) */
    char key[16], val[128];
    snprintf(key, sizeof key, "win_%s", view_keys[v]);
    snprintf(val, sizeof val, "%d,%d,%.0f,%.0f,%d,%.0f", x, y, w, h, mx, v == UI_VIEW_TREE ? 0.0 : ui_container_scroll_y(f->cont));
    cv a = { .k = CV_TEXT, .s = val, .len = strlen(val) }, b = { .k = CV_TEXT, .s = (char *)view_keys[v], .len = strlen(view_keys[v]) };
    svc_settings_set(f->path, key, &a, -1, ignore_reply, NULL);
    svc_settings_set(f->path, "win_view", &b, -1, ignore_reply, NULL);
}

/* Scrolled back to where it was, once its contents are there (again as
 * they grow: its stored positions come after its names). */
void folder_scroll_restore(struct folder *f)
{
    if (f->want_scroll <= 0 || !f->cont) return;
    ui_container_scroll_by(f->cont, f->want_scroll - ui_container_scroll_y(f->cont));
    if (ui_container_scroll_y(f->cont) >= f->want_scroll - 1) f->want_scroll = 0;
}

struct folder *open_desktop(const char *path)
{
    char d[PATH_MAX];
    if (!path) { desktop_dir(d, sizeof d); path = d; }
    struct folder *f = open_window(path, UI_VIEW_ICON, true);
    desktop_first_start(f->path);
    return f;
}

struct folder *open_window(const char *path, ui_view view, bool desktop)
{
    char real0[PATH_MAX];
    const char *want = realpath(path, real0) ? real0 : path;
    for (struct folder *o = folders; o; o = o->next)
        if (!o->closing && !strcmp(o->path, want) && ui_container_view(o->cont) == view && !desktop) {
            ui_window_activate(o->win);   /* already open in this view: switch to it, as OS/2 did */
            return o;
        }
    struct folder *f = calloc(1, sizeof *f);
    f->id = next_folder_id++;
    char real[PATH_MAX];
    snprintf(f->path, sizeof f->path, "%s", realpath(path, real) ? real : path);
    const char *base = strrchr(f->path, '/');
    snprintf(f->title, sizeof f->title, "%s", base && base[1] ? base + 1 : f->path);
    double w = view == UI_VIEW_ICON ? 640 : 760, h = 480;
    const char *sz = getenv("WPS_SHELL_SIZE");
    if (sz) sscanf(sz, "%lfx%lf", &w, &h);
    if (next_w > 0 && next_h > 0 && !desktop) { w = next_w; h = next_h; }   /* (reopened at login: its size) */
    next_w = next_h = 0;
    f->open_w = w;
    f->open_h = h;
    f->desktop = desktop;
    if (desktop) findwin_set_desktop(path);   /* Find's saved results go there */
    if (desktop) {
        /* Its app ID tells the GNOME Shell extension which window to make
         * GNOME's desktop (below everything, on every workspace). */
        snprintf(f->title, sizeof f->title, "Desktop");
        f->win = ui_window_new(app, &win_ops, f, "org.wps.Desktop", w, h);
        ui_window_set_frameless(f->win, true);
        ui_a11y_window(f->win, UI_ROLE_DESKTOP, NULL);   /* for screen readers: the desktop */
        ui_window_set_maximized(f->win);
    } else {
        f->win = ui_window_new(app, &win_ops, f, "org.wps.shell", w, h);
    }
    f->lay = layout_new(UI_CELL_W, UI_CELL_H);
    f->whole_titles = true;   /* (until its Icon view page says otherwise) */
    f->show_hidden = f->show_ext = true;   /* (until its properties come) */
    layout_set_height_fn(f->lay, lay_h, f);
    layout_set_width_fn(f->lay, lay_w, f);
    f->free_layout = -1;   /* (its Layout: the property, when it comes) */
    f->cont = ui_container_new(f->win, &folder_src, f, view);
    ui_container_set_whole_titles(f->cont, true);
    f->sort_choice = -1;
    f->icon_size = -1;   /* (its set: the System's until its properties come) */
    ui_container_set_head_fns(f->cont, head_clicked, cols_resized, as_arg(f));
    if (desktop) {
        ui_container_set_backdrop(f->cont, desk_backdrop, f);
        wallpaper_start(app, desk_wallpaper_changed, as_arg(f));
    }
    if (view == UI_VIEW_TREE) tree_start(f);
    f->next = folders;
    folders = f;
    update_title(f);
    layout(f);
    if (!desktop) svc_resolve(f->path, got_title, as_arg(f));
    if (!desktop) raise_in(f, "window.open", NULL, "folder");
    notify_in_use(f->path);
    svc_subscribe_folder(f->path, got_sub, as_arg(f));
    if (!desktop) bg_follow(f);   /* (the desktop's after its first start: then wpsd knows it is the desktop) */
    svc_layout(f->path, got_layout, as_arg(f));
    request_more(f, false);
    return f;
}

/* Window events from the GNOME Shell extension, one line each on
 * standard input: event TAB app TAB title TAB x TAB y TAB w TAB h. */
char ev_buf[8192];

size_t ev_len;
