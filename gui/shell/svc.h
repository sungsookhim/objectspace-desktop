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
void svc_rename(const char *target, const char *name, svc_fn fn, void *arg);
void svc_settings(const char *target, svc_fn fn, void *arg);
void svc_shadow(const char *target, const char *folder, svc_fn fn, void *arg);
void svc_select(const char *folder, const char *const *names, size_t n, svc_fn fn, void *arg);
/* "move", "copy"; with names/x/y (n > 0), where the items should land. */
void svc_job_start(const char *kind, int64_t selection, const char *dest, const char *const *names, const int64_t *x,
                   const int64_t *y, size_t n, svc_fn fn, void *arg);
/* Move or copy a central object (vobj:ID) into a folder; the reply's
 * result is its address there. */
void svc_vmove(const char *target, const char *folder, bool copy, svc_fn fn, void *arg);
void svc_job_decide(int64_t job, const char *decision, bool all);                                 /* "skip", "rename" */
void svc_job_cancel(int64_t job);
void svc_subscribe_jobs(svc_fn fn, void *arg);
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
void svc_unsubscribe(int64_t sub);

#endif
