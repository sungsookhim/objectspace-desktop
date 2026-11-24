/* Tests for the database and git-style volume synchronisation. Two
 * "computers" (two main databases with different writers) share one volume
 * database, as a USB disk moved between them would. */
#define _GNU_SOURCE
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "db.h"

#define VOL "1234-ABCD"
static char dir[4096];

static char *p(const char *name)
{
    static char buf[4][4200];
    static int i;
    i = (i + 1) % 4;
    snprintf(buf[i], sizeof buf[i], "%s/%s", dir, name);
    return buf[i];
}

static osd_db *open_db(const char *name, const char *writer)
{
    char err[256];
    osd_db *db = osd_db_open(p(name), writer, err, sizeof err);
    if (!db) fprintf(stderr, "open: %s\n", err);
    return db;
}

static void put_title(osd_db *db, const char *loc, const char *title)
{
    char key[256];
    osd_db_key(key, sizeof key, loc, "OsdObject", "title");
    osd_db_put(db, VOL, key, 's', title, (uint32_t)strlen(title));
}

static char *get_title(osd_db *db, const char *loc)
{
    osd_props ps;
    char *t = NULL;
    osd_db_get_class(db, VOL, loc, "OsdObject", &ps);
    if (!osd_props_str(&ps, "title", &t)) t = strdup("(none)");
    osd_props_free(&ps);
    return t;
}

static int attach(osd_db *db, bool writable, osd_sync_stats *st)
{
    char err[256];
    int rc = osd_db_attach_volume(db, VOL, p("vol.db"), writable, st, err, sizeof err);
    if (rc) fprintf(stderr, "attach: %s\n", err);
    return rc;
}

#define TITLE_IS(db, loc, want) do { char *t_ = get_title(db, loc); CHECK_STR(t_, want); free(t_); } while (0)

TEST(local_rows)
{
    osd_db *a = open_db("local.db", "idA/hostA");
    put_title(a, "path:notes.txt", "Notes");
    TITLE_IS(a, "path:notes.txt", "Notes");
    put_title(a, "path:notes.txt", "Notes 2");
    TITLE_IS(a, "path:notes.txt", "Notes 2");
    char key[256];
    osd_db_key(key, sizeof key, "path:notes.txt", "OsdObject", "title");
    osd_db_delete(a, VOL, key);
    TITLE_IS(a, "path:notes.txt", "(none)");   /* tombstone hides it */
    CHECK(osd_db_count(a, "state", VOL) == 0);
    /* Other classes and locators are separate. */
    put_title(a, "id-1", "One");
    put_title(a, "id-12", "Twelve");           /* prefix of another locator must not match */
    TITLE_IS(a, "id-1", "One");
    struct stat st;
    CHECK(stat(p("local.db"), &st) == 0 && (st.st_mode & 0777) == 0600);
    osd_db_close(a);
}

TEST(state_travels_between_computers)
{
    osd_db *a = open_db("a.db", "idX/hostA"), *b = open_db("b.db", "idX/hostB");
    osd_sync_stats st;
    CHECK(attach(a, true, &st) == 0);
    put_title(a, "path:report.odt", "Q3 report");      /* written through to the volume */
    CHECK(osd_db_unpushed(a, VOL) == 0);
    osd_db_detach_volume(a, VOL);
    /* The disk moves to computer B. */
    CHECK(attach(b, true, &st) == 0);
    CHECK(st.pulled == 1 && st.conflicts == 0);
    TITLE_IS(b, "path:report.odt", "Q3 report");
    CHECK(osd_db_clock(b) >= osd_db_clock(a));          /* B's clock caught up */
    /* B edits; the disk goes back to A. */
    put_title(b, "path:report.odt", "Q3 report (final)");
    osd_db_detach_volume(b, VOL);
    CHECK(attach(a, true, &st) == 0);
    CHECK(st.pulled == 1 && st.conflicts == 0);
    TITLE_IS(a, "path:report.odt", "Q3 report (final)");
    osd_db_detach_volume(a, VOL);
    osd_db_close(a);
    osd_db_close(b);
}

TEST(no_false_conflict_with_own_old_value)
{
    osd_db *a = open_db("a2.db", "idY/hostA");
    unlink(p("vol.db"));
    attach(a, true, NULL);
    put_title(a, "path:x", "v1");
    osd_db_detach_volume(a, VOL);
    put_title(a, "path:x", "v2");                      /* changed while the disk was away */
    CHECK(osd_db_unpushed(a, VOL) == 1);
    osd_sync_stats st;
    attach(a, true, &st);
    CHECK(st.conflicts == 0 && st.pushed == 1);        /* the disk only had our older value */
    CHECK(osd_db_unpushed(a, VOL) == 0);
    osd_db_detach_volume(a, VOL);
    osd_db_close(a);
}

TEST(both_sides_changed)
{
    unlink(p("vol.db"));
    osd_db *a = open_db("a3.db", "idZ/hostA"), *b = open_db("b3.db", "idZ/hostB");
    attach(a, true, NULL);
    put_title(a, "path:plan.txt", "Plan");
    osd_db_detach_volume(a, VOL);
    attach(b, true, NULL);                             /* B gets "Plan" */
    osd_db_detach_volume(b, VOL);
    /* Apart: A edits without the disk; B mounts it and edits too. */
    put_title(a, "path:plan.txt", "Plan from A");
    attach(b, true, NULL);
    put_title(b, "path:plan.txt", "Plan from B");
    osd_db_detach_volume(b, VOL);
    osd_sync_stats st;
    attach(a, true, &st);
    CHECK(st.conflicts == 1);
    CHECK(osd_db_count(a, "conflicts", VOL) == 1);     /* the losing value is kept */
    char *wa = get_title(a, "path:plan.txt");
    osd_db_detach_volume(a, VOL);
    attach(b, true, &st);                              /* B learns the outcome */
    char *wb = get_title(b, "path:plan.txt");
    CHECK_STR(wa, wb);                                 /* both computers agree */
    CHECK(strcmp(wa, "Plan from A") == 0 || strcmp(wa, "Plan from B") == 0);
    printf("      winner: \"%s\"\n", wa);
    free(wa);
    free(wb);
    osd_db_detach_volume(b, VOL);
    osd_db_close(a);
    osd_db_close(b);
}

TEST(read_only_volume)
{
    unlink(p("vol.db"));
    osd_db *a = open_db("a4.db", "idR/hostA");
    attach(a, true, NULL);
    put_title(a, "path:f", "first");
    osd_db_detach_volume(a, VOL);
    osd_sync_stats st;
    CHECK(attach(a, false, &st) == 0);                 /* mounted read-only */
    put_title(a, "path:f", "changed while read-only");
    CHECK(osd_db_unpushed(a, VOL) == 1);               /* waits */
    CHECK(attach(a, true, &st) == 0);                  /* remounted writable */
    CHECK(st.pushed == 1 && osd_db_unpushed(a, VOL) == 0);
    osd_db_detach_volume(a, VOL);
    osd_db *b = open_db("b4.db", "idR/hostB");
    attach(b, true, NULL);
    TITLE_IS(b, "path:f", "changed while read-only");
    osd_db_close(a);
    osd_db_close(b);
}

TEST(deletions_travel)
{
    unlink(p("vol.db"));
    osd_db *a = open_db("a5.db", "idD/hostA"), *b = open_db("b5.db", "idD/hostB");
    attach(a, true, NULL);
    put_title(a, "path:old", "Old");
    osd_db_detach_volume(a, VOL);
    attach(b, true, NULL);
    TITLE_IS(b, "path:old", "Old");
    char key[256];
    osd_db_key(key, sizeof key, "path:old", "OsdObject", "title");
    osd_db_delete(b, VOL, key);
    osd_db_detach_volume(b, VOL);
    attach(a, true, NULL);
    TITLE_IS(a, "path:old", "(none)");                 /* not resurrected by A's old copy */
    osd_db_close(a);
    osd_db_close(b);
}

/* 0.2: the databases' format version (user_version). */
static void versions(void)
{
    char err[512];
    osd_db *a = osd_db_open(p("v-new.db"), "w1", err, sizeof err);
    CHECK(a && osd_db_version(a) == OSD_DB_VERSION);   /* a new one: the current version */
    osd_db_close(a);
    /* 0.1's (no version): brought up, a copy kept */
    sqlite3 *h;
    CHECK(sqlite3_open(p("v-old.db"), &h) == SQLITE_OK);
    sqlite3_exec(h, "PRAGMA user_version=0", NULL, NULL, NULL);
    sqlite3_close(h);
    a = osd_db_open(p("v-old.db"), "w1", err, sizeof err);
    CHECK(a && osd_db_version(a) == OSD_DB_VERSION);
    osd_db_close(a);
    struct stat st;
    CHECK(stat(p("v-old.db.v0.bak"), &st) == 0);
    /* a newer one: refused, with a message */
    CHECK(sqlite3_open(p("v-future.db"), &h) == SQLITE_OK);
    sqlite3_exec(h, "PRAGMA user_version=99", NULL, NULL, NULL);
    sqlite3_close(h);
    err[0] = 0;
    a = osd_db_open(p("v-future.db"), "w1", err, sizeof err);
    CHECK(!a && strstr(err, "newer WPS"));
    if (a) osd_db_close(a);
}

int main(int argc, char **argv)
{
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "build/tests/db-tmp");
    char cmd[8400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd) != 0) return 1;
    RUN(local_rows);
    RUN(state_travels_between_computers);
    RUN(no_false_conflict_with_own_old_value);
    RUN(both_sides_changed);
    RUN(read_only_volume);
    RUN(deletions_travel);
    RUN(versions);
    return CHECK_SUMMARY();
}
