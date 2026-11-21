/*
 * Messages (v0.2-design 9): what programs and WPS itself told the user,
 * kept by wpsd so every GUI sees the same list, in
 * $XDG_STATE_HOME/wps/messages (a CBOR array; the last 500, newest
 * last). Loop thread only.
 *
 * A message: id (wpsd's), time (Unix seconds), app, icon, summary, body,
 * urgency (0 low, 1 normal, 2 critical), read, the notification's ID and
 * its sender on the session bus (nid, sender: for its actions), actions
 * ([key, label, ...]) and closed (its program closed it: the actions are
 * gone).
 */
#ifndef WPS_MESSAGES_H
#define WPS_MESSAGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor.h"

#define WPS_MESSAGES_MAX 500

void wps_messages_init(const char *path);
void wps_messages_shutdown(void);

/* From a request map's fields; its new ID. */
int64_t wps_messages_add(const cv *req);
/* The list, newest first, as an array. */
void wps_messages_encode(cbuf *b);
size_t wps_messages_count(void);
size_t wps_messages_unread(void);
/* ids: an array of IDs (or NULL with all). read/closed: -1 unchanged.
 * Returns how many changed. */
size_t wps_messages_set(const cv *ids, bool all, int read, int closed);
size_t wps_messages_delete(const cv *ids, bool all);
/* The program's notification nid (from sender) was closed: its actions go. */
size_t wps_messages_closed(int64_t nid, const char *sender);

#endif
