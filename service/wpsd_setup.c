/* wpsd: The desktop's first start, the settings objects, the default objects, Startup. */
#define _GNU_SOURCE
#include "wpsd.h"

static void fs_next(void);

static int first_start_done_version(char *marker, size_t n)
{
    snprintf(marker, n, "%s/desktop-made", env.config_dir);
    FILE *f = fopen(marker, "r");
    if (!f) return 0;
    char line[128];
    int v = 1;   /* 0.1's marker: no version */
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "version ", 8)) v = atoi(line + 8);
    fclose(f);
    return v;
}

static struct fs_shadow *fs_queue, **fs_tail = &fs_queue;

static bool fs_busy;

static void protect(const char *target);

static void first_start_shadow_done(void *arg, const char *link, const char *error)
{
    (void)arg;
    if (!link && error) log_msg("first start: a shadow was not made: %s", error);
    if (link) protect(link);   /* (WPS's own: Prevent delete, as step 11 gives the others) */
    fs_busy = false;
    fs_next();
}

static void fs_next(void)
{
    if (fs_busy || !fs_queue) return;
    struct fs_shadow *q = fs_queue;
    fs_queue = q->next;
    if (!fs_queue) fs_tail = &fs_queue;
    fs_busy = true;
    wps_shadow_create(q->target, q->folder, first_start_shadow_done, NULL);
    free(q);
}

static void first_start_shadow(const char *target, const char *folder)
{
    struct fs_shadow *q = calloc(1, sizeof *q);
    if (!q) return;
    snprintf(q->target, sizeof q->target, "%s", target);
    snprintf(q->folder, sizeof q->folder, "%s", folder);
    *fs_tail = q;
    fs_tail = &q->next;
    fs_next();
}

/* the desktop's shadows of System Setup (0.1 put one there) */
static void find_setup_shadow(const char *vol, const char *id, void *user)
{
    (void)vol;
    struct first_start *j = user;
    char loc[WPS_ID_LEN + 8];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_loaded ld;
    wps_load(loc, false, &ld);
    char sd[PATH_MAX];
    setup_dir(sd, sizeof sd);
    wps_loaded sl;
    wps_load(sd, false, &sl);
    if (!ld.err && !sl.err && sl.id[0] && !strcmp(ld.shadow_of, sl.id) && j->ngone < 8)
        snprintf(j->gone[j->ngone++], sizeof j->gone[0], "%s", id);
    wps_loaded_free(&sl);
    wps_loaded_free(&ld);
}

/* shadows of the Templates folder (on the desktop: to move; in System:
 * there already) */
static void find_tpl_shadow(const char *vol, const char *id, void *user)
{
    (void)vol;
    struct first_start *j = user;
    char loc[WPS_ID_LEN + 8];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_loaded ld, tl;
    wps_load(loc, false, &ld);
    wps_load(wps_templates_dir(), false, &tl);
    if (!ld.err && !tl.err && tl.id[0] && !strcmp(ld.shadow_of, tl.id)) {
        if (j->scan_sys) j->tpl_in_system = true;
        else if (j->ntpl < 8) snprintf(j->tpl_moved[j->ntpl++], sizeof j->tpl_moved[0], "%s", id);
    }
    wps_loaded_free(&tl);
    wps_loaded_free(&ld);
}

void first_start_work(void *arg)
{
    struct first_start *j = arg;
    char marker[PATH_MAX + 32];
    j->from = first_start_done_version(marker, sizeof marker);
    if (j->from >= FIRST_START_VERSION) return;
    wps_mkdirs(env.config_dir, 0700);
    char sd[PATH_MAX];
    setup_dir(sd, sizeof sd);
    wps_mkdirs(sd, 0700);
    const char *td = wps_templates_dir();
    if (td[0]) mkdir(td, 0755);
    system_dir(j->sys, sizeof j->sys);
    wps_mkdirs(j->sys, 0700);
    const char *xc = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xc && *xc) snprintf(j->autostart, sizeof j->autostart, "%s/autostart", xc);
    else snprintf(j->autostart, sizeof j->autostart, "%s/.config/autostart", home ? home : "");
    if (j->from < 3) {
        /* step 3 (0.2): System on the desktop in System Setup's place;
         * the Minimized Window Viewer and Startup in System */
        wps_mkdirs(j->autostart, 0755);
        wps_loaded fl;
        wps_load(j->desk, true, &fl);
        if (!fl.err && fl.id[0]) wps_central_in(fl.vol, fl.id, find_setup_shadow, j);
        wps_loaded_free(&fl);
        for (int k = 0; k < j->ngone; k++) wps_central_delete(j->gone[k]);
        wps_loaded sy;
        wps_load(j->sys, true, &sy);
        if (!sy.err && sy.id[0] && wps_central_create(sy.vol, sy.id, "WPMinWinViewer", "Minimized Window Viewer", NULL, j->viewer) != 0)
            j->viewer[0] = 0;
        wps_loaded_free(&sy);
        wps_load(j->autostart, true, &j->asld);
    }
    if (j->from < 4) {
        /* step 4 (0.2): the Trash Can - the home trash's files folder, of
         * class WPTrashCan - on the desktop */
        char home[PATH_MAX], info[PATH_MAX + 8];
        wps_trash_home(home, sizeof home);
        snprintf(j->trash, sizeof j->trash, "%s/files", home);
        snprintf(info, sizeof info, "%s/info", home);
        wps_mkdirs(j->trash, 0700);
        wps_mkdirs(info, 0700);
        if (wps_set_folder_class(j->trash, "WPTrashCan") == 0) {
            /* its title, on it at once (a shadow made next shows it: the
             * object's own saving comes a moment later) */
            wps_props cur, set;
            wps_props_init(&cur);
            wps_props_init(&set);
            if (wps_attr_read(j->trash, "WPObject", &cur) != WPS_A_OK || !wps_props_get(&cur, "title")) {
                wps_props_set_str(&set, "title", "Trash Can");
                wps_attr_update(j->trash, "WPObject", &set, NULL, 2000, NULL, NULL);
            }
            wps_props_free(&cur);
            wps_props_free(&set);
            wps_load(j->trash, true, &j->tld);
        } else j->trash[0] = 0;
    }
    if (j->from < 5) {
        /* step 5 (0.2): Computer, with Drives, in System */
        char comp[PATH_MAX], drv[PATH_MAX + 8];
        computer_dirs(comp, sizeof comp, drv, sizeof drv);
        wps_mkdirs(drv, 0700);
        wps_set_folder_class(comp, "WPComputer");
        wps_set_folder_class(drv, "WPDrives");
    }
    if (j->from < 6) {
        /* step 6 (0.2): Programs, on the desktop */
        data_dir("Programs", j->programs, sizeof j->programs);
        wps_mkdirs(j->programs, 0700);
        if (wps_set_folder_class(j->programs, "WPProgramsFolder") != 0) j->programs[0] = 0;
    }
    if (j->from < 10) {   /* step 10 (0.3): their titles, set in first_start_done */
        char comp[PATH_MAX], drv[PATH_MAX + 8];
        computer_dirs(comp, sizeof comp, drv, sizeof drv);
        wps_mkdirs(drv, 0700);   /* (as step 5: there on every desktop since) */
        wps_set_folder_class(comp, "WPComputer");
        wps_load(j->sys, true, &j->sysld);
        wps_load(comp, true, &j->compld);
    }
    if (j->from < 8) {
        /* step 8 (0.2): Restore Desktop in System */
        wps_loaded sy;
        wps_load(j->sys, true, &sy);
        if (sy.err || !sy.id[0] || wps_central_create(sy.vol, sy.id, "WPRestoreDesktop", "Restore Desktop", NULL, j->restorer) != 0)
            j->restorer[0] = 0;
        wps_loaded_free(&sy);
    }
    if (j->from < 7 && j->from >= 1 && wps_templates_dir()[0]) {
        /* step 7 (0.2): the desktop's Templates shadows into System */
        wps_loaded sy, fl;
        wps_load(j->sys, true, &sy);
        wps_load(j->desk, true, &fl);
        if (!sy.err && sy.id[0]) { j->scan_sys = true; wps_central_in(sy.vol, sy.id, find_tpl_shadow, j); j->scan_sys = false; }
        if (!fl.err && fl.id[0]) wps_central_in(fl.vol, fl.id, find_tpl_shadow, j);
        int k = 0;
        for (int i = 0; i < j->ntpl; i++) {
            char nid[WPS_ID_LEN + 1] = "";
            bool moved = !j->tpl_in_system && !sy.err && sy.id[0] && wps_central_move(j->tpl_moved[i], sy.vol, sy.id, false, nid) == 0;
            if (moved) j->tpl_in_system = true;
            else if (wps_central_delete(j->tpl_moved[i]) != 0) continue;   /* (one in System is enough) */
            else nid[0] = 0;
            if (k != i) memcpy(j->tpl_moved[k], j->tpl_moved[i], sizeof j->tpl_moved[0]);
            snprintf(j->tpl_new[k++], sizeof j->tpl_new[0], "%s", nid);
        }
        j->ntpl = k;
        wps_loaded_free(&sy);
        wps_loaded_free(&fl);
    }
}

static bool protect_pending;   /* first-start step 11: Prevent delete, in defaults_done */

/* My System - <host name> and My Storages, unless they have titles (the
 * user's, or these): at step 10, and again for folders made anew (gone
 * from outside, put back by Restore Desktop or wpsd's start). */
static void default_titles(wps_loaded *sysld, wps_loaded *compld)
{
    char host[256] = "", t[300];
    if (gethostname(host, sizeof host - 1) != 0 || !host[0]) snprintf(host, sizeof host, "this computer");
    snprintf(t, sizeof t, "My System - %s", host);
    wpo_object *so = sysld->err ? NULL : wps_object_from_loaded(sysld);
    if (so && wpo_is_a(so, WPObjectClass) && !WPObject_data(so)->title) WPObject_wpSetTitle(so, t);
    wpo_object *co = compld->err ? NULL : wps_object_from_loaded(compld);
    if (co && wpo_is_a(co, WPObjectClass) && !WPObject_data(co)->title) WPObject_wpSetTitle(co, "My Storages");
}

void first_start_done(void *arg)
{
    struct first_start *j = arg;
    if (j->from < FIRST_START_VERSION && j->desk[0]) {
        char marker[PATH_MAX + 32], legacy[PATH_MAX + 32];
        snprintf(marker, sizeof marker, "%s/desktop-made", env.config_dir);
        snprintf(legacy, sizeof legacy, "%s/desktop-setup-made", env.config_dir);
        if (j->from < 1) {   /* step 1: a new desktop: Templates, in System since step 7 */
            const char *td = wps_templates_dir();
            if (td[0]) first_start_shadow(td, j->sys);
        }
        /* step 2 (0.2's first): nothing */
        if (j->from < 3) {
            for (int k = 0; k < j->ngone; k++) {
                wps_central_forget(j->gone[k]);
                on_central_change(j->desk, j->gone[k], 'd');
            }
            first_start_shadow(j->sys, j->desk);
            if (j->viewer[0]) on_central_change(j->sys, j->viewer, 'c');
            /* Startup: the autostart folder, titled so, shown in System */
            wpo_object *as = j->asld.err ? NULL : wps_object_from_loaded(&j->asld);
            if (as && wpo_is_a(as, WPObjectClass) && !WPObject_data(as)->title) WPObject_wpSetTitle(as, "Startup");
            first_start_shadow(j->autostart, j->sys);
        }
        if (j->from < 4 && j->trash[0]) {
            sobj *old = wps_sobj_by_path(j->trash);   /* (loaded as a plain folder before) */
            if (old) wps_sobj_forget(old);
            wpo_object *tc = j->tld.err ? NULL : wps_object_from_loaded(&j->tld);
            if (tc && wpo_is_a(tc, WPObjectClass) && !WPObject_data(tc)->title) WPObject_wpSetTitle(tc, "Trash Can");
            first_start_shadow(j->trash, j->desk);
        }
        if (j->from < 5) {
            char comp[PATH_MAX], drv[PATH_MAX + 8];
            computer_dirs(comp, sizeof comp, drv, sizeof drv);
            for (int k = 0; k < 2; k++) {   /* (seen as plain folders before) */
                sobj *old = wps_sobj_by_path(k ? drv : comp);
                if (old) wps_sobj_forget(old);
            }
            drives_changed();
        }
        if (j->from < 6 && j->programs[0]) {
            sobj *old = wps_sobj_by_path(j->programs);
            if (old) wps_sobj_forget(old);
            first_start_shadow(j->programs, j->desk);
            wps_programs_sync();   /* filled now */
        }
        if (j->from < 7 && j->from >= 1) {
            for (int i = 0; i < j->ntpl; i++) {
                wps_central_forget(j->tpl_moved[i]);
                on_central_change(j->desk, j->tpl_moved[i], 'd');
                if (j->tpl_new[i][0]) on_central_change(j->sys, j->tpl_new[i], 'c');
            }
            const char *td = wps_templates_dir();
            if (!j->tpl_in_system && !j->ntpl && td[0]) first_start_shadow(td, j->sys);
        }
        if (j->restorer[0]) on_central_change(j->sys, j->restorer, 'c');
        if (j->from < 10) default_titles(&j->sysld, &j->compld);   /* step 10 */
        if (j->from < 11) protect_pending = true;   /* step 11 (0.4): Prevent delete, once they are noted (defaults_done) */
        const char *home = getenv("HOME");
        if (j->from < 9 && home && *home) {   /* step 9: the home folder in Computer */
            char comp[PATH_MAX], drv[PATH_MAX + 8];
            computer_dirs(comp, sizeof comp, drv, sizeof drv);
            wps_mkdirs(comp, 0700);
            first_start_shadow(home, comp);
        }
        FILE *f = fopen(marker, "w");
        if (f) {
            fprintf(f, "version %d\n# The desktop's first-start steps done (wpsd); 0.1 wrote no version.\n", FIRST_START_VERSION);
            fclose(f);
        }
        unlink(legacy);
        log_msg("first start: the desktop %s brought from step %d to %d", j->desk, j->from, FIRST_START_VERSION);
    }
    if (j->desk[0]) defaults_start(NULL, 0, j->desk, false);   /* the defaults in place, noted */
    wps_loaded_free(&j->asld);
    wps_loaded_free(&j->sysld);
    wps_loaded_free(&j->compld);
    wps_loaded_free(&j->tld);
    if (!j->c->dead) reply_int(j->c, j->id, j->from);
    client_unref(j->c);
    free(j);
}

/* ---- settings objects (System, Sound, Mouse, Scheme Palette): one of each
 * per user, central objects in the System Setup folder
 * ($XDG_DATA_HOME/wps/System Setup; WPS_SETUP_DIR for tests), made once and
 * remembered by class (locator "setup:<folder ID>", class WPS, prop
 * <class> = object ID). */
static const char *const setup_classes[][2] = {
    { "WPSystem", "System" }, { "WPSound", "Sound" }, { "WPMouse", "Mouse" }, { "WPSchemePalette", "Scheme Palette" },
    { "WPFontPalette", "Font Palette" },   /* (0.3) */
    { "WPColorPalette", "Colour Palette" },   /* (0.4) */
    /* (0.2: in the System folder itself, v0.2-design 9 and 10) */
    { "WPLaunchPad", "LaunchPad" }, { "WPWarpCenter", "WarpCenter" }, { "WPMessages", "Messages" },
};

/* WPS's own folders in the user's data ($XDG_DATA_HOME/wps/<name>). */
void data_dir(const char *name, char *out, size_t n)
{
    const char *dh = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (dh && *dh) snprintf(out, n, "%s/wps%s%s", dh, name[0] ? "/" : "", name);
    else snprintf(out, n, "%s/.local/share/wps%s%s", home ? home : "", name[0] ? "/" : "", name);
}

/* The System folder (OS/2's "OS/2 System", v0.2-design 7). */
void system_dir(char *out, size_t n)
{
    const char *e = getenv("WPS_SYSTEM_DIR");
    if (e && *e) snprintf(out, n, "%s", e);
    else data_dir("System", out, n);
}

/* System Setup: in the System folder since 0.2 (0.1 had it beside it). */
void setup_dir(char *out, size_t n)
{
    const char *e = getenv("WPS_SETUP_DIR");
    if (e && *e) { snprintf(out, n, "%s", e); return; }
    char sys[PATH_MAX], old[PATH_MAX];
    system_dir(sys, sizeof sys);
    snprintf(out, n, "%s/System Setup", sys);
    /* (if 0.1's could not be moved: it is still where it was) */
    data_dir("System Setup", old, sizeof old);
    if (access(out, F_OK) != 0 && access(old, F_OK) == 0) snprintf(out, n, "%s", old);
}

/* At start, before tracking: the System folder made, and 0.1's System
 * Setup moved into it (one rename: its ID, settings objects and the
 * shadows of it go with it). */
void system_folder_init(void)
{
    char sys[PATH_MAX], old[PATH_MAX], now[PATH_MAX];
    system_dir(sys, sizeof sys);
    if (wps_mkdirs(sys, 0700) != 0) { log_msg("cannot make the System folder %s", sys); return; }
    /* 0.3: My Storages' folder is Storages (0.2's Computer), the owner's */
    char was[PATH_MAX + 16], comp[PATH_MAX], drv[PATH_MAX + 8];
    snprintf(was, sizeof was, "%s/Computer", sys);
    computer_dirs(comp, sizeof comp, drv, sizeof drv);
    struct stat cs;
    if (stat(was, &cs) == 0 && S_ISDIR(cs.st_mode) && access(comp, F_OK) != 0) {
        if (rename(was, comp) == 0) {
            wps_shadows_prefix_moved(was, comp);
            log_msg("My Storages: its folder is %s now (was Computer)", comp);
        } else {
            log_msg("cannot rename Computer to Storages: %s", strerror(errno));
        }
    }
    if (getenv("WPS_SETUP_DIR")) return;
    data_dir("System Setup", old, sizeof old);
    snprintf(now, sizeof now, "%s/System Setup", sys);   /* (not setup_dir: it falls back to the old one) */
    struct stat st;
    if (stat(old, &st) == 0 && S_ISDIR(st.st_mode) && access(now, F_OK) != 0) {
        if (rename(old, now) == 0) {
            wps_shadows_prefix_moved(old, now);
            log_msg("System Setup moved into the System folder (%s)", now);
        } else {
            log_msg("cannot move System Setup into the System folder: %s", strerror(errno));
        }
    }
}

/* ---- schemes (object-properties-design.md 2.3): WPScheme objects in
 * System Setup's Schemes folder, made once (Classic, Dark, High contrast;
 * remembered by locator "schemes:<folder ID>", class WPS, prop <title>);
 * the user copies one to make their own. Their values are their declared
 * properties (Classic's are the defaults; the others' written here). */
static const struct { const char *title, *values; } builtin_schemes[] = {
    { "Classic", "" },
    { "Dark", "face=#3c3e42;light=#64666c;shadow=#1e1f22;dark=#0f0f11;text=#e6e6e6;disabled_text=#828282;title_active=#28468c;title_inactive=#46484c;title_text_inactive=#aaaaaa;client_bg=#242528;alt_row=#2a2b2f;selection=#28468c;cursor=#e6e6e6;menu_bg=#3c3e42;menu_text=#e6e6e6;menu_hilite=#28468c;placeholder=#37383c;shadow_text=#78b4ff" },
    { "High contrast", "face=#000000;shadow=#ffffff;dark=#ffffff;text=#ffffff;disabled_text=#00ff00;title_active=#ffff00;title_inactive=#000000;title_text=#000000;title_text_inactive=#ffffff;client_bg=#000000;alt_row=#141414;selection=#ffff00;selection_text=#000000;cursor=#ffffff;menu_bg=#000000;menu_text=#ffffff;menu_hilite=#ffff00;menu_hilite_text=#000000;placeholder=#3c3c3c;shadow_text=#00ffff" },
};
static char schemes_vol[WPS_VOLID_MAX], schemes_fid[WPS_ID_LEN + 1];   /* set once, by the worker */

static void schemes_work(void)
{
    char sd[PATH_MAX], sf[PATH_MAX + 16];
    setup_dir(sd, sizeof sd);
    snprintf(sf, sizeof sf, "%s/Schemes", sd);
    if (wps_mkdirs(sf, 0700) != 0) return;
    wps_loaded fl;
    wps_load(sf, true, &fl);
    if (fl.err || !fl.id[0]) { wps_loaded_free(&fl); return; }
    wps_props known;
    wps_props_init(&known);
    char sloc[WPS_ID_LEN + 16];
    snprintf(sloc, sizeof sloc, "schemes:%s", fl.id);
    wps_db_get_class(db, fl.vol, sloc, "WPS", &known);
    for (size_t i = 0; i < sizeof builtin_schemes / sizeof *builtin_schemes; i++) {
        char *id = NULL, vol[WPS_VOLID_MAX];
        if (wps_props_str(&known, builtin_schemes[i].title, &id) && id && wps_db_vobj_vol(db, id, vol, sizeof vol)) { free(id); continue; }
        free(id);
        char nid[WPS_ID_LEN + 1], key[512], loc[WPS_ID_LEN + 8];
        if (wps_central_create(fl.vol, fl.id, "WPScheme", builtin_schemes[i].title, NULL, nid)) continue;
        snprintf(loc, sizeof loc, "vobj:%s", nid);
        char vals[2048];
        snprintf(vals, sizeof vals, "%s", builtin_schemes[i].values);
        for (char *q = vals, *pair; (pair = strsep(&q, ";"));) {
            char *eq = strchr(pair, '=');
            if (!eq) continue;
            *eq = 0;
            wps_db_key(key, sizeof key, loc, "WPScheme", pair);
            wps_db_put(db, fl.vol, key, 's', eq + 1, (uint32_t)strlen(eq + 1));
        }
        wps_db_key(key, sizeof key, sloc, "WPS", builtin_schemes[i].title);
        wps_db_put(db, fl.vol, key, 's', nid, WPS_ID_LEN);
    }
    snprintf(schemes_vol, sizeof schemes_vol, "%s", fl.vol);
    snprintf(schemes_fid, sizeof schemes_fid, "%s", fl.id);
    wps_props_free(&known);
    wps_loaded_free(&fl);
}

/* The schemes there are: central objects of the Schemes folder. */
struct scheme_ids { char id[32][WPS_ID_LEN + 1]; size_t n; };

static void collect_scheme(const char *vol, const char *id, void *user)
{
    (void)vol;
    struct scheme_ids *x = user;
    if (x->n < 32) snprintf(x->id[x->n++], WPS_ID_LEN + 1, "%s", id);
}

static void scheme_ids_of(struct scheme_ids *x)
{
    x->n = 0;
    if (schemes_fid[0]) wps_central_in(schemes_vol, schemes_fid, collect_scheme, x);
}

static int by_title(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

/* The Scheme Palette's choices (loop thread): the schemes' titles, WPS's
 * own first (Classic, Dark, High contrast), then the user's by title. */
char *scheme_titles(void)
{
    struct scheme_ids x;
    scheme_ids_of(&x);
    char *titles[32];
    size_t nt = 0;
    for (size_t i = 0; i < x.n; i++) {
        char loc[WPS_ID_LEN + 8];
        snprintf(loc, sizeof loc, "vobj:%s", x.id[i]);
        wps_loaded ld;
        wps_load(loc, false, &ld);
        wpo_object *o = ld.err ? NULL : wps_object_from_loaded(&ld);
        wps_loaded_free(&ld);
        char *t = o && wpo_is_a(o, WPSchemeClass) ? WPObject_wpQueryTitle(o) : NULL;   /* (o: borrowed) */
        if (t && *t && !strchr(t, ',') && nt < 32) titles[nt++] = t;
        else free(t);
    }
    qsort(titles, nt, sizeof *titles, by_title);
    char *out = strdup("");
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < nt; i++) {
            bool own = false;
            for (size_t b = 0; b < sizeof builtin_schemes / sizeof *builtin_schemes; b++) own |= !strcasecmp(titles[i], builtin_schemes[b].title);
            if (own != (pass == 0)) continue;
            char *r = malloc(strlen(out) + strlen(titles[i]) + 2);
            sprintf(r, "%s%s%s", out, *out ? "," : "", titles[i]);
            free(out);
            out = r;
        }
    for (size_t i = 0; i < nt; i++) free(titles[i]);
    return out;
}

bool is_schemes_folder(const char *folder_id) { return schemes_fid[0] && folder_id && !strcmp(folder_id, schemes_fid); }

bool is_schemes_path(const char *folder)
{
    char sd[PATH_MAX], sf[PATH_MAX + 16];
    setup_dir(sd, sizeof sd);
    snprintf(sf, sizeof sf, "%s/Schemes", sd);
    return folder && !strcmp(folder, sf);
}

static void setup_objects_work(void *arg)
{
    (void)arg;
    char sd[PATH_MAX];
    setup_dir(sd, sizeof sd);
    if (wps_mkdirs(sd, 0700) != 0) return;
    wps_loaded fl;
    wps_load(sd, true, &fl);
    if (fl.err || !fl.id[0]) { wps_loaded_free(&fl); return; }
    wps_props known;
    wps_props_init(&known);
    char sloc[WPS_ID_LEN + 8];
    snprintf(sloc, sizeof sloc, "setup:%s", fl.id);   /* by the folder: one set per System Setup folder */
    wps_db_get_class(db, fl.vol, sloc, "WPS", &known);
    for (size_t i = 0; i < NSETUP; i++) {
        char *id = NULL, vol[WPS_VOLID_MAX];
        if (wps_props_str(&known, setup_classes[i][0], &id) && id && wps_db_vobj_vol(db, id, vol, sizeof vol)) { free(id); continue; }
        free(id);
        char nid[WPS_ID_LEN + 1], key[256];
        if (i >= SETUP_IN_SYSTEM) {
            char sys[PATH_MAX];
            system_dir(sys, sizeof sys);
            wps_loaded sy;
            wps_load(sys, true, &sy);
            int rc = sy.err || !sy.id[0] ? -1 : wps_central_create(sy.vol, sy.id, setup_classes[i][0], setup_classes[i][1], NULL, nid);
            wps_loaded_free(&sy);
            if (rc) continue;
        } else if (wps_central_create(fl.vol, fl.id, setup_classes[i][0], setup_classes[i][1], NULL, nid)) continue;
        wps_db_key(key, sizeof key, sloc, "WPS", setup_classes[i][0]);
        wps_db_put(db, fl.vol, key, 's', nid, WPS_ID_LEN);
    }
    wps_props_free(&known);
    wps_loaded_free(&fl);
    schemes_work();
}

/* The System object, read once the settings objects are there (its
 * Terminal), and kept: its changes come through on_object_changed. */
static wps_loaded system_ld;

static wpo_object *system_obj;

static void system_load_work(void *arg)
{
    (void)arg;
    char sd[PATH_MAX];
    setup_dir(sd, sizeof sd);
    wps_loaded fl;
    wps_load(sd, false, &fl);
    wps_props known;
    wps_props_init(&known);
    char sloc[WPS_ID_LEN + 8], *id = NULL;
    snprintf(sloc, sizeof sloc, "setup:%s", fl.id);
    system_ld.err = ENOENT;
    if (!fl.err && fl.id[0]) wps_db_get_class(db, fl.vol, sloc, "WPS", &known);
    if (wps_props_str(&known, "WPSystem", &id) && id) {
        char loc[WPS_ID_LEN + 8];
        snprintf(loc, sizeof loc, "vobj:%s", id);
        wps_load(loc, false, &system_ld);
    }
    free(id);
    wps_props_free(&known);
    wps_loaded_free(&fl);
}

static void system_loaded(void *arg)
{
    (void)arg;
    wpo_object *o = system_ld.err ? NULL : wps_object_from_loaded(&system_ld);
    wps_loaded_free(&system_ld);
    if (o && wpo_is_a(o, WPSystemClass)) { system_obj = o; terminal_from_system(o); }
    else wpo_unref(o);
}

static void system_load_done(void *arg)
{
    (void)arg;
    if (!system_obj) wpo_defer(loop, "the System object", system_load_work, system_loaded, NULL);
}

void builtin_templates_done(void *arg)
{
    (void)arg;
    wps_listing_invalidate(wps_templates_dir());
    wpo_defer(loop, "settings objects", setup_objects_work, system_load_done, NULL);
}

void autostart_dir(char *out, size_t n)
{
    const char *xc = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xc && *xc) snprintf(out, n, "%s/autostart", xc);
    else snprintf(out, n, "%s/.config/autostart", home ? home : "");
}

static int defaults_list(const char *desk, struct defobj *d)
{
    char sys[PATH_MAX], home[PATH_MAX];
    system_dir(sys, sizeof sys);
    wps_trash_home(home, sizeof home);
    int n = 0;
    d[n] = (struct defobj){ "desktop.system", "My System", NULL, "", "" };
    snprintf(d[n].target, sizeof d[n].target, "%s", sys);
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", desk);
    d[n] = (struct defobj){ "desktop.programs", "Programs", NULL, "", "" };
    data_dir("Programs", d[n].target, sizeof d[n].target);
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", desk);
    d[n] = (struct defobj){ "desktop.trash", "Trash Can", NULL, "", "" };
    snprintf(d[n].target, sizeof d[n].target, "%s/files", home);
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", desk);
    if (wps_templates_dir()[0]) {
        d[n] = (struct defobj){ "system.templates", "Templates", NULL, "", "" };
        snprintf(d[n].target, sizeof d[n].target, "%s", wps_templates_dir());
        snprintf(d[n++].folder, sizeof d[0].folder, "%s", sys);
    }
    d[n] = (struct defobj){ "system.startup", "Startup", NULL, "", "" };
    autostart_dir(d[n].target, sizeof d[n].target);
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", sys);
    d[n] = (struct defobj){ "system.viewer", "Minimized Window Viewer", "WPMinWinViewer", "", "" };
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", sys);
    d[n] = (struct defobj){ "system.restore", "Restore Desktop", "WPRestoreDesktop", "", "" };
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", sys);
    const char *uh = getenv("HOME");
    if (uh && *uh) {
        char drv[PATH_MAX + 8];
        d[n] = (struct defobj){ "computer.home", "Home", NULL, "", "" };
        snprintf(d[n].target, sizeof d[n].target, "%s", uh);
        computer_dirs(d[n].folder, sizeof d[n].folder, drv, sizeof drv);
        n++;
    }
    return n;
}

static void def_match_one(const char *vol, const char *id, void *user)
{
    (void)vol;
    struct def_match *m = user;
    if (m->found[0]) return;
    char loc[WPS_ID_LEN + 8];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_loaded ld;
    wps_load(loc, false, &ld);
    if (!ld.err && (m->cls ? !strcmp(ld.obj_class, m->cls) : m->tid && !strcmp(ld.shadow_of, m->tid)))
        snprintf(m->found, sizeof m->found, "%s", id);
    wps_loaded_free(&ld);
}

/* One like it (a shadow of tid, or of class cls) among folder's central objects. */
static bool def_find_in(const wps_loaded *folder, const char *tid, const char *cls, char out[WPS_ID_LEN + 1])
{
    if (folder->err || !folder->id[0]) return false;
    struct def_match m = { tid, cls, "" };
    wps_central_in(folder->vol, folder->id, def_match_one, &m);
    if (!m.found[0]) return false;
    memcpy(out, m.found, WPS_ID_LEN + 1);
    return true;
}

static void def_note(const char *vol, const char *name, const char *id)
{
    char key[256];
    wps_db_key(key, sizeof key, DEFAULTS_LOC, "WPS", name);
    wps_db_put(db, vol, key, 's', id, WPS_ID_LEN);
}

static void def_event(struct defaults_job *j, const char *folder, const char *id, const char *old_folder, const char *old_id)
{
    if (j->nev >= 24) return;
    snprintf(j->ev[j->nev].folder, sizeof j->ev[0].folder, "%s", folder);
    snprintf(j->ev[j->nev].id, sizeof j->ev[0].id, "%s", id);
    snprintf(j->ev[j->nev].old_folder, sizeof j->ev[0].old_folder, "%s", old_folder ? old_folder : "");
    snprintf(j->ev[j->nev].old_id, sizeof j->ev[0].old_id, "%s", old_id ? old_id : "");
    j->nev++;
}

/* Move central object `id` into folder `to` (loaded); its old folder's
 * path for the event. */
static bool def_move(struct defaults_job *j, const char *id, const wps_loaded *to, const char *to_path, char nid[WPS_ID_LEN + 1])
{
    char loc[WPS_ID_LEN + 8], from[PATH_MAX] = "";
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_loaded ld, fl;
    wps_load(loc, false, &ld);
    if (ld.err) { wps_loaded_free(&ld); return false; }
    snprintf(loc, sizeof loc, "id:%s", ld.folder_id);
    wps_load(loc, false, &fl);
    if (!fl.err) snprintf(from, sizeof from, "%s", fl.path);
    wps_loaded_free(&fl);
    wps_loaded_free(&ld);
    if (wps_central_move(id, to->vol, to->id, false, nid) != 0) return false;
    def_event(j, to_path, nid, from, id);
    return true;
}

static bool def_made_dir(const char *path)
{
    if (access(path, F_OK) == 0) return false;
    return wps_mkdirs(path, 0700) == 0;
}

static void defaults_work(void *arg)
{
    struct defaults_job *j = arg;
    char sys[PATH_MAX], sd[PATH_MAX], home[PATH_MAX], tmp[PATH_MAX + 16];
    system_dir(sys, sizeof sys);
    setup_dir(sd, sizeof sd);
    wps_trash_home(home, sizeof home);
    if (j->restore) {
        /* the default folders, if gone (they come back empty, of their class) */
        if (def_made_dir(sys)) j->put_back[j->nput++] = "the System folder";
        if (def_made_dir(sd)) j->put_back[j->nput++] = "System Setup";
        autostart_dir(tmp, sizeof tmp);
        if (def_made_dir(tmp)) j->put_back[j->nput++] = "the Startup folder";
        if (wps_templates_dir()[0] && def_made_dir(wps_templates_dir())) j->put_back[j->nput++] = "the Templates folder";
        data_dir("Programs", tmp, sizeof tmp);
        if (def_made_dir(tmp)) { j->made_programs = true; j->put_back[j->nput++] = "the Programs folder"; }
        wps_set_folder_class(tmp, "WPProgramsFolder");
        snprintf(tmp, sizeof tmp, "%s/files", home);
        if (def_made_dir(tmp)) { j->made_trash = true; j->put_back[j->nput++] = "the Trash Can"; }
        wps_set_folder_class(tmp, "WPTrashCan");
        snprintf(tmp, sizeof tmp, "%s/info", home);
        wps_mkdirs(tmp, 0700);
        char comp[PATH_MAX], drv[PATH_MAX + 8];
        computer_dirs(comp, sizeof comp, drv, sizeof drv);
        if (def_made_dir(drv)) { j->made_computer = true; j->put_back[j->nput++] = "My Storages"; }
        wps_set_folder_class(comp, "WPComputer");
        wps_set_folder_class(drv, "WPDrives");
    }
    {   /* (titled in defaults_done, if made anew) */
        char comp[PATH_MAX], drv[PATH_MAX + 8];
        computer_dirs(comp, sizeof comp, drv, sizeof drv);
        wps_load(sys, true, &j->sysld);
        wps_load(comp, true, &j->compld);
    }
    wps_loaded sy;
    wps_load(sys, true, &sy);
    if (sy.err || !sy.id[0]) { wps_loaded_free(&sy); return; }
    wps_props known;
    wps_props_init(&known);
    wps_db_get_class(db, sy.vol, DEFAULTS_LOC, "WPS", &known);
    char key[256], *dk = NULL;
    wps_db_key(key, sizeof key, DEFAULTS_LOC, "WPS", "desktop");
    if (!j->desk[0] && wps_props_str(&known, "desktop", &dk) && dk) snprintf(j->desk, sizeof j->desk, "%s", dk);
    else if (j->desk[0] && (!wps_props_str(&known, "desktop", &dk) || !dk || strcmp(dk, j->desk)))
        wps_db_put(db, sy.vol, key, 's', j->desk, (uint32_t)strlen(j->desk));   /* (wpsctl restore-defaults) */
    free(dk);
    if (!j->desk[0]) { j->no_desktop = true; wps_props_free(&known); wps_loaded_free(&sy); return; }
    char trash_files[PATH_MAX + 8];
    snprintf(trash_files, sizeof trash_files, "%s/files", home);
    wps_loaded tr;
    wps_load(trash_files, false, &tr);
    struct defobj d[NDEFAULTS];
    int n = defaults_list(j->desk, d);
    for (int i = 0; i < n; i++) {
        wps_loaded fl, tl;
        wps_load(d[i].folder, true, &fl);
        if (fl.err || !fl.id[0] || !S_ISDIR(fl.st.st_mode)) { wps_loaded_free(&fl); continue; }
        char tid[WPS_ID_LEN + 1] = "";
        if (!d[i].cls) {
            wps_load(d[i].target, true, &tl);
            if (!tl.err) snprintf(tid, sizeof tid, "%s", tl.shadow_of[0] ? tl.shadow_of : tl.id);
            wps_loaded_free(&tl);
            if (!tid[0]) { wps_loaded_free(&fl); continue; }
        }
        char *rec = NULL, nid[WPS_ID_LEN + 1] = "";
        wps_props_str(&known, d[i].name, &rec);
        bool done = false;
        if (rec && strlen(rec) == WPS_ID_LEN) {
            /* the one noted: in place, or put back */
            char loc[WPS_ID_LEN + 8];
            snprintf(loc, sizeof loc, "vobj:%s", rec);
            wps_loaded ld;
            wps_load(loc, false, &ld);
            if (!ld.err && ld.central) {
                done = true;
                if (strcmp(ld.folder_id, fl.id) && j->restore && def_move(j, rec, &fl, d[i].folder, nid)) {
                    def_note(sy.vol, d[i].name, nid);
                    j->put_back[j->nput++] = d[i].title;
                }
            }
            wps_loaded_free(&ld);
        }
        if (!done && def_find_in(&fl, tid, d[i].cls, nid)) {   /* one is there: noted from now on */
            def_note(sy.vol, d[i].name, nid);
            done = true;
        }
        if (!done && j->restore) {
            char was[WPS_ID_LEN + 1];
            if (def_find_in(&tr, tid, d[i].cls, was) && def_move(j, was, &fl, d[i].folder, nid)) {   /* from the Trash Can */
                def_note(sy.vol, d[i].name, nid);
            } else if (wps_central_create(fl.vol, fl.id, d[i].cls ? d[i].cls : "WPShadow", d[i].cls ? d[i].title : NULL,
                                          d[i].cls ? NULL : tid, nid) == 0) {
                def_note(sy.vol, d[i].name, nid);
                def_event(j, d[i].folder, nid, NULL, NULL);
            } else nid[0] = 0;
            if (nid[0]) j->put_back[j->nput++] = d[i].title;
        }
        free(rec);
        wps_loaded_free(&fl);
    }
    wps_props_free(&known);
    if (j->restore) {
        /* the settings objects: each back in its folder; any gone, made again */
        wps_loaded sf;
        wps_load(sd, true, &sf);
        if (!sf.err && sf.id[0]) {
            char sloc[WPS_ID_LEN + 8];
            snprintf(sloc, sizeof sloc, "setup:%s", sf.id);
            wps_props_init(&known);
            wps_db_get_class(db, sf.vol, sloc, "WPS", &known);
            char before[NSETUP][WPS_ID_LEN + 1];
            for (size_t i = 0; i < NSETUP; i++) {
                char *id = NULL, nid[WPS_ID_LEN + 1];
                before[i][0] = 0;
                if (!wps_props_str(&known, setup_classes[i][0], &id) || !id || strlen(id) != WPS_ID_LEN) { free(id); continue; }
                snprintf(before[i], sizeof before[i], "%s", id);
                char loc[WPS_ID_LEN + 8];
                snprintf(loc, sizeof loc, "vobj:%s", id);
                wps_loaded ld;
                wps_load(loc, false, &ld);
                const wps_loaded *home_f = i >= SETUP_IN_SYSTEM ? &sy : &sf;
                if (!ld.err && strcmp(ld.folder_id, home_f->id) &&
                    def_move(j, id, home_f, i >= SETUP_IN_SYSTEM ? sys : sd, nid)) {
                    char key[256];
                    wps_db_key(key, sizeof key, sloc, "WPS", setup_classes[i][0]);
                    wps_db_put(db, sf.vol, key, 's', nid, WPS_ID_LEN);
                    snprintf(before[i], sizeof before[i], "%s", nid);
                    j->put_back[j->nput++] = setup_classes[i][1];
                }
                wps_loaded_free(&ld);
                free(id);
            }
            wps_props_free(&known);
            setup_objects_work(NULL);   /* makes the ones gone */
            wps_props_init(&known);
            wps_db_get_class(db, sf.vol, sloc, "WPS", &known);
            for (size_t i = 0; i < NSETUP; i++) {
                char *id = NULL;
                if (wps_props_str(&known, setup_classes[i][0], &id) && id && strlen(id) == WPS_ID_LEN && strcmp(id, before[i])) {
                    def_event(j, i >= SETUP_IN_SYSTEM ? sys : sd, id, NULL, NULL);
                    j->put_back[j->nput++] = setup_classes[i][1];
                }
                free(id);
            }
            wps_props_free(&known);
        }
        wps_loaded_free(&sf);
    }
    wps_loaded_free(&tr);
    wps_loaded_free(&sy);
}

/* First-start step 11 (0.4), and Restore Desktop: WPS's own objects get
 * Prevent delete (no_delete, their General page; the user may turn it
 * off) - its folders, the default objects and the desktop's shadows of
 * them, the settings objects. (protect_pending: set by the first start) */

static void protect(const char *target)
{
    wps_loaded ld;
    wps_load(target, false, &ld);
    wpo_object *o = ld.err ? NULL : wps_object_from_loaded(&ld);   /* (borrowed) */
    wps_loaded_free(&ld);
    char err[128];
    if (o && wpo_is_a(o, WPObjectClass) && !apply_setup(o, "no_delete=1", err, sizeof err))
        log_msg("prevent delete: %s: %s", target, err);
}

static void protect_defaults(const char *desk)
{
    char t[PATH_MAX + 16], drv[PATH_MAX + 8], home[PATH_MAX];
    system_dir(t, sizeof t); protect(t);
    setup_dir(t, sizeof t); protect(t);
    data_dir("Programs", t, sizeof t); protect(t);
    wps_trash_home(home, sizeof home);
    snprintf(t, sizeof t, "%s/files", home); protect(t);
    if (wps_templates_dir()[0]) protect(wps_templates_dir());
    autostart_dir(t, sizeof t); protect(t);
    computer_dirs(t, sizeof t, drv, sizeof drv); protect(t); protect(drv);
    /* the default objects (central: their shadows, the viewer, Restore Desktop), as noted */
    char sys[PATH_MAX];
    system_dir(sys, sizeof sys);
    wps_loaded sy;
    wps_load(sys, true, &sy);
    if (!sy.err && sy.id[0]) {
        wps_props known;
        wps_props_init(&known);
        wps_db_get_class(db, sy.vol, DEFAULTS_LOC, "WPS", &known);
        struct defobj d[NDEFAULTS];
        int n = defaults_list(desk, d);
        for (int i = 0; i < n; i++) {
            char *id = NULL;
            if (wps_props_str(&known, d[i].name, &id) && id && strlen(id) == WPS_ID_LEN) {
                snprintf(t, sizeof t, "vobj:%s", id);
                protect(t);
            }
            free(id);
        }
        wps_props_free(&known);
    }
    wps_loaded_free(&sy);
    /* the settings objects (System Setup's, and those in System) */
    setup_dir(t, sizeof t);
    wps_loaded fl;
    wps_load(t, false, &fl);
    if (!fl.err && fl.id[0]) {
        wps_props known;
        wps_props_init(&known);
        char sloc[WPS_ID_LEN + 8];
        snprintf(sloc, sizeof sloc, "setup:%s", fl.id);
        wps_db_get_class(db, fl.vol, sloc, "WPS", &known);
        for (size_t i = 0; i < NSETUP; i++) {
            char *id = NULL;
            if (wps_props_str(&known, setup_classes[i][0], &id) && id && strlen(id) == WPS_ID_LEN) {
                snprintf(t, sizeof t, "vobj:%s", id);
                protect(t);
            }
            free(id);
        }
        wps_props_free(&known);
    }
    wps_loaded_free(&fl);
}

static void defaults_done(void *arg)
{
    struct defaults_job *j = arg;
    if ((protect_pending || j->restore) && j->desk[0] && !j->no_desktop) {
        protect_pending = false;
        protect_defaults(j->desk);
    }
    for (int i = 0; i < j->nev; i++) {
        if (j->ev[i].old_id[0]) {
            wps_central_forget(j->ev[i].old_id);
            if (j->ev[i].old_folder[0]) on_central_change(j->ev[i].old_folder, j->ev[i].old_id, 'd');
        }
        on_central_change(j->ev[i].folder, j->ev[i].id, 'c');
    }
    char home[PATH_MAX], tmp[PATH_MAX + 16];
    if (j->made_programs) {
        data_dir("Programs", tmp, sizeof tmp);
        sobj *old = wps_sobj_by_path(tmp);
        if (old) wps_sobj_forget(old);
        wps_programs_sync();
    }
    if (j->made_trash) {
        wps_trash_home(home, sizeof home);
        snprintf(tmp, sizeof tmp, "%s/files", home);
        sobj *old = wps_sobj_by_path(tmp);
        if (old) wps_sobj_forget(old);
    }
    if (j->made_computer) drives_changed();
    if (j->restore && j->no_desktop) {
        if (j->c && !j->c->dead) reply_error(j->c, j->id, "no desktop", "the desktop has not opened yet: name its folder");
    } else if (j->restore) {
        if (j->nput) log_msg("restore defaults: %d put back", j->nput);
        if (j->c && !j->c->dead) {
            cbuf val;
            cb_init(&val);
            cb_map(&val, 1);
            cb_text(&val, "restored");
            cb_array(&val, (size_t)j->nput);
            for (int i = 0; i < j->nput; i++) cb_text(&val, j->put_back[i]);
            reply_value_cbor(j->c, j->id, &val);
            cb_free(&val);
        }
    }
    default_titles(&j->sysld, &j->compld);
    wps_loaded_free(&j->sysld);
    wps_loaded_free(&j->compld);
    if (j->c) client_unref(j->c);
    free(j);
}

void defaults_start(struct client *c, int64_t id, const char *desk, bool restore)
{
    struct defaults_job *j = calloc(1, sizeof *j);
    if (c) { j->c = c; c->refs++; }
    j->id = id;
    j->restore = restore;
    snprintf(j->desk, sizeof j->desk, "%s", desk);
    wpo_defer(loop, restore ? "restore defaults" : "note defaults", defaults_work, defaults_done, j);
}

void setup_req_work(void *arg)
{
    struct setup_req *r = arg;
    setup_dir(r->folder, sizeof r->folder);
    wps_loaded fl;
    wps_load(r->folder, false, &fl);
    wps_props known;
    wps_props_init(&known);
    char sloc[WPS_ID_LEN + 8];
    snprintf(sloc, sizeof sloc, "setup:%s", fl.id);
    if (!fl.err && fl.id[0]) wps_db_get_class(db, fl.vol, sloc, "WPS", &known);
    for (size_t i = 0; i < NSETUP; i++) {
        char *id = NULL, loc[WPS_ID_LEN + 8];
        r->ld[i].err = ENOENT;
        if (wps_props_str(&known, setup_classes[i][0], &id) && id) {
            snprintf(loc, sizeof loc, "vobj:%s", id);
            wps_load(loc, false, &r->ld[i]);
        }
        free(id);
    }
    wps_props_free(&known);
    wps_loaded_free(&fl);
    struct scheme_ids x;
    scheme_ids_of(&x);
    r->nsch = 0;
    for (size_t i = 0; i < x.n && r->nsch < 32; i++) {
        char loc[WPS_ID_LEN + 8];
        snprintf(loc, sizeof loc, "vobj:%s", x.id[i]);
        wps_load(loc, false, &r->sch[r->nsch]);
        if (r->sch[r->nsch].err) wps_loaded_free(&r->sch[r->nsch]);
        else r->nsch++;
    }
}

/* A scheme's values: its declared properties, "key=value;..." */
static char *scheme_values(wpo_object *o)
{
    char *out = strdup("");
    for (size_t i = 0; i < WPSchemeClass->nprops; i++) {
        const char *k = WPSchemeClass->props[i].name;
        char *v = WPObject_wpQueryProperty(o, k);
        char *r = malloc(strlen(out) + strlen(k) + strlen(v ? v : "") + 3);
        sprintf(r, "%s%s=%s;", out, k, v ? v : "");
        free(out);
        free(v);
        out = r;
    }
    return out;
}

void setup_req_done(void *arg)

{
    struct setup_req *r = arg;
    if (!r->c->dead) {
        cbuf b;
        cb_init(&b);
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, r->id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "result");
        cb_map(&b, 3);
        cb_text(&b, "folder"); cb_text(&b, r->folder);
        /* the schemes: [{title, address, values "key=value;..."}] */
        wpo_object *so[32] = { 0 };
        size_t nso = 0;
        for (size_t i = 0; i < r->nsch; i++) {
            wpo_object *o = wps_object_from_loaded(&r->sch[i]);
            if (o && wpo_is_a(o, WPSchemeClass)) so[nso++] = o;   /* (the service's: borrowed) */
        }
        cb_text(&b, "schemes");
        cb_array(&b, nso);
        for (size_t i = 0; i < nso; i++) {
            char *t = WPObject_wpQueryTitle(so[i]), *v = scheme_values(so[i]), addr[WPS_ID_LEN + 8];
            snprintf(addr, sizeof addr, "vobj:%s", WPObject_data(so[i])->id);
            cb_map(&b, 3);
            cb_text(&b, "title"); cb_text(&b, t ? t : "");
            cb_text(&b, "address"); cb_text(&b, addr);
            cb_text(&b, "values"); cb_text(&b, v);
            free(t);
            free(v);
        }

        size_t n = 0;
        wpo_object *objs[NSETUP] = { 0 };
        for (size_t i = 0; i < NSETUP; i++)
            if (!r->ld[i].err && (objs[i] = wps_object_from_loaded(&r->ld[i]))) n++;
        cb_text(&b, "objects");
        cb_map(&b, n);
        for (size_t i = 0; i < NSETUP; i++) {
            if (!objs[i]) continue;
            char *text = NULL;
            wpo_value rv;
            if (wpo_send(objs[i], "wpQuerySettings", NULL, 0, &rv) == WPO_OK && rv.type == 's') { text = rv.u.s; rv.u.s = NULL; }
            wpo_value_clear(&rv);
            if (!text) text = strdup("");
            size_t nk = 0;
            for (const char *p = text; *p; p++) nk += *p == '\n';
            cb_text(&b, setup_classes[i][0]);
            cb_map(&b, 2);
            char addr[WPS_ID_LEN + 8];
            snprintf(addr, sizeof addr, "vobj:%s", WPObject_data(objs[i])->id);
            cb_text(&b, "address"); cb_text(&b, addr);
            cb_text(&b, "settings");
            cb_map(&b, nk);
            for (char *line = text, *nl; (nl = strchr(line, '\n')); line = nl + 1) {
                *nl = 0;   /* key TAB value */
                char *tab = strchr(line, '\t');
                if (tab) *tab = 0;
                cb_text(&b, line);
                cb_text(&b, tab ? tab + 1 : "");
            }
            free(text);
        }
        send_frame(r->c, &b);
        cb_free(&b);
    }
    for (size_t i = 0; i < NSETUP; i++) wps_loaded_free(&r->ld[i]);
    for (size_t i = 0; i < r->nsch; i++) wps_loaded_free(&r->sch[i]);
    client_unref(r->c);
    free(r);
}

static void shadow_recorded(void *arg)
{
    struct shadow_reply *r = arg;
    reply_simple_ok(r->s, r->link);
    free(r);
}

void shadow_made(void *arg, const char *link, const char *error)
{
    struct simple *s = arg;
    if (link) {
        /* Reply once the folder's record is written, so the shadow is a
         * shadow for everyone from the moment it is reported. */
        struct shadow_reply *r = calloc(1, sizeof *r);
        r->s = s;
        snprintf(r->link, sizeof r->link, "%s", link);
        wps_objects_flush(shadow_recorded, r);
        return;
    }
    if (!s->c->dead) reply_error(s->c, s->id, "failed", error);
    client_unref(s->c);
    free(s);
}

void quit_after_flush(void *arg) { (void)arg; quitting = true; wpo_loop_quit(loop); }

void shutdown_flushed(void *arg)
{
    reply_simple_ok(arg, "shutting down");
    quit_after_flush(NULL);
}

void startup_work(void *arg)
{
    struct startup_job *j = arg;
    wps_startup_dir(j->dir, sizeof j->dir);
    wps_mkdirs(j->dir, 0755);
    wps_loaded ld;
    wps_load(j->target, false, &ld);
    char path[PATH_MAX] = "";
    if (!ld.err && ld.shadow_of[0]) {   /* a shadow: what it stands for */
        char loc[WPS_ID_LEN + 8];
        snprintf(loc, sizeof loc, "id:%s", ld.shadow_of);
        wps_loaded tl;
        wps_load(loc, false, &tl);
        if (!tl.err && !tl.central) snprintf(path, sizeof path, "%s", tl.path);
        wps_loaded_free(&tl);
    } else if (!ld.err && !ld.central) snprintf(path, sizeof path, "%s", ld.path);
    j->err = ld.err ? ld.err : path[0] ? wps_startup_add(path, j->made, sizeof j->made) : ENOEXEC;
    wps_loaded_free(&ld);
}

void startup_done(void *arg)
{
    struct startup_job *j = arg;
    if (j->err == ENOEXEC) {   /* not a program: a shadow, opened at login */
        struct simple *s = calloc(1, sizeof *s);
        s->c = j->c; s->id = j->id;   /* (takes the reference) */
        wps_shadow_create(j->target, j->dir, shadow_made, s);
        free(j);
        return;
    }
    if (!j->c->dead) {
        if (j->err) reply_error(j->c, j->id, "failed", strerror(j->err));
        else {
            struct simple *s = calloc(1, sizeof *s);
            s->c = j->c; s->id = j->id; j->c->refs++;
            reply_simple_ok(s, j->made);
        }
    }
    client_unref(j->c);
    free(j);
}

bool startup_query(wpo_object *o, bool *on)
{
    const char *p = wps_object_path(o);
    if (!p || !wps_startup_is_entry(p)) return false;
    *on = wps_startup_enabled(p);
    return true;
}

bool startup_set(wpo_object *o, bool on)
{
    const char *p = wps_object_path(o);
    return p && wps_startup_set_enabled(p, on) == 0;
}
