/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * Messages (v0.2-design 9): the list window of the OsdMessages object and
 * of TheBar's envelope. osdd keeps the messages (every GUI sees
 * the same list); this shows them newest first, with program, time,
 * summary and body, unread ones in bold. A message's actions are buttons
 * while its program still has the notification; Delete, Delete all and
 * Mark all read.
 */
#ifndef OSD_MESSAGES_H
#define OSD_MESSAGES_H

#include <stdbool.h>
#include <stddef.h>

#include "cbor.h"
#include "ui.h"

void messages_init(ui_app *a);
/* A "messages_changed" event (true: it was one). */
bool messages_event(const cv *ev);
/* The Messages object's settings (dnd, popup_seconds). */
void messages_setting(const char *key, const char *val);
size_t messages_unread(void);
void messages_open(void);

/* Objectspace's own message (folder windows' errors, job results): kept, and an
 * error pops up in the Objectspace session. urgency: 0 low, 1 normal, 2 an error. */
void messages_post(const char *summary, const char *body, int urgency);

/* Self-tests. */
bool messages_is_open(void);
size_t messages_count(void);
const char *messages_summary(size_t i);    /* newest first */
bool messages_is_read(size_t i);
bool messages_select(size_t i);
/* A button of the window by its label ("Delete", an action's label). */
bool messages_button(const char *label);
bool messages_snapshot(const char *png);

#endif
