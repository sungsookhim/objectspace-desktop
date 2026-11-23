#define _GNU_SOURCE
#include "shell.h"

static void on_window_events(void *arg, int fd, short rev)
{
    (void)arg;
    ssize_t n = read(fd, ev_buf + ev_len, sizeof ev_buf - 1 - ev_len);
    if (n <= 0) {
        if (n < 0 && errno == EAGAIN) return;
        ui_app_unwatch_fd(app, fd);   /* the extension went away */
        return;
    }
    (void)rev;
    ev_len += (size_t)n;
    ev_buf[ev_len] = 0;
    char *line = ev_buf, *nl;
    while ((nl = strchr(line, '\n'))) {
        *nl = 0;
        char *f[13] = { 0 };
        int k = 0;
        for (char *p = line, *t; k < 13 && (t = strsep(&p, "\t"));) f[k++] = t;
        if (k >= 7 && !strcmp(f[0], "W")) winlist_line(f, k);   /* the window list (G4) */
        else if (k >= 2 && !strcmp(f[0], "K") && !strcmp(f[1], "window-list")) winlist_open(false);   /* Ctrl+Esc */
        else if (k >= 2 && !strcmp(f[0], "K") && !strcmp(f[1], "wps-menu")) {   /* Super alone (WPS session) */
            if (ui_menu_is_open()) ui_menu_close_all();   /* pressed again: closed (as on Windows) */
            else if (warpcenter_is_open()) warpcenter_press("desktop");
        }
        else if (k >= 5 && !strcmp(f[0], "I")) warpcenter_input_source(atoi(f[1]), atoi(f[2]), !strcmp(f[3], "ibus"), f[4]);
        else if (k == 7 && !strncmp(f[0], "window.", 7)) {
            ui_rect at = { atof(f[3]), atof(f[4]), atof(f[5]), atof(f[6]) };
            events_raise(f[0], f[1], "window", at);
        }
        line = nl + 1;
    }
    ev_len = strlen(line);
    memmove(ev_buf, line, ev_len + 1);
    if (ev_len == sizeof ev_buf - 1) ev_len = 0;   /* a line too long: dropped */
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--version")) { printf("wps-shell %s\n", WPS_VERSION); return 0; }
    /* --events-stdin (anywhere: the extension adds it last) */
    bool events_stdin = false;
    for (int k = 1; k < argc; k++)
        if (!strcmp(argv[k], "--events-stdin")) {
            events_stdin = true;
            memmove(argv + k, argv + k + 1, (size_t)(argc - k) * sizeof *argv);
            argc--;
            k--;
        }
    int i = 1;
    bool desk = false;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--desktop")) desk = true;
        else if (!strcmp(argv[i], "--details")) opt_view = UI_VIEW_DETAILS;
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) opt_bench = atof(argv[++i]);
        else { fprintf(stderr, "usage: wps-shell [--desktop [FOLDER]] [--details] [--bench SECONDS] [FOLDER...]\n"); return 2; }
    }
    if (i >= argc) desk = true;   /* no folder: the desktop */
    since_start();
    app = ui_app_new();
    load_system_conf();
    events_init();
    ui_set_action_hook(toolkit_action);
    if (events_stdin) {
        fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
        ui_app_watch_fd(app, 0, POLLIN, on_window_events, NULL);
        /* Standard output is the extension's command pipe now: anything
         * else printed goes to standard error. */
        int cmd = dup(1);
        dup2(2, 1);
        fcntl(cmd, F_SETFD, FD_CLOEXEC);
        winlist_init(app, cmd);
    } else {
        winlist_init(app, -1);
    }
    findwin_init(app, launchpad_open_folder, shell_drag_paths);
    icons_init(app);
    appicons_init(appicons_ready);
    iconpick_init(app, launchpad_open_folder);
    filepick_init(app);
    apppick_init(app);
    fontpal_init(app, font_apply);
    tooltip_init(app);
    launchpad_set_props(lp_props);
    warpcenter_on_font(font_dropped_wc);
    if (!app) return 1;
    char err[512];
    /* The service may be starting at the same moment (a session starts
     * both): wait for it a little before giving up. */
    bool up = false;
    for (int tries = 0; tries < 50 && !(up = svc_start(app, err, sizeof err)); tries++) {
        if (tries == 0) fprintf(stderr, "wps-shell: waiting for wpsd...\n");
        usleep(100000);
    }
    if (!up) {
        fprintf(stderr, "wps-shell: %s\n", err);
        fprintf(stderr, "wps-shell: start wpsd first (build/wpsd &), with the same WPS_RUNTIME_DIR%s%s\n",
                getenv("WPS_RUNTIME_DIR") ? ": " : " (unset here)", getenv("WPS_RUNTIME_DIR") ? getenv("WPS_RUNTIME_DIR") : "");
        return 1;
    }
    svc_on_event(on_event, NULL);
    svc_view_fn = open_view;
    svc_subscribe_jobs(ignore_reply, NULL);
    svc_setup(got_setup, NULL);
    /* Settings shared with GNOME: every shell follows its font; the
     * desktop keeps the rest in step both ways. */
    gnome_start(app, desk, gnome_set_wps, gnome_font);
    if (desk) {
        /* the desktop process: Messages, and in the WPS session the
         * notification service */
        messages_init(app);
        notify_init(app);
        /* --desktop FOLDER: that folder as the desktop (tests) */
        struct folder *f = open_desktop(i < argc ? argv[i++] : NULL);
        if (getenv("WPS_SHELL_SELFTEST")) ui_surface_set_continuous(f->win, true);
    }
    for (int k = i; k < argc; k++) {
        struct folder *f = open_folder(argv[k], opt_view);
        if (getenv("WPS_SHELL_SELFTEST")) ui_surface_set_continuous(f->win, true);   /* every window ticks */
        if (k == i && opt_bench > 0) {
            f->bench_secs = opt_bench;
            f->bench_seed = 12345;
            ui_surface_set_continuous(f->win, true);
        }
    }
    if (desk) events_raise("shell.startup", "", "", (ui_rect){ 0, 0, 0, 0 });
    ui_app_run(app);
    events_raise("shell.shutdown", "", "", (ui_rect){ 0, 0, 0, 0 });
    events_shutdown();
    gnome_stop();
    wallpaper_stop();
    while (folders) free_folder(folders);
    svc_stop();
    return 0;
}


