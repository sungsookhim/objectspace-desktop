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

#endif
