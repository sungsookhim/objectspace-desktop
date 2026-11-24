/* osdd: list (a page of a folder), open (programs and files), from_template. */
#define _GNU_SOURCE
#include "osdd.h"

#include <pwd.h>

void list_reply(void *arg, const osd_page *pg)
{
    struct list_req *r = arg;
    if (!r->c->dead) {
        if (pg->err) {
            reply_error(r->c, r->id, "failed", strerror(pg->err));
        } else {
            cbuf b;
            cb_init(&b);
            cb_map(&b, 3);
            cb_text(&b, "id"); cb_int(&b, r->id);
            cb_text(&b, "ok"); cb_bool(&b, true);
            cb_text(&b, "result");
            cb_map(&b, 5);
            cb_text(&b, "version"); cb_uint(&b, pg->version);
            cb_text(&b, "total"); cb_uint(&b, pg->total);
            cb_text(&b, "complete"); cb_bool(&b, pg->complete);
            cb_text(&b, "offset"); cb_uint(&b, pg->offset);
            cb_text(&b, "items");
            cb_array(&b, pg->n);
            for (size_t i = 0; i < pg->n; i++) {
                const osd_entry *e = &pg->items[i];
                const char *t = e->name[0] == '\x01' ? "object" : e->type == DT_DIR ? "folder" : e->type == DT_LNK ? "link" :
                                e->type == DT_REG ? "file" : "other";
                bool iu = false;
                if (e->detailed) {
                    /* In use: a lock file says so, or a program Objectspace started has it open. */
                    char fp[PATH_MAX + NAME_MAX + 2];
                    snprintf(fp, sizeof fp, "%s/%s", strcmp(r->folder, "/") ? r->folder : "", e->name);
                    iu = e->locked || (e->name[0] != '\x01' && osd_launch_in_use(fp)) ||
                         ((e->shadow || e->type == DT_LNK) && e->target_path[0] && osd_launch_in_use(e->target_path));
                }
                bool link = e->detailed && !e->shadow && e->type == DT_LNK;
                bool tpl = e->detailed && e->is_template, makes = e->detailed && e->makes[0], icon = e->detailed && e->icon[0];
                bool iopen = e->detailed && e->icon_open[0], hatch = e->detailed && e->open_hatch;
                bool ex = e->detailed && e->exec;
                cb_map(&b, e->detailed ? (e->shadow ? 9 : link ? 8 : 6) + iu + tpl + makes + icon + iopen + hatch + ex : 2);
                cb_text(&b, "name"); cb_text(&b, e->name);
                cb_text(&b, "type"); cb_text(&b, t);
                if (e->detailed) {
                    cb_text(&b, "size"); cb_int(&b, e->size);
                    cb_text(&b, "mtime"); cb_int(&b, e->mtime);
                    cb_text(&b, "id"); cb_text(&b, e->id);
                    cb_text(&b, "title"); cb_text(&b, e->title);
                    if (e->shadow) {
                        cb_text(&b, "shadow"); cb_bool(&b, true);
                        cb_text(&b, "target"); cb_text(&b, e->target_type == DT_DIR ? "folder" : "file");
                        cb_text(&b, "target_path"); cb_text(&b, e->target_path);
                    }
                    if (link) {   /* "folder", "file", or "missing" for a dangling link */
                        cb_text(&b, "target");
                        cb_text(&b, e->target_type == DT_DIR ? "folder" : e->target_type == DT_REG ? "file" : "missing");
                        cb_text(&b, "target_path"); cb_text(&b, e->target_path);
                    }
                    if (iu) { cb_text(&b, "in_use"); cb_bool(&b, true); }
                    if (ex) { cb_text(&b, "exec"); cb_text(&b, e->exec == 2 ? "text" : "window"); }   /* a program file (0.4.1) */
                    if (tpl) { cb_text(&b, "template"); cb_bool(&b, true); }
                    if (makes) { cb_text(&b, "makes"); cb_text(&b, e->makes); }   /* a built-in template */
                    if (icon) { cb_text(&b, "icon"); cb_text(&b, e->icon); }       /* its own icon (a PNG) */
                    if (iopen) { cb_text(&b, "icon_open"); cb_text(&b, e->icon_open); }   /* its own open icon */
                    if (hatch) { cb_text(&b, "open_hatch"); cb_bool(&b, true); }   /* hatched over it too */
                }
            }
            send_frame(r->c, &b);
            cb_free(&b);
        }
    }
    client_unref(r->c);
    free(r);
}

/* ---- open: start a program (as this user), or open a file */

/* An object the desktop shows itself (a view of its own, e.g. the
 * Minimized Window Viewer's): {view: name}. */
static void reply_view(struct req *r, const char *view)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    cb_map(&b, 1);
    cb_text(&b, "view"); cb_text(&b, view);
    send_frame(r->c, &b);
    cb_free(&b);
}

static void reply_folder(struct req *r, const char *path)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    cb_map(&b, 1);
    cb_text(&b, "folder"); cb_text(&b, path);   /* the GUI opens a folder window */
    send_frame(r->c, &b);
    cb_free(&b);
}

/* A program another user (not root) owns asks first: nobody can plant
 * something that runs when another user opens it. */
static void ask_first(struct req *r, const char *cmd, uid_t owner)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    cb_map(&b, 3);
    cb_text(&b, "needs_confirmation"); cb_bool(&b, true);
    cb_text(&b, "command"); cb_text(&b, cmd);
    cb_text(&b, "owner_uid"); cb_int(&b, (int64_t)owner);
    send_frame(r->c, &b);
    cb_free(&b);
}

void do_open(struct req *r, osdo_object *o)
{
    const cv *cf = cv_get(&r->msg, "confirm");
    bool confirmed = cf && cf->k == CV_BOOL && cf->b;
    char buf[8192], *argv[64], cwd[PATH_MAX] = "";
    /* objects dropped on a program: it starts with them (files: absolute
     * paths, at most 32) */
    const char *files[32];
    size_t nfiles = 0;
    const cv *fl = cv_get(&r->msg, "files");
    for (size_t i = 0; fl && fl->k == CV_ARRAY && i < fl->n && nfiles < 32; i++)
        if (fl->items[i].k == CV_TEXT && fl->items[i].s[0] == '/' && strlen(fl->items[i].s) < PATH_MAX) files[nfiles++] = fl->items[i].s;
    const char *what;
    const char *tracked;   /* what shows as in use while it runs */
    const char *path = r->tgt.path;
    const struct stat *st = &r->tgt.st;
    osdo_object *orig = NULL;
    /* A shadow opens its original, wherever that is now. */
    if (osdo_is_a(o, OsdShadowClass)) {
        orig = OsdShadow_osdQueryTarget(o);
        if (!orig || r->shadow_target.err || (!r->shadow_target.path[0] && !r->shadow_target.central)) {
            osdo_unref(orig);
            reply_error(r->c, r->id, "unavailable", "the original is not available (on a volume that is not mounted, or gone)");
            return;
        }
        o = orig;
        path = r->shadow_target.path;
        st = &r->shadow_target.st;
    }
    /* Command prompt here (0.4.1): the user's shell in the System's
     * terminal (Objectspace Terminal by default), in the folder, $OSD_FOLDER set */
    const cv *pr = cv_get(&r->msg, "prompt");
    bool prompt = pr && pr->k == CV_BOOL && pr->b && osdo_is_a(o, OsdFolderClass) && path[0];
    static char prompt_track[PATH_MAX + 16], prompt_env[PATH_MAX + 16], prompt_title[PATH_MAX + 32];
    /* Command Prompt (0.5, in System): the same, in the home folder or the
     * folder its Program page names (gone: home) */
    static char prompt_dir[PATH_MAX];
    if (osdo_is_a(o, OsdCommandPromptClass)) {
        const char *home = getenv("HOME");
        char *si = OsdObject_osdQueryProperty(o, "start_in");
        snprintf(prompt_dir, sizeof prompt_dir, "%s", home && *home ? home : "/");
        if (si && *si) {
            char want[PATH_MAX];
            if (si[0] == '~') snprintf(want, sizeof want, "%s%s", home ? home : "", si + 1);
            else snprintf(want, sizeof want, "%s", si);
            struct stat ws;
            if (want[0] == '/' && stat(want, &ws) == 0 && S_ISDIR(ws.st_mode)) snprintf(prompt_dir, sizeof prompt_dir, "%s", want);
        }
        free(si);
        path = prompt_dir;
        prompt = true;
    }
    if (osdo_is_a(o, OsdFolderClass) && !prompt) {
        free(fire_event(o, path, "object.open", NULL, NULL, NULL, 0));   /* its actions (its window has its own sound) */
        reply_folder(r, path);
        osdo_unref(orig);
        return;
    }
    /* A plain file-system link opens what it points to, as that would. */
    struct stat ts;
    char real[PATH_MAX];
    bool data = osdo_is_a(o, OsdDataFileClass);
    if (S_ISLNK(st->st_mode) && stat(path, &ts) == 0 && realpath(path, real)) {
        if (S_ISDIR(ts.st_mode)) { reply_folder(r, real); osdo_unref(orig); return; }
        path = real;
        st = &ts;
        data = true;
    }
    if (prompt) {
        const char *sh = getenv("SHELL");
        struct passwd *pw = (!sh || !*sh) ? getpwuid(getuid()) : NULL;
        if (pw) sh = pw->pw_shell;
        argv[0] = (char *)(sh && *sh ? sh : "/bin/sh");
        argv[1] = NULL;
        const char *bn = strrchr(path, '/');
        snprintf(prompt_title, sizeof prompt_title, "Command Prompt - %s", bn && bn[1] ? bn + 1 : path);
        if (!osd_terminal_wrap(argv, 1, 64, prompt_title, false)) {
            reply_error(r->c, r->id, "failed", "no terminal program was found (System: Programs page)");
            osdo_unref(orig);
            return;
        }
        snprintf(cwd, sizeof cwd, "%s", path);
        snprintf(prompt_track, sizeof prompt_track, "\x01prompt:%s", path);   /* (tracked, not shown as in use) */
        snprintf(prompt_env, sizeof prompt_env, "OSD_FOLDER=%s", path);
        what = argv[0];
        tracked = prompt_track;
    } else if (!data && osdo_is_a(o, OsdProgramClass)) {
        const char *cmd = OsdProgram_data(o)->command;
        if (!cmd || !*cmd) { reply_error(r->c, r->id, "failed", "the program has no command"); osdo_unref(orig); return; }
        if (st->st_uid != geteuid() && st->st_uid != 0 && !confirmed) { ask_first(r, cmd, st->st_uid); osdo_unref(orig); return; }
        int argc = osd_split_exec_files(cmd, nfiles ? files : NULL, nfiles, buf, sizeof buf, argv, 64);
        if (argc == 0) { reply_error(r->c, r->id, "failed", "empty command"); osdo_unref(orig); return; }
        size_t pl = strlen(path);
        /* its Program page (v0.3-design 8): the type, Close window on
         * exit, the working directory */
        int64_t ptype = 0, close_exit = 1;
        char *wd = NULL;
        osd_state_get_int(o, "OsdProgram", "ptype", &ptype);
        osd_state_get_int(o, "OsdProgram", "close_on_exit", &close_exit);
        osd_state_get_str(o, "OsdProgram", "cwd", &wd);
        if (wd && *wd) {
            const char *home = getenv("HOME");
            if (wd[0] == '~') snprintf(cwd, sizeof cwd, "%s%s", home ? home : "", wd + 1);
            else snprintf(cwd, sizeof cwd, "%s", wd);
            struct stat ws;
            if (stat(cwd, &ws) != 0 || !S_ISDIR(ws.st_mode)) cwd[0] = 0;   /* (gone: home, as without one) */
        }
        free(wd);
        bool term = ptype == 2 || (ptype == 0 && pl > 8 && !strcmp(path + pl - 8, ".desktop") && osd_desktop_wants_terminal(path));
        char *ttl = OsdObject_osdQueryTitle(o);
        int wrapped = term ? osd_terminal_wrap(argv, argc, 64, ttl, !close_exit) : argc;
        free(ttl);
        if (term) argc = wrapped;
        if (term && !wrapped) {
            reply_error(r->c, r->id, "failed", "it runs in a terminal, and no terminal program was found (System: Programs page)");
            osdo_unref(orig);
            return;
        }
        what = cmd;
        tracked = path;
    } else if (data) {
        tracked = path;
        if (S_ISREG(st->st_mode) && osd_is_program_file(path)) {
            /* An executable runs (as a program file does in OS/2), in its
             * own folder; one another user owns asks first. */
            if (st->st_uid != geteuid() && st->st_uid != 0 && !confirmed) { ask_first(r, path, st->st_uid); osdo_unref(orig); return; }
            argv[0] = (char *)path;
            int na = 1;
            for (size_t k = 0; k < nfiles && na < 60; k++) argv[na++] = (char *)files[k];   /* dropped on it */
            argv[na] = NULL;
            what = path;
            /* its Program page: in a window or a terminal (automatic: as
             * the file says), the terminal kept or not, where it runs */
            char *pt = OsdObject_osdQueryProperty(o, "prog_type"), *pc = OsdObject_osdQueryProperty(o, "prog_close"),
                 *pw = OsdObject_osdQueryProperty(o, "prog_cwd");
            bool term = pt && !strncmp(pt, "text", 4) ? true : pt && !strcmp(pt, "window") ? false : osd_program_needs_terminal(path);
            bool keep = pc && (!strcmp(pc, "0") || !strcmp(pc, "false"));
            free(pt);
            free(pc);
            const char *bn = strrchr(path, '/');
            int wrapped = term ? osd_terminal_wrap(argv, na, 64, bn ? bn + 1 : path, keep) : na;
            if (term) na = wrapped;
            if (term && !wrapped) {
                free(pw);
                reply_error(r->c, r->id, "failed", "a text-mode program, and no terminal program was found (System: Programs page)");
                osdo_unref(orig);
                return;
            }
            snprintf(cwd, sizeof cwd, "%s", path);
            *strrchr(cwd, '/') = 0;
            if (!cwd[0]) strcpy(cwd, "/");
            if (pw && *pw) {   /* (its own: if it is a folder; else its own folder, as without one) */
                const char *home = getenv("HOME");
                char want[PATH_MAX];
                if (pw[0] == '~') snprintf(want, sizeof want, "%s%s", home ? home : "", pw + 1);
                else snprintf(want, sizeof want, "%s", pw);
                struct stat ws;
                if (stat(want, &ws) == 0 && S_ISDIR(ws.st_mode)) snprintf(cwd, sizeof cwd, "%s", want);
            }
            free(pw);
        } else if (cv_text(&r->msg, "app") || cv_get(&r->msg, "app_index")) {
            /* Open With: the program chosen (by its ID, or its place in the
             * Open cascade), made the type's default if asked */
            const char *app = cv_text(&r->msg, "app");
            int64_t k = -1;
            osd_app *a = NULL;
            if (!app && cv_int(&r->msg, "app_index", &k) && k >= 0 && k < OSD_MI_OPEN_APPS) {
                a = malloc(OSD_MI_OPEN_APPS * sizeof *a);
                int n = a ? osd_apps_for_file(path, NULL, 0, a, OSD_MI_OPEN_APPS) : 0;
                if (k < n) app = a[k].id;
            }
            const cv *al = cv_get(&r->msg, "always");
            bool ok = app != NULL;
            if (ok && al && al->k == CV_BOOL && al->b && !osd_app_set_default(path, app)) {
                reply_error(r->c, r->id, "failed", "the default program for the type could not be stored (mimeapps.list)");
                free(a);
                osdo_unref(orig);
                return;
            }
            if (!ok || osd_app_argv(app, path, buf, sizeof buf, argv, 64) <= 0) {
                reply_error(r->c, r->id, "failed", "that program cannot be started (not installed, or it runs in a terminal and there is none)");
                free(a);
                osdo_unref(orig);
                return;
            }
            free(a);
            what = argv[0];
        } else if (osd_handler_argv(path, buf, sizeof buf, argv, 64) > 0) {
            /* Its type's default application, started directly so that its
             * process stands for the open file (xdg-open hands over and exits). */
            what = argv[0];
        } else {
            argv[0] = "xdg-open";
            argv[1] = (char *)path;
            argv[2] = NULL;
            what = "xdg-open";
        }
    } else if (osdo_is_a(o, OsdLauncherClass) || osdo_is_a(o, OsdTheBarClass)) {
        /* opened as the palette or the bar: the desktop shows it, or
         * brings it forward (never two) */
        reply_view(r, osdo_is_a(o, OsdLauncherClass) ? "launcher" : "thebar");
        osdo_unref(orig);
        return;
    } else if (osdo_is_a(o, OsdColorPaletteClass)) {
        reply_view(r, "colorpalette");
        osdo_unref(orig);
        return;
    } else if (osdo_is_a(o, OsdFontPaletteClass)) {
        reply_view(r, "fontpalette");
        osdo_unref(orig);
        return;
    } else if (osdo_is_a(o, OsdMessagesClass)) {
        reply_view(r, "messages");
        osdo_unref(orig);
        return;
    } else if (osdo_is_a(o, OsdRestoreDesktopClass)) {
        reply_view(r, "restore-defaults");   /* the desktop asks, then sends restore_defaults */
        osdo_unref(orig);
        return;
    } else if (osdo_is_a(o, OsdMinWinViewerClass)) {
        reply_view(r, "minimized-windows");
        osdo_unref(orig);
        return;
    } else if (osdo_is_a(o, OsdDriveClass)) {
        /* a drive: its top folder, mounted first if need be */
        const char *u = OsdDrive_data(o)->uuid;
        osd_drive_info d;
        if (!u || !osd_udisks_find(u, &d)) reply_error(r->c, r->id, "not found", "the drive is not there (plugged out?)");
        else if (d.mount[0]) reply_folder(r, d.mount);
        else if (d.encrypted && !d.unlocked) reply_error(r->c, r->id, "locked", "the volume is locked: unlock it first");
        else {
            struct drive_req *q = calloc(1, sizeof *q);
            q->c = r->c; r->c->refs++;
            q->id = r->id;
            q->open = true;
            osd_udisks_call(u, "mount", NULL, drive_done, q);
        }
        osdo_unref(orig);
        return;
    } else {
        reply_error(r->c, r->id, "bad request", "this kind of object opens in the GUI");
        osdo_unref(orig);
        return;
    }
    /* On the asking GUI's display: a socket path that is this user's. */
    const char *disp = cv_text(&r->msg, "display");
    struct stat ds;
    if (disp && (disp[0] != '/' || stat(disp, &ds) != 0 || !S_ISSOCK(ds.st_mode) || ds.st_uid != geteuid())) disp = NULL;
    /* and its X display (":N" or ":N.S", nothing else) */
    const char *x11 = cv_text(&r->msg, "x11");
    if (x11) {
        const char *q = x11;
        bool ok = *q++ == ':' && isdigit((unsigned char)*q);
        while (ok && isdigit((unsigned char)*q)) q++;
        if (ok && *q == '.') { q++; ok = isdigit((unsigned char)*q); while (ok && isdigit((unsigned char)*q)) q++; }
        if (!ok || *q || strlen(x11) > 16) x11 = NULL;
    }
    /* and the asking session's input-method and desktop settings: these
     * names only, short printable values */
    const char *envs[8], *toks[2];
    size_t nenv = 0, ntok = 0;
    const cv *ev = cv_get(&r->msg, "env");
    /* (and its activation token, 0.4 step 6: the click's, so the program's
     * window comes to the front - Wayland's, and X11's startup ID) */
    static const char *const allowed[] = { "GTK_IM_MODULE=", "QT_IM_MODULE=", "XMODIFIERS=", "XDG_CURRENT_DESKTOP=", "XDG_SESSION_TYPE=",
                                           "XDG_ACTIVATION_TOKEN=", "DESKTOP_STARTUP_ID=" };
    for (size_t i = 0; ev && ev->k == CV_ARRAY && i < ev->n && nenv + ntok < 8; i++) {
        const cv *e = &ev->items[i];
        if (e->k != CV_TEXT || strlen(e->s) > 280) continue;
        size_t k = 0;
        bool ok = false;
        for (; k < 7 && !ok; k++) ok = !strncmp(e->s, allowed[k], strlen(allowed[k]));
        for (const char *p = e->s; ok && *p; p++) ok = (unsigned char)*p >= 0x20 && *p != 0x7f;
        if (!ok) continue;
        if (k > 5 && ntok < 2) toks[ntok++] = e->s;   /* (k: one past the match) */
        else if (k <= 5) envs[nenv++] = e->s;
    }
    const cv *te = cv_get(&r->msg, "token_env");   /* (the GUI's, sent apart) */
    for (size_t i = 0; te && te->k == CV_ARRAY && i < te->n && ntok < 2; i++) {
        const cv *e = &te->items[i];
        bool ok = e->k == CV_TEXT && strlen(e->s) <= 280 &&
                  (!strncmp(e->s, allowed[5], strlen(allowed[5])) || !strncmp(e->s, allowed[6], strlen(allowed[6])));
        for (const char *p = ok ? e->s : ""; ok && *p; p++) ok = (unsigned char)*p > 0x20 && *p != 0x7f;
        if (ok) toks[ntok++] = e->s;
    }
    client_note_session(r->c, disp, x11, envs, nenv);   /* (its next actions' session; a token is for this once) */
    for (size_t i = 0; i < ntok && nenv < 8; i++) envs[nenv++] = toks[i];
    const char *envs2[9];   /* (a prompt: its folder too) */
    memcpy(envs2, envs, nenv * sizeof *envs);
    size_t nenv2 = nenv;
    if (prompt) envs2[nenv2++] = prompt_env;
    pid_t pid = osd_launch(argv, tracked, cwd[0] ? cwd : NULL, disp, x11, envs2, nenv2);   /* as this user, in this user's session */
    if (pid < 0) { reply_error(r->c, r->id, "failed", strerror(errno)); osdo_unref(orig); return; }
    char wa[PATH_MAX];   /* opened from a work area (or a folder or shadow in it): its group's */
    char *own_wa = prompt ? OsdObject_osdQueryProperty(o, "work_area") : NULL;   /* (a prompt in the work area itself) */
    bool in_own = own_wa && (!strcmp(own_wa, "1") || !strcmp(own_wa, "true"));
    free(own_wa);
    if (in_own) osd_launch_set_group(pid, path);
    else if (r->tgt.path[0] && work_area_of(r->tgt.path, wa, sizeof wa)) work_area_opened(wa, r->tgt.path, prompt ? "" : tracked, pid);
    /* its actions on opening (object-properties-design.md 2.4); the sound
     * the GUI plays: absent, the Sound object's; "" none */
    char *sound = fire_event(o, path, "object.open", disp, x11, envs, nenv);
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    cb_map(&b, sound ? 3 : 2);
    cb_text(&b, "launched"); cb_int(&b, pid);
    cb_text(&b, "command"); cb_text(&b, what);
    if (sound) { cb_text(&b, "sound"); cb_text(&b, sound); }
    free(sound);
    send_frame(r->c, &b);
    cb_free(&b);
    osdo_unref(orig);   /* after the reply: `what` may be the original's command */
}

/* Copy the user.osd.* attributes of one open file to another, without
 * the object ID (a new object gets its own) and, at the top, without the
 * template mark. */
static void tmpl_attrs(int in, int out, bool top)
{
    char names[8192];
    ssize_t nl = flistxattr(in, names, sizeof names);
    for (ssize_t o = 0; nl > 0 && o < nl; o += (ssize_t)strlen(names + o) + 1) {
        const char *nm = names + o;
        if (strncmp(nm, "user.osd.", 9) || !strcmp(nm, OSD_ATTR_ID)) continue;
        unsigned char v[65536];
        ssize_t vl = fgetxattr(in, nm, v, sizeof v);
        if (vl < 0) continue;
        if (top && !strcmp(nm, "user.osd.OsdObject")) {
            osd_props ps;
            if (osd_props_decode(v, (size_t)vl, &ps) == 0) {
                osd_props_remove(&ps, "template");
                osd_props_remove(&ps, "title");   /* the copy is titled by its own name */
                ps.counter = 0;
                size_t el;
                unsigned char *enc = osd_props_encode(&ps, &el);
                if (enc) fsetxattr(out, nm, enc, el, 0);
                free(enc);
                osd_props_free(&ps);
            }
        } else {
            fsetxattr(out, nm, v, (size_t)vl, 0);
        }
    }
}

/* A folder template's contents, copied (links as links; nothing else). */
static int tmpl_copy_tree(const char *src, const char *dst, int depth)
{
    if (depth > 64) return ELOOP;
    DIR *d = opendir(src);
    if (!d) return errno;
    struct dirent *de;
    int err = 0;
    while (!err && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char s[PATH_MAX], t[PATH_MAX];
        snprintf(s, sizeof s, "%s/%s", src, de->d_name);
        snprintf(t, sizeof t, "%s/%s", dst, de->d_name);
        struct stat st;
        if (lstat(s, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (mkdir(t, st.st_mode & 07777) != 0) { err = errno; break; }
            int a = open(s, O_RDONLY | O_DIRECTORY | O_CLOEXEC), b = open(t, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (a >= 0 && b >= 0) tmpl_attrs(a, b, false);
            if (a >= 0) close(a);
            if (b >= 0) close(b);
            err = tmpl_copy_tree(s, t, depth + 1);
        } else if (S_ISLNK(st.st_mode)) {
            char l[PATH_MAX];
            ssize_t n = readlink(s, l, sizeof l - 1);
            if (n >= 0) { l[n] = 0; if (symlink(l, t) != 0) err = errno; }
        } else if (S_ISREG(st.st_mode)) {
            int in = open(s, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            int out = in < 0 ? -1 : open(t, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 0777);
            if (in < 0 || out < 0) err = errno;
            char buf[65536];
            ssize_t n;
            while (!err && (n = read(in, buf, sizeof buf)) > 0)
                if (write(out, buf, (size_t)n) != n) err = EIO;
            if (!err) tmpl_attrs(in, out, false);
            if (in >= 0) close(in);
            if (out >= 0) close(out);
        }
    }
    closedir(d);
    return err;
}

void tmpl_work(void *arg)
{
    struct tmpl_job *j = arg;
    struct stat sst;
    if (lstat(j->src, &sst) != 0) { j->err = errno; return; }
    if (S_ISDIR(sst.st_mode)) {
        /* A folder template: a new folder with a copy of what it holds. */
        int e = EEXIST;
        for (int k = 1; k < 100 && e == EEXIST; k++) {
            if (k == 1) snprintf(j->path, sizeof j->path, "%s/%.200s", j->folder, j->title);
            else snprintf(j->path, sizeof j->path, "%s/%.200s (%d)", j->folder, j->title, k);
            e = mkdir(j->path, 0777) == 0 ? 0 : errno;   /* never into an existing one */
        }
        if (e) { j->err = e; return; }
        int a = open(j->src, O_RDONLY | O_DIRECTORY | O_CLOEXEC), b = open(j->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (a >= 0 && b >= 0) tmpl_attrs(a, b, true);
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        j->err = tmpl_copy_tree(j->src, j->path, 0);
        if (!j->err) osd_load(j->path, false, &j->ld);
        return;
    }
    const char *base = strrchr(j->src, '/') ? strrchr(j->src, '/') + 1 : j->src;
    const char *ext = strchr(base, '.') ? strchr(base, '.') : "";
    /* A title that already ends in the extension (a file named as it is
     * titled) does not get it twice. */
    size_t tl = strlen(j->title), el = strlen(ext);
    if (el && tl > el && !strcmp(j->title + tl - el, ext)) j->title[tl - el] = 0;
    int in = open(j->src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC), out = -1;
    if (in < 0) { j->err = errno; return; }
    for (int k = 1; k < 100 && out < 0; k++) {
        if (k == 1) snprintf(j->path, sizeof j->path, "%s/%.200s%s", j->folder, j->title, ext);
        else snprintf(j->path, sizeof j->path, "%s/%.200s (%d)%s", j->folder, j->title, k, ext);
        out = open(j->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);   /* never overwrite */
        if (out < 0 && errno != EEXIST) break;
    }
    if (out < 0) { j->err = errno; close(in); return; }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) { j->err = EIO; break; }
    /* Settings come along; identity and the template mark do not. */
    tmpl_attrs(in, out, true);
    close(in);
    close(out);
    if (!j->err) osd_load(j->path, false, &j->ld);
}

void tmpl_done(void *arg)
{
    struct tmpl_job *j = arg;
    if (!j->c->dead) {
        osdo_object *o = j->err || j->ld.err ? NULL : osd_object_from_loaded(&j->ld);
        if (!o) reply_error(j->c, j->id, "failed", strerror(j->err ? j->err : j->ld.err));
        else {
            /* Titled by its name, like any file (see create_done). */
            if (OsdObject_data(o)->title) OsdObject_osdSetTitle(o, NULL);
            char *sound = fire_event_for(j->c, o, osd_object_path(o), "object.create");   /* its actions (When made) */
            struct req fake = { .c = j->c, .id = j->id };
            reply_ok_object_sound(&fake, o, NULL, sound);
            free(sound);
        }
    }
    osd_loaded_free(&j->ld);
    client_unref(j->c);
    free(j);
}
