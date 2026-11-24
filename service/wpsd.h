/* wpsd, the object service: what its files share - clients, requests,
 * replies, subscriptions and every function and variable used across them.
 * Split from wpsd.c (refactor R1); see each file's top for its part. */
#ifndef WPSD_H
#define WPSD_H


#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <dirent.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <ctype.h>
#include <spawn.h>
#include <sys/xattr.h>

#include "cbor.h"
#include "db.h"
#include "env.h"
#include "objects.h"
#include "jobs.h"
#include "trash.h"
#include "udisks.h"
#include "programs.h"
#include "launch.h"
#include "wps_menu_ids.h"
#include "listing.h"
#include "find.h"
#include "plugins.h"
#include "shadows.h"
#include "WPShadow.h"
#include "WPProgram.h"
#include "WPSystem.h"
#include "WPScheme.h"
#include "WPSetup.h"
#include "WPFontPalette.h"
#include "WPColorPalette.h"
#include "WPFolder.h"
#include "WPTrashCan.h"
#include "WPDrive.h"
#include "WPMinWinViewer.h"
#include "WPRestoreDesktop.h"
#include "WPMessages.h"
#include "WPLaunchPad.h"
#include "WPWarpCenter.h"
#include "messages.h"
#include "startup.h"
#include "wps_startup_hook.h"
#include "WPDataFile.h"
#include "wps_state.h"
#include "tracker.h"
#include "volumes.h"
#include "wpo.h"
#include "wpo_loop.h"
#include "wps_classes.h"
#include "WPObject.h"

#define MAX_FRAME (1u << 20)
#define MAX_ARGS 16

/* ------------------------------------------------------------- clients */

struct client {
    struct client *next, *prev;   /* all open connections, for shutdown */
    int fd;
    int refs;          /* the connection plus requests in flight */
    bool broken;           /* a send failed: closed by the loop next (not while events are sent) */
    bool dead;
    unsigned char *in;
    size_t inlen, incap;
    unsigned char *out;
    size_t outlen, outoff;
    /* its desktop session, from its last open (for actions: wpsd_actions.c) */
    char display[108], x11[24], envs[8][300];
    size_t nenvs;
};
struct drive_req { struct client *c; int64_t id; bool open; char uuid[80], action[16], *pass; };

/* ------------------------------------------------------------ requests */

struct req {
    struct client *c;
    int64_t id;
    cv msg;
    const char *op;
    char target[PATH_MAX];
    wps_loaded tgt;
    wps_loaded shadow_target;    /* a shadow's target, when the target is a shadow */
    char *remote_text;           /* a plug-in's answer, for menus (and its settings pages) */
    bool remote_done, remote_ok; /* a plug-in handled it (menu_select, a setter) */
    char *open_with;             /* a data file's Open With items, made on the worker (menu) */
    int nobj;
    char objpath[MAX_ARGS][PATH_MAX];
    wps_loaded *objs;
};

/* A method implemented by a plug-in: run in its host; reply when it
 * answers, or with the parent's behaviour if it misses its deadline. */
struct remote_ctx { struct req *r; wpo_object *o; };

struct simple { struct client *c; int64_t id; };

struct stats_job { struct client *c; int64_t id; int64_t rows, conflicts; };

/* ---- volumes: scan, attach portable ones, detach vanished ones */

#define MAX_ATTACHED 32

struct vol_job {
    wps_volumes vs;
    struct simple *reply;     /* a "sync" request waiting, or NULL */
    char report[1024];
};

/* ---- subscriptions and events (design: Service API, Events) */

struct sub {
    struct sub *next;
    int64_t id;
    struct client *c;
    char **targets;        /* object IDs, or paths */
    size_t ntargets;
    char **folders;
    size_t nfolders;
    bool jobs;
};

struct shadow_ids { char ids[32][WPS_ID_LEN + 1]; int n; };

struct field { char key[64], type[16], label[128], value[1024], setter[64], choices[256]; };

/* ---- list: a page of a folder's entries */

struct list_req { struct client *c; int64_t id; char folder[PATH_MAX]; };

/* ---- from_template: a new object copied from a template */

struct tmpl_job {
    struct client *c;
    int64_t id;
    char src[PATH_MAX], folder[PATH_MAX], title[256], path[PATH_MAX + 300];
    int err;
    wps_loaded ld;
};


/* ---- icon positions: layout, place, arrange (G2: free placement) */

/* {op:"layout", target:FOLDER} -> {positions:{ID:"x,y"}, items:[{name, id, x, y}]}
 * Positions are kept by object ID; the GUI knows items by name, so a
 * worker reads the folder and matches each child's ID (one attribute read
 * per child, and only when the folder has positions at all). */
struct layout_job {
    struct client *c;
    int64_t id;
    char folder[PATH_MAX];
    size_t n;
    char (*ids)[WPS_ID_LEN + 1];
    char **vals;
    char **names;        /* found by the worker, per position */
};

/* {op:"place", folder, items:[{target, x, y}]}: set icon positions. Items
 * get object IDs if they have none (positions are kept by ID). Replies with
 * the IDs, in order. */
struct place_job {
    struct client *c;
    int64_t id;
    char folder[PATH_MAX];
    size_t n;
    char **targets;
    int64_t *x, *y;
    int8_t *lock;        /* per item: -1 as it was, 0 unlock, 1 lock */
    wps_loaded fld, *items;
};


/* ---- vmove: move or copy a central object to another folder */

struct vmove_job {
    struct client *c;
    int64_t id;
    char target[64], dest[PATH_MAX], from_folder[PATH_MAX];
    bool copy, del;         /* del: vdelete, the object is deleted */
    bool trash;             /* vtrash: into the Trash Can, its folder noted (restore) */
    char new_id[WPS_ID_LEN + 1];
    int err;
};

/* ---- rename: the file or folder itself (decided 2026-11-18, as in OS/2) */

struct rename_job {
    struct client *c;
    int64_t id;
    char path[PATH_MAX], newpath[PATH_MAX + NAME_MAX + 2], name[NAME_MAX + 1];
    int err;
    bool as_title;          /* no permission to rename: keep it as a title */
    wps_loaded ld;
};

/* ---- create: a new object of a class in a folder, never overwriting */

struct create_req {
    struct client *c;
    int64_t id;
    char folder[PATH_MAX], cls[64], title[256], command[1024];
    char path[PATH_MAX + 64];
    int err;
    bool central;
    wps_loaded ld;
};

/* A drive's action through udisks2; for one that unmounts, the volume is
 * let go of first (wpsd's database on it closed), and taken up again if
 * the unmount fails. */
struct drive_call { char uuid[80], action[16], mount[PATH_MAX], *pass; wps_udisks_fn done; void *arg; };

/* ---- the Trash Can: putting an item back (v0.2-design 4) */
struct restore_job { struct client *c; int64_t id; char path[PATH_MAX], out[PATH_MAX], from[PATH_MAX]; char new_id[WPS_ID_LEN + 1]; int err; };

/* ---- Empty trash: its central objects (trashed shadows) deleted, then a
 * delete job for its files */
struct empty_job { struct client *c; int64_t id; char files[PATH_MAX]; char (*gone)[WPS_ID_LEN + 1]; size_t ngone; };

/* ---- the desktop's first start (v0.2-design 2): what a new desktop gets,
 * in versioned steps, each done once for the user (<config>/desktop-made:
 * "version N"; 0.1 wrote it without a version: 1). The desktop asks each
 * time it opens ({op:"first_start", desktop}); a step the user undid (a
 * deleted shadow) is not redone. Steps:
 *   1 (0.1's): a shadow of the Templates folder, one of System Setup;
 *   2 (0.2's): (the System folder, Programs, Trash Can: v0.2 steps 2-5);
 *   7: Templates in System, as on OS/2 (the desktop's shadows of it move
 *      there); the desktop puts the Trash Can in its lower right corner;
 *   8: Restore Desktop in System;
 *   9 (0.3): the home folder in Computer (a shadow);
 *  10 (0.3): titles (the owner's): System "My System - <host name>",
 *      Computer "My Storages" (unless titled already).
 * After it, the default objects in place are noted (restore_defaults). */
#define FIRST_START_VERSION 11

struct first_start {
    struct client *c;
    int64_t id;
    char desk[PATH_MAX];
    int from;
    char gone[8][WPS_ID_LEN + 1];   /* the desktop's shadows of System Setup, removed */
    int ngone;
    char viewer[WPS_ID_LEN + 1];    /* the Minimized Window Viewer, made */
    char restorer[WPS_ID_LEN + 1];  /* Restore Desktop, made (step 8) */
    char sys[PATH_MAX], autostart[PATH_MAX];
    wps_loaded asld;                /* the autostart folder (titled Startup) */
    wps_loaded sysld, compld;       /* step 10: System and Computer, titled */
    char trash[PATH_MAX];           /* the Trash Can: the home trash's files folder */
    char programs[PATH_MAX];        /* Programs (step 6) */
    char tpl_moved[8][WPS_ID_LEN + 1];   /* step 7: the desktop's Templates shadows */
    char tpl_new[8][WPS_ID_LEN + 1];     /* each one's ID in System ("": deleted, one was there) */
    int ntpl;
    bool tpl_in_system;             /* System has one already */
    bool scan_sys;
    wps_loaded tld;
};

/* The first start's shadows, made one after another: the desktop places
 * new icons in the order they appear, so a new desktop is always laid out
 * the same (concurrently, My System and Programs swapped places at random). */
struct fs_shadow { char target[PATH_MAX], folder[PATH_MAX]; struct fs_shadow *next; };
#define NSETUP 9
#define SETUP_IN_SYSTEM 6   /* setup_classes from this one on */

/* ---- the default objects (0.2): System, Programs and the Trash Can on the
 * desktop; Templates, Startup, the Minimized Window Viewer and Restore
 * Desktop in System; the settings objects where they belong. Each is
 * noted by ID once it is in place (locator "wps-defaults", class WPS, one
 * prop per object), so {op:"restore_defaults", desktop} can put back the
 * very object the user moved or deleted (a central object moved gets a
 * new ID: noted again), or make one if it is gone for good; missing
 * default folders are made again. Each desktop open notes the ones in
 * place ({op:"first_start"}). */
#define DEFAULTS_LOC "wps-defaults"
#define NDEFAULTS 8

struct defobj {
    const char *name, *title;
    const char *cls;            /* a central object of this class; NULL: a shadow of target */
    char target[PATH_MAX], folder[PATH_MAX];
};

struct defaults_job {
    struct client *c;
    int64_t id;
    bool restore;
    wps_loaded sysld, compld;   /* their titles, if made anew */
    char desk[PATH_MAX];
    struct { char folder[PATH_MAX], id[WPS_ID_LEN + 1], old_folder[PATH_MAX], old_id[WPS_ID_LEN + 1]; } ev[24];
    int nev;
    const char *put_back[32];   /* titles, for the reply */
    int nput;
    bool made_programs, made_trash, made_computer;
    bool no_desktop;            /* none named, none known */
};

struct def_match { const char *tid, *cls; char found[WPS_ID_LEN + 1]; };

/* {op:"setup"} -> {folder, objects:{<class>:{address, settings:{key:value}}}} */
struct setup_req {
    struct client *c;
    int64_t id;
    char folder[PATH_MAX];
    wps_loaded ld[NSETUP];
    wps_loaded sch[32];      /* the schemes */
    size_t nsch;
};

struct shadow_reply { struct simple *s; char link[PATH_MAX]; };

/* ---- Startup (startup.h) */

struct startup_job {
    struct client *c;
    int64_t id;
    char target[PATH_MAX], made[PATH_MAX], dir[PATH_MAX];
    int err;
};

struct op_def { const char *name; bool (*fn)(struct client *c, int64_t id, const char *op, cv *msg); };

#define DRIVES_PARKED "00000000-0000-4000-8000-0000000d15c0"

struct drive_obj { char id[WPS_ID_LEN + 1], uuid[80], title[256], autotitle[256]; int where; bool seen; };
struct drive_event { char folder[PATH_MAX]; char id[WPS_ID_LEN + 1]; char kind; };

struct drives_sync {
    wps_drive_info *ds;
    size_t n;
    char comp[PATH_MAX], drives[PATH_MAX + 8];
    char vol[WPS_VOLID_MAX], cid[WPS_ID_LEN + 1], did[WPS_ID_LEN + 1];
    struct drive_obj *objs;
    size_t nobjs;
    int collecting;   /* where the objects being listed are: 0 Drives, 1 Computer, 2 parked */
    struct drive_event *ev;
    size_t nev;
    bool changed;
};

/* ---- variables */

extern wps_env env;
extern wps_db *db;
extern wpo_loop *loop;
extern bool quitting;
extern struct client *clients;
extern struct sub *subs;
extern int64_t next_sub;
extern char known_desktop[PATH_MAX];

/* ---- functions */

void do_props(struct req *r, wpo_object *o);   /* wpsd_props.c */
void props_init(void);
/* wpsd_backing.c: properties Linux keeps; a type's programs (mimeapps). */
void backing_init(void);
char *mimeapps_options(const char *mime);
char *mimeapps_get(const char *mime);
bool mimeapps_set(const char *mime, const char *name);
/* A setup string ("key=value;...") applied to o, in order; false (and why
 * in err) at the first refused. */
bool apply_setup(wpo_object *o, const char *setup, char *err, size_t en);
/* wpsd_actions.c: an event's actions on o (object-properties-design.md
 * 2.4) run; its sound returned: NULL if none is set (the Sound object's),
 * "" for none, else the sound. Caller frees. */
char *fire_event(wpo_object *o, const char *path, const char *event, const char *display, const char *x11,
                 const char *const *envs, size_t nenvs);
/* ... in the session client c last opened something in (NULL: none). */
char *fire_event_for(struct client *c, wpo_object *o, const char *path, const char *event);
/* Prevent delete (its General page, no_delete): true if `target` (a path
 * or vobj:ID) has it on, with a message naming it in `why`. */
bool no_delete_of(const char *target, char *why, size_t n);
/* The same for every object of a selection (the first one found). */
bool no_delete_in(int64_t selection, char *why, size_t n);
void client_note_session(struct client *c, const char *display, const char *x11, const char *const *envs, size_t nenvs);
char *scheme_titles(void);   /* wpsd_setup.c */
void props_broadcast(const char *scope);   /* wpsd_props.c: defaults_changed */
bool is_schemes_path(const char *folder);
bool is_schemes_folder(const char *folder_id);
bool do_scope(struct client *c, int64_t id, const char *op, cv *msg, const char *target);

void log_msg(const char *fmt, ...);
void client_unref(struct client *c);
void client_close(struct client *c);
void client_flush(struct client *c);
void send_frame(struct client *c, cbuf *b);
void reply_error(struct client *c, int64_t id, const char *error, const char *message);
void put_object(cbuf *b, wpo_object *o);
void prepare_work(void *arg)    /* worker */;
void reply_ok_object(struct req *r, wpo_object *o, const wpo_value *result);
void reply_ok_object_sound(struct req *r, wpo_object *o, const wpo_value *result, const char *sound);
void prepare_done(void *arg)    /* loop */;
void do_describe(struct client *c, int64_t id, const char *name);
void reply_simple_ok(struct simple *s, const char *result);
void flushed(void *arg);
void stats_work(void *arg);
void stats_done(void *arg);
void volume_release(const char *mnt);
void volume_unrelease_all(void);
void start_volume_job(struct simple *reply);
void on_mounts(int fd, short rev, void *arg);
void scanned(void *arg, const char *report);
void subs_drop_client(struct client *c);
void event_head(cbuf *b, const char *name, int64_t sub, size_t extra);
void messages_broadcast(int64_t added);
void say_broadcast(const char *text, bool stay);   /* wpsctl say: a message on the desktop */
void terminal_from_system(wpo_object *o);
void on_object_changed(wpo_object *o);
void on_layout_changed(const char *folder);
void on_central_change(const char *folder, const char *id, char kind);
void on_fs_event(char kind, const char *from, const char *path);
void on_in_use(const char *path, bool in_use);
void on_job(int64_t jid);
void on_volumes_changed(const char *report);
void strlist(const cv *a, char ***out, size_t *n);
void put_menu(cbuf *b, const char *text, const char *parent);
void put_settings(cbuf *b, wpo_object *o, const char *text, const char *icon_shown);
typedef void (*origin_fn)(void *ctx, const char *key, bool *own, char *from, size_t fn);
void put_settings_with(cbuf *b, const char *text, const char *icon_shown, origin_fn of, void *ctx);
char *call_text(wpo_object *o, const char *method, const char *arg);
char *settings_text(wpo_object *o);
const char *apply_settings(wpo_object *o, const cv *values, const char *pages_text);
char *settings_text_with(wpo_object *o, char *t);
bool settings_setter_of(wpo_object *o, const char *pages_text, const char *key, char *out, size_t n);
void reply_value_cbor(struct client *c, int64_t id, cbuf *val);
void reply_int(struct client *c, int64_t id, int64_t v);
bool handle_jobs(struct client *c, int64_t id, const char *op, cv *msg);
void list_reply(void *arg, const wps_page *pg);
void do_open(struct req *r, wpo_object *o);
void tmpl_work(void *arg);
void tmpl_done(void *arg);
void do_layout(struct req *r, wpo_object *o);
void do_arrange(struct req *r, wpo_object *o);
/* wpsd_workarea.c: Work Area folders (0.4 step 9) */
bool work_area_of(const char *path, char *out, size_t n);   /* the nearest work area holding path */
void work_area_opened(const char *group, const char *opened, const char *tracked, pid_t pid);
void work_area_in_use(const char *path, bool in_use);
void work_area_save_all(void);   /* at the end: each group's open objects kept on its folder */
void do_work_area(struct req *r, wpo_object *o);
void job_placed(const char *dest, char **paths, int64_t *x, int64_t *y, size_t n);
void start_place(struct client *c, int64_t id, const cv *msg);
void vmove_work(void *arg);
void vmove_done(void *arg);
bool is_desktop(const char *path);
bool wps_folder(const char *path);
void do_rename(struct req *r, wpo_object *o);
void do_from_template(struct req *r, wpo_object *o);
void start_create(struct client *c, int64_t id, const char *folder, const char *cls, const char *title, const char *command);
void builtin_templates_work(void *arg);
void put_drive(cbuf *b, const wps_drive_info *d);
void drive_call(const char *uuid, const char *action, const char *pass, wps_udisks_fn done, void *arg);
void drive_done(void *arg, int err, const char *msg, const char *mount);
void drive_target_work(void *arg);
void drive_target_done(void *arg);
bool drive_query(const char *uuid, wps_drive_state *st, bool details);
void restore_work(void *arg);
void restore_done(void *arg);
void empty_work(void *arg);
void empty_done(void *arg);
void first_start_work(void *arg);
void first_start_done(void *arg);
void data_dir(const char *name, char *out, size_t n);
void system_dir(char *out, size_t n);
void setup_dir(char *out, size_t n);
void system_folder_init(void);
void builtin_templates_done(void *arg);
void autostart_dir(char *out, size_t n);
void defaults_start(struct client *c, int64_t id, const char *desk, bool restore);
void setup_req_work(void *arg);
void setup_req_done(void *arg);
void shadow_made(void *arg, const char *link, const char *error);
void quit_after_flush(void *arg);
void shutdown_flushed(void *arg);
void found(void *arg, int64_t find, const wps_found *items, size_t n, bool done, uint64_t scanned, bool truncated, int err);
void startup_work(void *arg);
void startup_done(void *arg);
bool startup_query(wpo_object *o, bool *on);
bool startup_set(wpo_object *o, bool on);
void client_io(int fd, short rev, void *arg);
void on_accept(int fd, short rev, void *arg);
void on_signal(int fd, short rev, void *arg);
void on_watchdog(const char *label, double ms, void *user);
int main(int argc, char **argv);
void computer_dirs(char *comp, size_t cn, char *drives, size_t dn);
void drive_event(struct drives_sync *j, int where, const char *id, char kind);
void drives_changed(void);

#endif
