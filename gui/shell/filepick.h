/*
 * The file picker (v0.3-design 1: a notebook's file field's Browse...): a
 * folder's contents in an icon view, folders first, then the files of the
 * kind asked for ("image": pictures, with previews; "audio": sounds; "":
 * any); Up, and a path field to type or paste into. A double click on a
 * folder (or Enter) goes into it; on a file it picks it: chosen(owner, key,
 * the file's path), and the picker closes. Escape closes it unchosen.
 */
#ifndef WPS_FILEPICK_H
#define WPS_FILEPICK_H

#include <stdbool.h>
#include <stddef.h>

#include "ui.h"

typedef void (*filepick_fn)(long owner, const char *key, const char *path);

void filepick_init(ui_app *a);
/* `for_title`: what it is for (in its title); `kind`: "image", "audio" or "". */
void filepick_open(const char *for_title, const char *kind, const char *start, filepick_fn chosen, long owner,
                   const char *key);

/* Self-tests (the open picker). */
bool filepick_is_open(void);
const char *filepick_folder(void);
size_t filepick_count(void);
const char *filepick_name(size_t i);
bool filepick_go(const char *folder);
bool filepick_activate(size_t i);          /* as a double click */
bool filepick_snapshot(const char *png);

#endif
