/*
 * Programs and files WPS starts, tracked while they run, for in-use
 * emphasis (design: decided 2026-11-18, In-use emphasis for files).
 *
 * A file is opened with its type's default application, started directly
 * (not through xdg-open, which hands over and exits at once), so the
 * started process stands for the open file. Each started process is
 * watched through a pidfd. The service is a child subreaper, so programs
 * that fork and let the parent exit are adopted and still counted.
 * Applications that hand the file to an instance already running exit at
 * once; for those, only lock files show the file as open.
 *
 * Lock files of common programs, next to the file, also mark it in use:
 * LibreOffice (.~lock.NAME#), vim (.NAME.swp), Emacs (.#NAME) and
 * Microsoft Office (~$NAME).
 */
#ifndef WPS_LAUNCH_H
#define WPS_LAUNCH_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "wpo_loop.h"

/* `changed` runs on the loop thread when a path starts or stops being in
 * use by programs WPS started. */
void wps_launch_init(wpo_loop *loop, void (*changed)(const char *path, bool in_use));
void wps_launch_shutdown(void);

/* Loop thread: start argv (searched on PATH) for the object at `path`
 * (the file opened, or the program object), in folder `cwd` if given, and
 * track it. The pid, or -1 with errno set. */
pid_t wps_launch(char **argv, const char *path, const char *cwd);

/* Whether `path` is a program to run: executable by this user, and a
 * binary (ELF) or a script (#!). */
bool wps_is_program_file(const char *path);

/* Loop thread: whether programs WPS started have `path` open. */
bool wps_launch_in_use(const char *path);

/* Loop thread: something at `from` is now at `to` (renamed or moved). */
void wps_launch_moved(const char *from, const char *to);

/* Split a .desktop Exec line into arguments (double quotes, backslash
 * escapes). File field codes (%f %F %u %U) become `file`, or are dropped
 * when it is NULL; other field codes are dropped; %% is kept as %. A file
 * given to a line without file codes is appended. The argument count. */
int wps_split_exec(const char *exec, const char *file, char *buf, size_t n, char **argv, int max);

/* Arguments that open `file` with the default application for its type,
 * read from mimeapps.list and the installed .desktop files. 0 when there
 * is none to start directly (unknown type, no default, or one that runs in
 * a terminal when no terminal program is found): the caller uses
 * xdg-open. A terminal application comes wrapped in a terminal window. */
int wps_handler_argv(const char *file, char *buf, size_t n, char **argv, int max);

/* Run argv[0..argc) in a terminal window: $WPS_TERMINAL (words the
 * command is appended to), else xdg-terminal-exec, else the first
 * installed common terminal, each asked for a window of its own. Shifts
 * argv; the new count, or 0 when there is no terminal. */
int wps_terminal_wrap(char **argv, int argc, int max);

/* Whether a program needs a terminal: a script, or a binary that links
 * no window library (as OS/2 told text-mode programs from PM ones). */
bool wps_program_needs_terminal(const char *path);

/* Whether a .desktop file says its program runs in a terminal. */
bool wps_desktop_wants_terminal(const char *desktop);

/* Any thread: whether a lock file next to `path` says it is open. */
bool wps_lock_file_present(const char *path);

/* Whether `name` is one of those lock files. */
bool wps_is_lock_file(const char *name);

#endif
