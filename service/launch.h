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
#ifndef OSD_LAUNCH_H
#define OSD_LAUNCH_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "osdo_loop.h"

/* `changed` runs on the loop thread when a path starts or stops being in
 * use by programs WPS started. */
void osd_launch_init(osdo_loop *loop, void (*changed)(const char *path, bool in_use));
void osd_launch_shutdown(void);

/* Loop thread: start argv (searched on PATH) for the object at `path`
 * (the file opened, or the program object), in folder `cwd` if given, on
 * Wayland display `display` (a socket path) if given, and track it. The pid, or -1 with errno set. */
pid_t osd_launch(char **argv, const char *path, const char *cwd, const char *display, const char *x11,
                 const char *const *envs, size_t nenvs);   /* envs: NAME=value, in place of ours */

/* Whether `path` is a program to run: executable by this user, and a
 * binary (ELF) or a script (#!). */
bool osd_is_program_file(const char *path);

/* Loop thread: the program started as `pid` (and what it starts) belongs
 * to a work area's group (0.4 step 9: the folder's path; NULL none). */
void osd_launch_set_group(pid_t pid, const char *group);
/* Loop thread: each live process of a group (adopted ones too), with its
 * scope ("" none) and the path it stands for. */
void osd_launch_group_each(const char *group, void (*fn)(pid_t pid, const char *unit, const char *path, void *arg), void *arg);

/* Loop thread: whether programs WPS started have `path` open. */
bool osd_launch_in_use(const char *path);

/* Loop thread: something at `from` is now at `to` (renamed or moved). */
void osd_launch_moved(const char *from, const char *to);

/* Split a .desktop Exec line into arguments (double quotes, backslash
 * escapes). File field codes (%f %F %u %U) become `file`, or are dropped
 * when it is NULL; other field codes are dropped; %% is kept as %. A file
 * given to a line without file codes is appended. The argument count. */
int osd_split_exec(const char *exec, const char *file, char *buf, size_t n, char **argv, int max);
/* The same with several files (objects dropped on a program). */
int osd_split_exec_files(const char *exec, const char *const *files, size_t nfiles, char *buf, size_t n, char **argv, int max);

/* Arguments that open `file` with the default application for its type,
 * read from mimeapps.list and the installed .desktop files. 0 when there
 * is none to start directly (unknown type, no default, or one that runs in
 * a terminal when no terminal program is found): the caller uses
 * xdg-open. A terminal application comes wrapped in a terminal window. */
int osd_handler_argv(const char *file, char *buf, size_t n, char **argv, int max);

/* Open With (v0.3-design 7): the programs that open `file`'s type, the
 * default first (is_default), then the added and declared ones, then
 * those of its parent types; the type in mime_out. */
typedef struct { char id[256]; char path[4096]; char name[128]; char icon[256]; bool is_default, hidden; } osd_app;
int osd_apps_for_file(const char *file, char *mime_out, size_t mn, osd_app *out, int max);
/* ... for a type (all files of it). */
int osd_apps_for_mime(const char *mime, osd_app *out, int max);
/* Every installed application to offer (Other program...), by name. */
int osd_apps_all(osd_app *out, int max);
/* Arguments that open `file` with the application `id` ("x.desktop"). */
int osd_app_argv(const char *id, const char *file, char *buf, size_t n, char **argv, int max);
/* Always use `id` for `file`'s type: the user's mimeapps.list (GNOME's too). */
bool osd_app_set_default(const char *file, const char *id);
/* ... for every file of a type (mimeapps.list's default, shared with GNOME). */
bool osd_app_set_default_mime(const char *mime, const char *id);

/* A program's Association page (v0.3-design 8): the types its desktop
 * entry opens ("a;b;"), the user's copy first; set: written to the user's
 * copy (made from the entry; GNOME reads it too). */
bool osd_app_types(const char *desktop, char *out, size_t n);
bool osd_app_set_types(const char *desktop, const char *types);

/* The System object's Terminal: a name from the list ("automatic": the
 * first installed), and a command used instead when not empty. */
void osd_terminal_choose(const char *choice, const char *command);
/* The terminal used now (for tests and the notebook), NULL: none. */
const char *osd_terminal_now(void);
/* Close window on exit off: argv run so that the terminal stays open
 * until Enter after it ends. The new count. */
int osd_hold_wrap(char **argv, int argc, int max);

/* Run argv[0..argc) in a terminal window: WPS Terminal (the System's
 * default, 0.4.1: titled `title`, --hold when `hold`), else the user's
 * command ($OSD_TERMINAL: words the program is appended to), else the
 * System's choice or xdg-terminal-exec, else the first installed common
 * terminal, each asked for a window of its own (`hold`: through sh, as
 * osd_hold_wrap). Shifts argv; the new count, or 0 when there is none. */
int osd_terminal_wrap(char **argv, int argc, int max, const char *title, bool hold);

/* Whether a program needs a terminal: a script, or a binary that links
 * no window library (as OS/2 told text-mode programs from PM ones). */
bool osd_program_needs_terminal(const char *path);

/* Whether a .desktop file says its program runs in a terminal. */
bool osd_desktop_wants_terminal(const char *desktop);

/* Any thread: whether a lock file next to `path` says it is open. */
bool osd_lock_file_present(const char *path);

/* Whether `name` is one of those lock files. */
bool osd_is_lock_file(const char *name);

#endif
