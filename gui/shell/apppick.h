/*
 * Other program... (Open With, v0.3-design 7): the installed programs in
 * an icon view, by name, and Always use for this type. A double click (or
 * Enter) on one chooses it: chosen(owner, target, its desktop ID, always),
 * and the picker closes. Escape closes it unchosen.
 */
#ifndef OSD_APPPICK_H
#define OSD_APPPICK_H

#include <stdbool.h>
#include <stddef.h>

#include "ui.h"

typedef void (*apppick_fn)(long owner, const char *target, const char *app, bool always);

void apppick_init(ui_app *a);
/* `title`: the file's; `target`: what is opened (handed back). */
void apppick_open(const char *title, const char *target, apppick_fn chosen, long owner);
/* Programs' icons became ready: drawn again. */
void apppick_icons_changed(void);

/* Self-tests. */
bool apppick_is_open(void);
size_t apppick_count(void);
const char *apppick_name(size_t i);
bool apppick_choose(size_t i, bool always);
bool apppick_snapshot(const char *png);

#endif
