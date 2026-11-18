#define _GNU_SOURCE
#include "db.h"
#include "env.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_VOLS 32

struct attached {
    char vol[WPS_VOLID_MAX];
    sqlite3 *h;
    bool writable;
};

struct wps_db {
    pthread_mutex_t mu;
    sqlite3 *h;
    char writer[128];
    uint64_t clock;
    struct attached vols[MAX_VOLS];
};

static const char *STATE_SCHEMA =
    "CREATE TABLE IF NOT EXISTS state ("
    " vol TEXT NOT NULL, key TEXT NOT NULL, type TEXT NOT NULL, value BLOB,"
    " rev INTEGER NOT NULL, writer TEXT NOT NULL, deleted INTEGER NOT NULL DEFAULT 0,"
    " pushed INTEGER NOT NULL DEFAULT 0, mtime INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    " synced_rev INTEGER NOT NULL DEFAULT 0, synced_writer TEXT NOT NULL DEFAULT '',"
    " PRIMARY KEY (vol, key));"
    "CREATE INDEX IF NOT EXISTS state_unpushed ON state(vol) WHERE pushed = 0;"
    "CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT);";

static const char *MAIN_SCHEMA =
    "CREATE TABLE IF NOT EXISTS conflicts ("
    " vol TEXT, key TEXT, type TEXT, value BLOB, rev INTEGER, writer TEXT,"
    " lost_to_rev INTEGER, lost_to_writer TEXT, at INTEGER DEFAULT (strftime('%s','now')));"
    "CREATE TABLE IF NOT EXISTS locations ("
    " id TEXT PRIMARY KEY, vol TEXT, path TEXT, seen INTEGER DEFAULT (strftime('%s','now')));"
    "CREATE TABLE IF NOT EXISTS shadows ("
    " link_vol TEXT NOT NULL, link_rel TEXT NOT NULL, target_id TEXT NOT NULL, target_vol TEXT, target_rel TEXT,"
    " PRIMARY KEY (link_vol, link_rel));"
    "CREATE INDEX IF NOT EXISTS shadows_target ON shadows(target_id);"
    "CREATE TABLE IF NOT EXISTS jobs ("
    " id INTEGER PRIMARY KEY, kind TEXT, src TEXT, sel TEXT, dest TEXT, state TEXT,"
    " items_done INTEGER, bytes_done INTEGER, updated INTEGER DEFAULT (strftime('%s','now')));";

static int exec(sqlite3 *h, const char *sql)
{
    char *msg = NULL;
    int rc = sqlite3_exec(h, sql, NULL, NULL, &msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "wps db: %s\n", msg ? msg : sqlite3_errstr(rc));
        sqlite3_free(msg);
        return -1;
    }
    return 0;
}

static uint64_t meta_clock(sqlite3 *h)
{
    sqlite3_stmt *s;
    uint64_t c = 0;
    if (sqlite3_prepare_v2(h, "SELECT max(rev) FROM state", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) c = (uint64_t)sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    if (sqlite3_prepare_v2(h, "SELECT v FROM meta WHERE k='clock'", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            uint64_t m = (uint64_t)sqlite3_column_int64(s, 0);
            if (m > c) c = m;
        }
        sqlite3_finalize(s);
    }
    return c;
}

static void save_clock(wps_db *db)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->h, "INSERT OR REPLACE INTO meta(k,v) VALUES('clock',?)", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)db->clock);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

static sqlite3 *open_handle(const char *path, bool create, bool wal, char *err, size_t errlen)
{
    sqlite3 *h = NULL;
    int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0) | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(path, &h, flags, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "%s: %s", path, h ? sqlite3_errmsg(h) : "cannot open");
        sqlite3_close(h);
        return NULL;
    }
    sqlite3_busy_timeout(h, 2000);
    exec(h, wal ? "PRAGMA journal_mode=WAL" : "PRAGMA journal_mode=DELETE");
    exec(h, "PRAGMA synchronous=NORMAL");
    if (exec(h, STATE_SCHEMA) < 0) {
        snprintf(err, errlen, "%s: cannot create the schema", path);
        sqlite3_close(h);
        return NULL;
    }
    return h;
}

wps_db *wps_db_open(const char *path, const char *writer, char *err, size_t errlen)
{
    wps_db *db = calloc(1, sizeof *db);
    if (!db) return NULL;
    pthread_mutex_init(&db->mu, NULL);
    snprintf(db->writer, sizeof db->writer, "%s", writer);
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);   /* private from the start */
    if (fd >= 0) close(fd);
    db->h = open_handle(path, true, true, err, errlen);
    if (!db->h || exec(db->h, MAIN_SCHEMA) < 0) {
        if (db->h) sqlite3_close(db->h);
        free(db);
        return NULL;
    }
    /* Added in M3; ignore the error when the column already exists. */
    sqlite3_exec(db->h, "ALTER TABLE locations ADD COLUMN deleted INTEGER NOT NULL DEFAULT 0", NULL, NULL, NULL);
    db->clock = meta_clock(db->h);
    return db;
}

void wps_db_close(wps_db *db)
{
    if (!db) return;
    for (int i = 0; i < MAX_VOLS; i++)
        if (db->vols[i].h) sqlite3_close(db->vols[i].h);
    sqlite3_close(db->h);
    pthread_mutex_destroy(&db->mu);
    free(db);
}

void wps_db_key(char *out, size_t n, const char *locator, const char *cls, const char *prop)
{
    if (prop) snprintf(out, n, "%s%c%s%c%s", locator, WPS_KEY_SEP, cls, WPS_KEY_SEP, prop);
    else snprintf(out, n, "%s%c%s%c", locator, WPS_KEY_SEP, cls, WPS_KEY_SEP);
}

static struct attached *find_vol(wps_db *db, const char *vol)
{
    for (int i = 0; i < MAX_VOLS; i++)
        if (db->vols[i].h && strcmp(db->vols[i].vol, vol) == 0) return &db->vols[i];
    return NULL;
}

/* (rev, writer) ordering: the later write wins; the writer breaks ties. */
static int newer(int64_t r1, const char *w1, int64_t r2, const char *w2)
{
    if (r1 != r2) return r1 > r2 ? 1 : -1;
    int c = strcmp(w1, w2);
    return c > 0 ? 1 : c < 0 ? -1 : 0;
}

/* ------------------------------------------------------------- push/pull */

struct row {
    char *key, *type, *writer;
    void *value;
    int vlen;
    int64_t rev;
    int deleted;
};

static void row_free(struct row *r)
{
    free(r->key); free(r->type); free(r->writer); free(r->value);
}

static void row_from(sqlite3_stmt *s, int base, struct row *r)
{
    r->key = strdup((const char *)sqlite3_column_text(s, base));
    r->type = strdup((const char *)sqlite3_column_text(s, base + 1));
    r->vlen = sqlite3_column_bytes(s, base + 2);
    r->value = malloc(r->vlen ? (size_t)r->vlen : 1);
    if (r->vlen) memcpy(r->value, sqlite3_column_blob(s, base + 2), (size_t)r->vlen);
    r->rev = sqlite3_column_int64(s, base + 3);
    r->writer = strdup((const char *)sqlite3_column_text(s, base + 4));
    r->deleted = sqlite3_column_int(s, base + 5);
}

/*
 * Write a row. `synced` marks a row that is now identical on both sides (a
 * pull or a push): its version becomes the merge base, as in git. A local
 * write (synced false) keeps the previous merge base, so a later sync can
 * tell whether the other side changed too.
 */
static void upsert(sqlite3 *h, const char *vol, const struct row *r, int pushed, bool synced)
{
    sqlite3_stmt *s;
    const char *sql = synced
        ? "INSERT INTO state(vol,key,type,value,rev,writer,deleted,pushed,synced_rev,synced_writer)"
          " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?5,?6) ON CONFLICT(vol,key) DO UPDATE SET"
          " type=?3,value=?4,rev=?5,writer=?6,deleted=?7,pushed=?8,synced_rev=?5,synced_writer=?6,"
          " mtime=strftime('%s','now')"
        : "INSERT INTO state(vol,key,type,value,rev,writer,deleted,pushed)"
          " VALUES(?1,?2,?3,?4,?5,?6,?7,?8) ON CONFLICT(vol,key) DO UPDATE SET"
          " type=?3,value=?4,rev=?5,writer=?6,deleted=?7,pushed=?8,mtime=strftime('%s','now')";
    if (sqlite3_prepare_v2(h, sql, -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, vol, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, r->key, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, r->type, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 4, r->value, r->vlen, SQLITE_STATIC);
    sqlite3_bind_int64(s, 5, r->rev);
    sqlite3_bind_text(s, 6, r->writer, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 7, r->deleted);
    sqlite3_bind_int(s, 8, pushed);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

static void record_conflict(wps_db *db, const char *vol, const struct row *loser, const struct row *winner)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->h, "INSERT INTO conflicts(vol,key,type,value,rev,writer,lost_to_rev,lost_to_writer)"
                                  " VALUES(?,?,?,?,?,?,?,?)", -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, vol, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, loser->key, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, loser->type, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 4, loser->value, loser->vlen, SQLITE_STATIC);
    sqlite3_bind_int64(s, 5, loser->rev);
    sqlite3_bind_text(s, 6, loser->writer, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 7, winner->rev);
    sqlite3_bind_text(s, 8, winner->writer, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

static bool same_value(const struct row *a, const struct row *b)
{
    return a->deleted == b->deleted && a->vlen == b->vlen && strcmp(a->type, b->type) == 0 &&
           (a->vlen == 0 || memcmp(a->value, b->value, (size_t)a->vlen) == 0);
}

/* Look up the main row for (vol,key), with its merge base. */
static bool main_row(wps_db *db, const char *vol, const char *key, struct row *r, int *pushed,
                     int64_t *base_rev, char *base_writer, size_t bwn)
{
    sqlite3_stmt *s;
    bool found = false;
    if (sqlite3_prepare_v2(db->h, "SELECT key,type,value,rev,writer,deleted,pushed,synced_rev,synced_writer"
                                  " FROM state WHERE vol=? AND key=?", -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, vol, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, key, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW) {
        row_from(s, 0, r);
        *pushed = sqlite3_column_int(s, 6);
        *base_rev = sqlite3_column_int64(s, 7);
        snprintf(base_writer, bwn, "%s", sqlite3_column_text(s, 8));
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

static void pull(wps_db *db, struct attached *a, wps_sync_stats *st)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(a->h, "SELECT key,type,value,rev,writer,deleted FROM state WHERE vol=?", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, a->vol, -1, SQLITE_STATIC);
    while (sqlite3_step(s) == SQLITE_ROW) {
        struct row v = { 0 }, m = { 0 };
        row_from(s, 0, &v);
        if ((uint64_t)v.rev > db->clock) db->clock = (uint64_t)v.rev;
        int pushed = 1;
        int64_t base_rev = 0;
        char base_writer[128] = "";
        bool have = main_row(db, a->vol, v.key, &m, &pushed, &base_rev, base_writer, sizeof base_writer);
        int cmp = have ? newer(v.rev, v.writer, m.rev, m.writer) : 1;
        /* Did each side change since the last sync (the merge base)? */
        bool ours_changed = have && !pushed;
        bool theirs_changed = !have || newer(v.rev, v.writer, base_rev, base_writer) != 0;
        if (cmp > 0) {
            if (ours_changed && theirs_changed && !same_value(&v, &m)) {
                record_conflict(db, a->vol, &m, &v);   /* ours loses */
                st->conflicts++;
            }
            upsert(db->h, a->vol, &v, 1, true);
            st->pulled++;
        } else if (cmp < 0 && ours_changed && theirs_changed && !same_value(&v, &m)) {
            record_conflict(db, a->vol, &v, &m);       /* theirs loses; ours is pushed below */
            st->conflicts++;
        } else if (cmp == 0 && ours_changed) {
            upsert(db->h, a->vol, &m, 1, true);         /* already identical on the volume */
        }
        row_free(&v);
        if (have) row_free(&m);
    }
    sqlite3_finalize(s);
}

/* Push unpushed rows of this volume; caller holds the lock and a write
 * transaction on the main database. */
static void push(wps_db *db, struct attached *a, wps_sync_stats *st)
{
    if (!a->writable) return;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->h, "SELECT key,type,value,rev,writer,deleted FROM state WHERE vol=? AND pushed=0",
                           -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, a->vol, -1, SQLITE_STATIC);
    exec(a->h, "BEGIN IMMEDIATE");
    int n = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        struct row m = { 0 };
        row_from(s, 0, &m);
        /* Only overwrite the volume's row if ours is newer. */
        sqlite3_stmt *q;
        bool write = true;
        if (sqlite3_prepare_v2(a->h, "SELECT rev,writer FROM state WHERE vol=? AND key=?", -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, a->vol, -1, SQLITE_STATIC);
            sqlite3_bind_text(q, 2, m.key, -1, SQLITE_STATIC);
            if (sqlite3_step(q) == SQLITE_ROW)
                write = newer(m.rev, m.writer, sqlite3_column_int64(q, 0), (const char *)sqlite3_column_text(q, 1)) > 0;
            sqlite3_finalize(q);
        }
        if (write) { upsert(a->h, a->vol, &m, 1, true); n++; }
        row_free(&m);
    }
    sqlite3_finalize(s);
    if (exec(a->h, "COMMIT") == 0) {
        sqlite3_stmt *u;
        if (sqlite3_prepare_v2(db->h, "UPDATE state SET pushed=1, synced_rev=rev, synced_writer=writer"
                                      " WHERE vol=? AND pushed=0", -1, &u, NULL) == SQLITE_OK) {
            sqlite3_bind_text(u, 1, a->vol, -1, SQLITE_STATIC);
            sqlite3_step(u);
            sqlite3_finalize(u);
        }
        if (st) st->pushed += n;
    } else {
        exec(a->h, "ROLLBACK");
    }
}

int wps_db_attach_volume(wps_db *db, const char *vol, const char *voldb_path, bool writable,
                         wps_sync_stats *st, char *err, size_t errlen)
{
    wps_sync_stats dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof *st);
    pthread_mutex_lock(&db->mu);
    struct attached *a = find_vol(db, vol);
    if (a) {   /* re-attach: writability may have changed (remount) */
        sqlite3_close(a->h);
        a->h = NULL;
    } else {
        for (int i = 0; i < MAX_VOLS && !a; i++)
            if (!db->vols[i].h) a = &db->vols[i];
    }
    if (!a) {
        pthread_mutex_unlock(&db->mu);
        snprintf(err, errlen, "too many volumes attached");
        return -1;
    }
    if (writable) {
        int fd = open(voldb_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd >= 0) close(fd);
    } else if (access(voldb_path, F_OK) != 0) {
        pthread_mutex_unlock(&db->mu);   /* nothing to pull, nowhere to push */
        return 0;
    }
    a->h = open_handle(voldb_path, writable, false, err, errlen);
    if (!a->h) { pthread_mutex_unlock(&db->mu); return -1; }
    if (!writable) sqlite3_exec(a->h, "PRAGMA query_only=1", NULL, NULL, NULL);
    snprintf(a->vol, sizeof a->vol, "%s", vol);
    a->writable = writable;

    exec(db->h, "BEGIN IMMEDIATE");
    pull(db, a, st);
    push(db, a, st);
    save_clock(db);
    exec(db->h, "COMMIT");
    pthread_mutex_unlock(&db->mu);
    return 0;
}

void wps_db_detach_volume(wps_db *db, const char *vol)
{
    pthread_mutex_lock(&db->mu);
    struct attached *a = find_vol(db, vol);
    if (a) {
        sqlite3_close(a->h);
        memset(a, 0, sizeof *a);
    }
    pthread_mutex_unlock(&db->mu);
}

bool wps_db_volume_attached(wps_db *db, const char *vol)
{
    pthread_mutex_lock(&db->mu);
    bool r = find_vol(db, vol) != NULL;
    pthread_mutex_unlock(&db->mu);
    return r;
}

/* ------------------------------------------------------------ properties */

static int write_row(wps_db *db, const char *vol, const char *key, const char *type,
                     const void *val, uint32_t len, int deleted)
{
    pthread_mutex_lock(&db->mu);
    exec(db->h, "BEGIN IMMEDIATE");
    struct row r = { .key = (char *)key, .type = (char *)type, .value = (void *)val, .vlen = (int)len,
                     .rev = (int64_t)++db->clock, .writer = db->writer, .deleted = deleted };
    upsert(db->h, vol, &r, 0, false);
    save_clock(db);
    struct attached *a = find_vol(db, vol);
    if (a) push(db, a, NULL);   /* write-through to a mounted portable volume */
    int rc = exec(db->h, "COMMIT");
    pthread_mutex_unlock(&db->mu);
    return rc;
}

int wps_db_put(wps_db *db, const char *vol, const char *key, char type, const void *val, uint32_t len)
{
    char t[2] = { type, 0 };
    return write_row(db, vol, key, t, val, len, 0);
}

int wps_db_delete(wps_db *db, const char *vol, const char *key)
{
    return write_row(db, vol, key, "-", "", 0, 1);
}

int wps_db_get_class(wps_db *db, const char *vol, const char *locator, const char *cls, wps_props *out)
{
    char prefix[512];
    wps_db_key(prefix, sizeof prefix, locator, cls, NULL);
    size_t pl = strlen(prefix);
    wps_props_init(out);
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    int n = -1;
    if (sqlite3_prepare_v2(db->h, "SELECT key,type,value FROM state WHERE vol=? AND deleted=0"
                                  " AND substr(key,1,?)=?", -1, &s, NULL) == SQLITE_OK) {
        n = 0;
        sqlite3_bind_text(s, 1, vol, -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 2, (int)pl);
        sqlite3_bind_text(s, 3, prefix, -1, SQLITE_STATIC);
        while (sqlite3_step(s) == SQLITE_ROW) {
            const char *key = (const char *)sqlite3_column_text(s, 0) + pl;
            const char *type = (const char *)sqlite3_column_text(s, 1);
            wps_props_set(out, key, type[0], sqlite3_column_blob(s, 2), (uint32_t)sqlite3_column_bytes(s, 2));
            n++;
        }
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    return n;
}

int wps_db_get_locator(wps_db *db, const char *vol, const char *locator, wps_db_prop_fn fn, void *user)
{
    char prefix[512];
    snprintf(prefix, sizeof prefix, "%s%c", locator, WPS_KEY_SEP);
    size_t pl = strlen(prefix);
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    int n = -1;
    if (sqlite3_prepare_v2(db->h, "SELECT key,type,value FROM state WHERE vol=? AND deleted=0"
                                  " AND substr(key,1,?)=?", -1, &s, NULL) == SQLITE_OK) {
        n = 0;
        sqlite3_bind_text(s, 1, vol, -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 2, (int)pl);
        sqlite3_bind_text(s, 3, prefix, -1, SQLITE_STATIC);
        while (sqlite3_step(s) == SQLITE_ROW) {
            char cls[128];
            const char *rest = (const char *)sqlite3_column_text(s, 0) + pl;
            const char *sep = strchr(rest, WPS_KEY_SEP);
            if (!sep || (size_t)(sep - rest) >= sizeof cls) continue;
            memcpy(cls, rest, (size_t)(sep - rest));
            cls[sep - rest] = 0;
            const char *type = (const char *)sqlite3_column_text(s, 1);
            fn(cls, sep + 1, type[0], sqlite3_column_blob(s, 2), (uint32_t)sqlite3_column_bytes(s, 2), user);
            n++;
        }
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    return n;
}

/* --------------------------------------------------------------- misc */

int64_t wps_db_count(wps_db *db, const char *table, const char *vol)
{
    char sql[200];
    if (strcmp(table, "state") == 0) snprintf(sql, sizeof sql, "SELECT count(*) FROM state WHERE deleted=0%s", vol ? " AND vol=?" : "");
    else if (strcmp(table, "conflicts") == 0) snprintf(sql, sizeof sql, "SELECT count(*) FROM conflicts%s", vol ? " WHERE vol=?" : "");
    else return -1;
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db->h, sql, -1, &s, NULL) == SQLITE_OK) {
        if (vol) sqlite3_bind_text(s, 1, vol, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    return n;
}

int64_t wps_db_unpushed(wps_db *db, const char *vol)
{
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db->h, "SELECT count(*) FROM state WHERE vol=? AND pushed=0", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, vol, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    return n;
}

uint64_t wps_db_clock(wps_db *db)
{
    pthread_mutex_lock(&db->mu);
    uint64_t c = db->clock;
    pthread_mutex_unlock(&db->mu);
    return c;
}

int wps_db_set_location(wps_db *db, const char *id, const char *vol, const char *relpath)
{
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db->h, "INSERT OR REPLACE INTO locations(id,vol,path,deleted) VALUES(?,?,?,0)", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, vol, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, relpath, -1, SQLITE_STATIC);
        rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    return rc;
}

int wps_db_get_location(wps_db *db, const char *id, char *vol, size_t vn, char *relpath, size_t rn)
{
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db->h, "SELECT vol,path FROM locations WHERE id=? AND deleted=0", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            snprintf(vol, vn, "%s", sqlite3_column_text(s, 0));
            snprintf(relpath, rn, "%s", sqlite3_column_text(s, 1));
            rc = 0;
        }
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    return rc;
}

int wps_db_locations(wps_db *db, wps_db_loc_fn fn, void *user)
{
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    int n = -1;
    if (sqlite3_prepare_v2(db->h, "SELECT id,vol,path FROM locations WHERE deleted=0", -1, &s, NULL) == SQLITE_OK) {
        n = 0;
        while (sqlite3_step(s) == SQLITE_ROW) {
            fn((const char *)sqlite3_column_text(s, 0), (const char *)sqlite3_column_text(s, 1),
               (const char *)sqlite3_column_text(s, 2), user);
            n++;
        }
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    return n;
}

int wps_db_location_forget(wps_db *db, const char *id)
{
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db->h, "UPDATE locations SET deleted=strftime('%s','now') WHERE id=?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
        rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    return rc;
}

/* ---------------------------------------------------------------- shadows */

int wps_db_shadow_put(wps_db *db, const char *link_vol, const char *link_rel, const char *target_id,
                      const char *target_vol, const char *target_rel)
{
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db->h, "INSERT OR REPLACE INTO shadows VALUES(?,?,?,?,?)", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, link_vol, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, link_rel, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, target_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 4, target_vol, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 5, target_rel, -1, SQLITE_STATIC);
        rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    return rc;
}

int wps_db_shadow_del(wps_db *db, const char *link_vol, const char *link_rel)
{
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db->h, "DELETE FROM shadows WHERE link_vol=? AND link_rel=?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, link_vol, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, link_rel, -1, SQLITE_STATIC);
        rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    return rc;
}

static int shadow_query(wps_db *db, const char *sql, const char *a, const char *b, wps_db_shadow_fn fn, void *user)
{
    /* Collect first: the callback may write to the database. */
    struct srow { char lv[WPS_VOLID_MAX], lr[4096], id[64], tv[WPS_VOLID_MAX], tr[4096]; } *rows = NULL;
    size_t n = 0, cap = 0;
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->h, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, a, -1, SQLITE_STATIC);
        if (b) sqlite3_bind_text(s, 2, b, -1, SQLITE_STATIC);
        while (sqlite3_step(s) == SQLITE_ROW) {
            if (n == cap) { cap = cap ? cap * 2 : 8; rows = realloc(rows, cap * sizeof *rows); }
            struct srow *r = &rows[n++];
            snprintf(r->lv, sizeof r->lv, "%s", sqlite3_column_text(s, 0));
            snprintf(r->lr, sizeof r->lr, "%s", sqlite3_column_text(s, 1));
            snprintf(r->id, sizeof r->id, "%s", sqlite3_column_text(s, 2));
            snprintf(r->tv, sizeof r->tv, "%s", sqlite3_column_text(s, 3) ? (const char *)sqlite3_column_text(s, 3) : "");
            snprintf(r->tr, sizeof r->tr, "%s", sqlite3_column_text(s, 4) ? (const char *)sqlite3_column_text(s, 4) : "");
        }
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    for (size_t i = 0; i < n; i++) fn(rows[i].lv, rows[i].lr, rows[i].id, rows[i].tv, rows[i].tr, user);
    free(rows);
    return (int)n;
}

int wps_db_shadows_for(wps_db *db, const char *target_id, wps_db_shadow_fn fn, void *user)
{
    return shadow_query(db, "SELECT * FROM shadows WHERE target_id=?", target_id, NULL, fn, user);
}

int wps_db_shadows_under(wps_db *db, const char *target_vol, const char *rel_prefix, wps_db_shadow_fn fn, void *user)
{
    char like[4200];
    snprintf(like, sizeof like, "%s/%%", rel_prefix);
    return shadow_query(db, "SELECT * FROM shadows WHERE target_vol=? AND target_rel LIKE ?", target_vol, like, fn, user);
}

/* ------------------------------------------------------------------- jobs */

int wps_db_job_put(wps_db *db, int64_t id, const char *kind, const char *src, const char *sel, const char *dest,
                   const char *state, int64_t items_done, int64_t bytes_done)
{
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db->h, "INSERT OR REPLACE INTO jobs(id,kind,src,sel,dest,state,items_done,bytes_done)"
                                  " VALUES(?,?,?,?,?,?,?,?)", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        sqlite3_bind_text(s, 2, kind, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, src, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 4, sel, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 5, dest, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 6, state, -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 7, items_done);
        sqlite3_bind_int64(s, 8, bytes_done);
        rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    return rc;
}

int wps_db_jobs_list(wps_db *db, wps_db_job_fn fn, void *user)
{
    struct jrow { int64_t id, items, bytes; char *kind, *src, *sel, *dest, *state; } *rows = NULL;
    size_t n = 0, cap = 0;
    pthread_mutex_lock(&db->mu);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->h, "SELECT id,kind,src,sel,dest,state,items_done,bytes_done FROM jobs ORDER BY id",
                           -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            if (n == cap) { cap = cap ? cap * 2 : 8; rows = realloc(rows, cap * sizeof *rows); }
            struct jrow *r = &rows[n++];
            r->id = sqlite3_column_int64(s, 0);
            r->kind = strdup((const char *)sqlite3_column_text(s, 1));
            r->src = strdup((const char *)sqlite3_column_text(s, 2));
            r->sel = strdup(sqlite3_column_text(s, 3) ? (const char *)sqlite3_column_text(s, 3) : "");
            r->dest = strdup((const char *)sqlite3_column_text(s, 4));
            r->state = strdup((const char *)sqlite3_column_text(s, 5));
            r->items = sqlite3_column_int64(s, 6);
            r->bytes = sqlite3_column_int64(s, 7);
        }
        sqlite3_finalize(s);
    }
    pthread_mutex_unlock(&db->mu);
    for (size_t i = 0; i < n; i++) {
        fn(rows[i].id, rows[i].kind, rows[i].src, rows[i].sel, rows[i].dest, rows[i].state, rows[i].items, rows[i].bytes, user);
        free(rows[i].kind); free(rows[i].src); free(rows[i].sel); free(rows[i].dest); free(rows[i].state);
    }
    free(rows);
    return (int)n;
}
