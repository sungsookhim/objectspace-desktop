/*
 * wps-shell's connection to wpsd (gui-design.md: Threads). The UI thread
 * never waits: requests go to a writer thread; a reader thread decodes
 * replies and hands each to its callback on the UI thread. Requests are
 * pipelined (matched by ID), so a slow page never holds up a menu.
 */
#ifndef WPS_SHELL_SVC_H
#define WPS_SHELL_SVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor.h"
#include "ui.h"

/* reply: the whole reply map, or NULL if the service is unreachable. The
 * reply is freed after the callback returns. */
typedef void (*svc_fn)(void *arg, const cv *reply);

bool svc_start(ui_app *app, char *err, size_t errlen);
void svc_stop(void);

void svc_list(const char *folder, size_t offset, size_t count, bool details, svc_fn fn, void *arg);
void svc_resolve(const char *target, svc_fn fn, void *arg);
void svc_menu(const char *target, svc_fn fn, void *arg);
void svc_menu_select(const char *target, int64_t item, svc_fn fn, void *arg);
void svc_open(const char *target, bool confirm, svc_fn fn, void *arg);
/* A program opened with objects dropped on it (paths). */
void svc_open_files(const char *target, bool confirm, const char *const *files, size_t nfiles, svc_fn fn, void *arg);
void svc_rename(const char *target, const char *name, svc_fn fn, void *arg);
void svc_settings(const char *target, svc_fn fn, void *arg);
void svc_shadow(const char *target, const char *folder, svc_fn fn, void *arg);
/* A new object from a template, in folder (title NULL: the template's). */
void svc_from_template(const char *target, const char *folder, const char *title, svc_fn fn, void *arg);
/* A new object of class cls in folder. */
void svc_create(const char *folder, const char *cls, const char *title, svc_fn fn, void *arg);
void svc_select(const char *folder, const char *const *names, size_t n, svc_fn fn, void *arg);
/* "move", "copy"; with names/x/y (n > 0), where the items should land. */
void svc_job_start(const char *kind, int64_t selection, const char *dest, const char *const *names, const int64_t *x,
                   const int64_t *y, size_t n, svc_fn fn, void *arg);
/* Move or copy a central object (vobj:ID) into a folder; the reply's
 * result is its address there. */
/* A central object (a shadow, ...) into the Trash Can, restorable. */
void svc_vtrash(const char *target, svc_fn fn, void *arg);
void svc_vdelete(const char *target, svc_fn fn, void *arg);   /* a central object */
void svc_vmove(const char *target, const char *folder, bool copy, svc_fn fn, void *arg);
void svc_job_decide(int64_t job, const char *decision, bool all);                                 /* "skip", "rename" */
void svc_job_cancel(int64_t job);
void svc_subscribe_jobs(svc_fn fn, void *arg);
/* A search (G4): replies {find: ID}; the matches come as "found" events
 * with that ID as their sub (svc_on_event). kind: any, folder, file, program. */
void svc_find(const char *folder, const char *name, bool subfolders, const char *kind, int64_t limit, svc_fn fn, void *arg);
void svc_find_cancel(int64_t find);
void svc_layout(const char *folder, svc_fn fn, void *arg);
void svc_arrange(const char *folder, svc_fn fn, void *arg);
void svc_place(const char *folder, const char *const *targets, const int64_t *x, const int64_t *y, size_t n, svc_fn fn, void *arg);
/* One setting; `value` is CV_TEXT, CV_INT or CV_BOOL. expect: the object's
 * change counter as last seen (a change elsewhere since is a conflict). */
void svc_settings_set(const char *target, const char *key, const cv *value, int64_t expect, svc_fn fn, void *arg);

/* Events: {event, sub, ...}, delivered to `fn` on the UI thread. The reply
 * to svc_subscribe carries the subscription ID in "result". */
void svc_on_event(svc_fn fn, void *arg);
void svc_subscribe_folder(const char *folder, svc_fn fn, void *arg);
void svc_subscribe_objects(const char *const *targets, size_t n, svc_fn fn, void *arg);
/* The settings objects' values: {folder, objects:{<class>:{address, settings}}}. */
/* The desktop's first-start steps (wpsd: once each, versioned). */
void svc_first_start(const char *desktop, svc_fn fn, void *arg);
/* The Trash Can: an item back where it was; the home trash emptied (a job). */
/* A drive (its object or a shadow): mount, unmount, eject, poweroff, unlock
 * (pass: the passphrase), lock; the result is its mount point. */
void svc_drive(const char *target, const char *action, const char *pass, svc_fn fn, void *arg);
void svc_restore(const char *target, svc_fn fn, void *arg);
void svc_empty_trash(svc_fn fn, void *arg);
void svc_setup(svc_fn fn, void *arg);
void svc_unsubscribe(int64_t sub);

/* Messages (wpsd's list; a "messages_changed" event follows each change).
 * actions: key, label, key, label, ... */
typedef struct {
    const char *app, *icon, *summary, *body, *sender;
    int64_t urgency, nid;
    const char *const *actions;
    size_t nactions;
} svc_message;
/* Into Startup: an autostart entry for a program or script, else a shadow. */
void svc_startup_add(const char *target, svc_fn fn, void *arg);
/* The default objects back in place (Restore Desktop) -> {restored:[titles]}. */
void svc_restore_defaults(const char *desktop, svc_fn fn, void *arg);
void svc_msg_add(const svc_message *m, svc_fn fn, void *arg);
void svc_msg_list(svc_fn fn, void *arg);
/* ids: n of them, or all messages when n is 0; read, closed: -1 unchanged. */
void svc_msg_set(const int64_t *ids, size_t n, int read, int closed, svc_fn fn, void *arg);
void svc_msg_delete(const int64_t *ids, size_t n, svc_fn fn, void *arg);
void svc_msg_closed(int64_t nid, const char *sender, svc_fn fn, void *arg);

/* An open's reply naming a view (messages, minimized-windows, launchpad,
 * warpcenter): the desktop shows it. Every part that opens objects passes
 * such replies here; true if it was one. */
extern bool (*svc_view_fn)(const char *view);
bool svc_reply_view(const cv *reply);

#endif
