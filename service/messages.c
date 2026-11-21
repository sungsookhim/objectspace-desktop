/* Messages kept by wpsd (messages.h). */
#define _GNU_SOURCE
#include "messages.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct msg {
    int64_t id, time, nid, urgency;
    char *app, *icon, *summary, *body, *sender;
    char **actions;
    size_t nact;
    bool read, closed;
};

static struct msg *msgs;       /* oldest first */
static size_t nmsgs, capmsgs;
static int64_t next_id = 1;
static char file[PATH_MAX];

static char *dup_or_empty(const char *s) { return strdup(s ? s : ""); }

static void msg_free(struct msg *m)
{
    free(m->app); free(m->icon); free(m->summary); free(m->body); free(m->sender);
    for (size_t i = 0; i < m->nact; i++) free(m->actions[i]);
    free(m->actions);
}

/* Fields of a map (a request, or a stored message) into m. */
static void from_map(struct msg *m, const cv *v)
{
    m->app = dup_or_empty(cv_text(v, "app"));
    m->icon = dup_or_empty(cv_text(v, "icon"));
    m->summary = dup_or_empty(cv_text(v, "summary"));
    m->body = dup_or_empty(cv_text(v, "body"));
    m->sender = dup_or_empty(cv_text(v, "sender"));
    int64_t x;
    m->urgency = cv_int(v, "urgency", &x) && x >= 0 && x <= 2 ? x : 1;
    m->nid = cv_int(v, "nid", &x) ? x : 0;
    const cv *b = cv_get(v, "read");
    m->read = b && b->k == CV_BOOL && b->b;
    b = cv_get(v, "closed");
    m->closed = b && b->k == CV_BOOL && b->b;
    const cv *a = cv_get(v, "actions");
    if (a && a->k == CV_ARRAY && a->n) {
        m->actions = calloc(a->n, sizeof *m->actions);
        for (size_t i = 0; i < a->n; i++)
            if (a->items[i].k == CV_TEXT) m->actions[m->nact++] = strdup(a->items[i].s);
        m->nact &= ~(size_t)1;   /* key, label pairs */
    }
}

static void put_msg(cbuf *b, const struct msg *m)
{
    cb_map(b, 13);
    cb_text(b, "id"); cb_int(b, m->id);
    cb_text(b, "time"); cb_int(b, m->time);
    cb_text(b, "app"); cb_text(b, m->app);
    cb_text(b, "icon"); cb_text(b, m->icon);
    cb_text(b, "summary"); cb_text(b, m->summary);
    cb_text(b, "body"); cb_text(b, m->body);
    cb_text(b, "urgency"); cb_int(b, m->urgency);
    cb_text(b, "read"); cb_bool(b, m->read);
    cb_text(b, "closed"); cb_bool(b, m->closed);
    cb_text(b, "nid"); cb_int(b, m->nid);
    cb_text(b, "sender"); cb_text(b, m->sender);
    cb_text(b, "actions"); cb_array(b, m->nact);
    for (size_t i = 0; i < m->nact; i++) cb_text(b, m->actions[i]);
    cb_text(b, "v"); cb_int(b, 1);
}

static void save(void)
{
    if (!file[0]) return;
    cbuf b;
    cb_init(&b);
    cb_array(&b, nmsgs);
    for (size_t i = 0; i < nmsgs; i++) put_msg(&b, &msgs[i]);
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.new", file);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    bool ok = fd >= 0 && !b.oom;
    for (size_t off = 0; ok && off < b.n;) {
        ssize_t w = write(fd, b.d + off, b.n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = false;
        else off += (size_t)w;
    }
    if (fd >= 0 && close(fd) != 0) ok = false;
    if (ok && rename(tmp, file) == 0) { cb_free(&b); return; }
    fprintf(stderr, "wpsd: messages: cannot write %s: %s\n", file, strerror(errno));
    unlink(tmp);
    cb_free(&b);
}

static void load(void)
{
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 64 << 20) { close(fd); return; }
    char *buf = malloc((size_t)st.st_size);
    ssize_t n = buf ? read(fd, buf, (size_t)st.st_size) : -1;
    close(fd);
    cv v;
    if (n == st.st_size && cv_decode(buf, (size_t)n, &v) == 0) {
        if (v.k == CV_ARRAY) {
            for (size_t i = 0; i < v.n; i++) {
                const cv *o = &v.items[i];
                if (o->k != CV_MAP) continue;
                if (nmsgs == capmsgs) { capmsgs = capmsgs ? capmsgs * 2 : 64; msgs = realloc(msgs, capmsgs * sizeof *msgs); }
                struct msg *m = &msgs[nmsgs++];
                memset(m, 0, sizeof *m);
                from_map(m, o);
                int64_t x;
                m->id = cv_int(o, "id", &x) ? x : next_id;
                m->time = cv_int(o, "time", &x) ? x : 0;
                if (m->id >= next_id) next_id = m->id + 1;
            }
        }
        cv_free(&v);
    } else {
        fprintf(stderr, "wpsd: messages: %s could not be read; starting a new list\n", file);
    }
    free(buf);
}

void wps_messages_init(const char *path)
{
    snprintf(file, sizeof file, "%s", path);
    load();
}

void wps_messages_shutdown(void)
{
    for (size_t i = 0; i < nmsgs; i++) msg_free(&msgs[i]);
    free(msgs);
    msgs = NULL;
    nmsgs = capmsgs = 0;
}

int64_t wps_messages_add(const cv *req)
{
    if (nmsgs >= WPS_MESSAGES_MAX) {   /* the oldest goes */
        size_t drop = nmsgs - WPS_MESSAGES_MAX + 1;
        for (size_t i = 0; i < drop; i++) msg_free(&msgs[i]);
        memmove(msgs, msgs + drop, (nmsgs - drop) * sizeof *msgs);
        nmsgs -= drop;
    }
    if (nmsgs == capmsgs) { capmsgs = capmsgs ? capmsgs * 2 : 64; msgs = realloc(msgs, capmsgs * sizeof *msgs); }
    struct msg *m = &msgs[nmsgs++];
    memset(m, 0, sizeof *m);
    from_map(m, req);
    m->read = false;
    m->closed = false;
    m->id = next_id++;
    m->time = (int64_t)time(NULL);
    save();
    return m->id;
}

void wps_messages_encode(cbuf *b)
{
    cb_array(b, nmsgs);
    for (size_t i = nmsgs; i-- > 0;) put_msg(b, &msgs[i]);
}

size_t wps_messages_count(void) { return nmsgs; }

size_t wps_messages_unread(void)
{
    size_t n = 0;
    for (size_t i = 0; i < nmsgs; i++) n += !msgs[i].read;
    return n;
}

static bool listed(const cv *ids, int64_t id)
{
    if (!ids || ids->k != CV_ARRAY) return false;
    for (size_t i = 0; i < ids->n; i++) if (ids->items[i].k == CV_INT && ids->items[i].i == id) return true;
    return false;
}

size_t wps_messages_set(const cv *ids, bool all, int read, int closed)
{
    size_t n = 0;
    for (size_t i = 0; i < nmsgs; i++) {
        struct msg *m = &msgs[i];
        if (!all && !listed(ids, m->id)) continue;
        bool ch = false;
        if (read >= 0 && m->read != (read != 0)) { m->read = read != 0; ch = true; }
        if (closed >= 0 && m->closed != (closed != 0)) { m->closed = closed != 0; ch = true; }
        n += ch;
    }
    if (n) save();
    return n;
}

size_t wps_messages_delete(const cv *ids, bool all)
{
    size_t k = 0, n = 0;
    for (size_t i = 0; i < nmsgs; i++) {
        if (all || listed(ids, msgs[i].id)) { msg_free(&msgs[i]); n++; }
        else msgs[k++] = msgs[i];
    }
    nmsgs = k;
    if (n) save();
    return n;
}

size_t wps_messages_closed(int64_t nid, const char *sender)
{
    size_t n = 0;
    for (size_t i = 0; i < nmsgs; i++) {
        struct msg *m = &msgs[i];
        if (m->nid == nid && !m->closed && !strcmp(m->sender, sender ? sender : "")) { m->closed = true; n++; }
    }
    if (n) save();
    return n;
}
