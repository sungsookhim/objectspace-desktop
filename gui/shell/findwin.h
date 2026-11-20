/*
 * Find (G4 search): OS/2's Find window, searching as you type.
 *
 * Name (a part of the name, or a pattern with * and ?), the folder to
 * look in, whether to include its subfolders, and the type of object
 * (any, folders, files, programs), as OS/2's Find asked; the results below,
 * in a details view (title, the folder it is in, size, date), filled in as
 * wpsd finds them. Each change of the name starts the search again (a
 * modern search field); Enter or Find starts it with the other fields. A
 * double click opens a result; its menu also opens the folder it is in.
 *
 * Opened from the LaunchPad's Find button (the home folder) and with
 * Ctrl+F in a folder window (that folder).
 */
#ifndef WPS_FINDWIN_H
#define WPS_FINDWIN_H

#include <stdbool.h>
#include <stddef.h>

#include "cbor.h"
#include "ui.h"

/* open_folder: the shell opens a folder window for a path. */
void findwin_init(ui_app *a, void (*open_folder)(const char *path));

/* A new Find window on `folder` (name: what to look for, or NULL). */
void findwin_open(const char *folder, const char *name);

/* A "found" event from wpsd: true if it was a search's. */
bool findwin_event(const cv *ev);

/* Self-tests (the most recent window). */
size_t findwin_count(void);
const char *findwin_result(size_t i);   /* its path */
bool findwin_searching(void);
bool findwin_type(const char *text);    /* as typed into the name field (replaces it) */
bool findwin_kind(int kind);              /* 0 any, 1 folders, 2 files, 3 programs */
bool findwin_snapshot(const char *png);

#endif
