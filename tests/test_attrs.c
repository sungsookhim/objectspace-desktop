/* Tests for the attribute store: encoding, merge, locking, IDs, errors. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "attrs.h"
#include "check.h"

static char dir[4096];

static char *path_of(const char *name)
{
    static char p[4096 + 64];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return p;
}

static void touch(const char *name)
{
    int fd = open(path_of(name), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
}

TEST(encode_roundtrip)
{
    wps_props a, b;
    wps_props_init(&a);
    a.counter = 0x0102030405060708ull;
    wps_props_set_str(&a, "title", "Quarterly report");
    wps_props_set_int(&a, "x", -42);
    wps_props_set_bool(&a, "template", true);
    wps_props_set(&a, "future", 'Z', "\x01\x02\x03", 3);   /* unknown type: kept */
    size_t len;
    unsigned char *enc = wps_props_encode(&a, &len);
    CHECK(enc && wps_props_decode(enc, len, &b) == 0);
    CHECK(b.counter == a.counter && b.n == 4);
    char *t = NULL;
    int64_t x = 0;
    CHECK(wps_props_str(&b, "title", &t));
    CHECK_STR(t, "Quarterly report");
    free(t);
    CHECK(wps_props_int(&b, "x", &x) && x == -42);
    const wps_prop *f = wps_props_get(&b, "future");
    CHECK(f && f->type == 'Z' && f->len == 3 && memcmp(f->val, "\x01\x02\x03", 3) == 0);
    /* Truncated and garbage input is rejected, never misread. */
    wps_props c;
    for (size_t cut = 1; cut < len; cut++)
        if (wps_props_decode(enc, cut, &c) == 0) {
            CHECK(c.n < 4);   /* only whole entries, never a partial one */
            wps_props_free(&c);
        }
    unsigned char junk[16] = { 9, 1, 2, 3 };
    CHECK(wps_props_decode(junk, sizeof junk, &c) == -1);
    free(enc);
    wps_props_free(&a);
    wps_props_free(&b);
}

TEST(merge_keeps_other_keys)
{
    touch("m");
    wps_props s, r;
    wps_props_init(&s);
    wps_props_set_str(&s, "title", "One");
    wps_props_set_str(&s, "icon", "doc.png");
    CHECK(wps_attr_update(path_of("m"), "WPObject", &s, NULL, 200, NULL, NULL) == WPS_A_OK);
    wps_props_free(&s);
    /* A second writer changes only the title; the icon stays. */
    wps_props_init(&s);
    wps_props_set_str(&s, "title", "Two");
    CHECK(wps_attr_update(path_of("m"), "WPObject", &s, NULL, 200, NULL, &r) == WPS_A_OK);
    char *t = NULL, *i = NULL;
    CHECK(wps_props_str(&r, "title", &t) && wps_props_str(&r, "icon", &i));
    CHECK_STR(t, "Two");
    CHECK_STR(i, "doc.png");
    CHECK(r.counter == 2);
    free(t); free(i);
    wps_props_free(&r);
    /* Removal. */
    const char *rm[] = { "icon", NULL };
    CHECK(wps_attr_update(path_of("m"), "WPObject", NULL, rm, 200, NULL, &r) == WPS_A_OK);
    CHECK(wps_props_get(&r, "icon") == NULL && r.counter == 3);
    wps_props_free(&r);
    /* Classes are separate attributes. */
    wps_props_set_int(&s, "sort", 2);
    CHECK(wps_attr_update(path_of("m"), "WPFolder", &s, NULL, 200, NULL, NULL) == WPS_A_OK);
    CHECK(wps_attr_read(path_of("m"), "WPObject", &r) == WPS_A_OK && r.counter == 3);
    wps_props_free(&r);
    wps_props_free(&s);
}

TEST(optimistic_conflict)
{
    touch("c");
    wps_props s;
    wps_props_init(&s);
    wps_props_set_str(&s, "title", "A");
    uint64_t seen = 0;
    CHECK(wps_attr_update(path_of("c"), "WPObject", &s, NULL, 200, &seen, NULL) == WPS_A_OK);
    /* Another writer gets in first ... */
    CHECK(wps_attr_update(path_of("c"), "WPObject", &s, NULL, 200, NULL, NULL) == WPS_A_OK);
    /* ... so a write that expected counter 1 is refused with the current one. */
    seen = 1;
    CHECK(wps_attr_update(path_of("c"), "WPObject", &s, NULL, 200, &seen, NULL) == WPS_A_ERROR);
    CHECK(seen == 2);
    wps_props_free(&s);
}

TEST(concurrent_writers_lose_nothing)
{
    /* Two processes, 500 updates each, each to its own keys. */
    touch("race");
    pid_t kids[2];
    for (int k = 0; k < 2; k++) {
        kids[k] = fork();
        if (kids[k] == 0) {
            for (int i = 0; i < 500; i++) {
                wps_props s;
                wps_props_init(&s);
                char key[32];
                snprintf(key, sizeof key, "%c%d", k ? 'b' : 'a', i % 50);
                wps_props_set_int(&s, key, i);
                if (wps_attr_update(path_of("race"), "WPObject", &s, NULL, 5000, NULL, NULL) != WPS_A_OK) _exit(1);
                wps_props_free(&s);
            }
            _exit(0);
        }
    }
    int ok = 1;
    for (int k = 0; k < 2; k++) {
        int st;
        waitpid(kids[k], &st, 0);
        ok &= WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }
    CHECK(ok);
    wps_props r;
    CHECK(wps_attr_read(path_of("race"), "WPObject", &r) == WPS_A_OK);
    CHECK(r.counter == 1000);   /* every update applied exactly once */
    CHECK(r.n == 100);          /* 50 keys from each writer */
    wps_props_free(&r);
}

TEST(lock_timeout_reports_busy)
{
    touch("busy");
    int holder = open(path_of("busy"), O_RDONLY);
    CHECK(flock(holder, LOCK_EX) == 0);   /* someone holds the lock and never lets go */
    wps_props s;
    wps_props_init(&s);
    wps_props_set_str(&s, "title", "x");
    CHECK(wps_attr_update(path_of("busy"), "WPObject", &s, NULL, 100, NULL, NULL) == WPS_A_BUSY);
    close(holder);
    CHECK(wps_attr_update(path_of("busy"), "WPObject", &s, NULL, 100, NULL, NULL) == WPS_A_OK);
    wps_props_free(&s);
}

TEST(ids)
{
    touch("id");
    char a[WPS_ID_LEN + 1], b[WPS_ID_LEN + 1], got[WPS_ID_LEN + 1];
    wps_new_id(a);
    wps_new_id(b);
    CHECK(strlen(a) == 36 && a[14] == '4' && strcmp(a, b) != 0);
    CHECK(!wps_attr_get_id(path_of("id"), got));
    CHECK(wps_attr_assign_id(path_of("id"), a, got) == WPS_A_OK);
    CHECK_STR(got, a);
    /* A second assignment keeps the first ID. */
    CHECK(wps_attr_assign_id(path_of("id"), b, got) == WPS_A_OK);
    CHECK_STR(got, a);
}

TEST(errors_are_classified)
{
    wps_props r, s;
    wps_props_init(&s);
    wps_props_set_str(&s, "t", "x");
    CHECK(wps_attr_read(path_of("missing"), "WPObject", &r) == WPS_A_NOFILE);
    /* Symlinks carry no user attributes; they are never followed. */
    touch("target");
    char target[4200];
    snprintf(target, sizeof target, "%s", path_of("target"));
    CHECK(symlink(target, path_of("link")) == 0);
    CHECK(wps_attr_update(path_of("link"), "WPObject", &s, NULL, 100, NULL, NULL) == WPS_A_NOTSUP);
    CHECK(wps_attr_read(path_of("target"), "WPObject", &r) == WPS_A_OK && r.n == 0);
    wps_props_free(&r);
    /* A corrupt stored value is reported and left alone. */
    touch("bad");
    setxattr(path_of("bad"), "user.wps.WPObject", "\x07garbage", 8, 0);
    CHECK(wps_attr_update(path_of("bad"), "WPObject", &s, NULL, 100, NULL, NULL) == WPS_A_CORRUPT);
    char raw[16];
    CHECK(getxattr(path_of("bad"), "user.wps.WPObject", raw, sizeof raw) == 8);
    /* Read-only file: no permission (unless running as root). */
    touch("ro");
    chmod(path_of("ro"), 0444);
    if (geteuid() != 0) CHECK(wps_attr_update(path_of("ro"), "WPObject", &s, NULL, 100, NULL, NULL) == WPS_A_NOPERM);
    wps_props_free(&s);
}

int main(int argc, char **argv)
{
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "build/tests/attrs-tmp");
    char cmd[8400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd) != 0) return 1;
    if (setxattr(dir, "user.wps.probe", "1", 1, 0) < 0 && errno == ENOTSUP) {
        printf("skip: %s does not support user attributes\n", dir);
        return 0;
    }
    RUN(encode_roundtrip);
    RUN(merge_keeps_other_keys);
    RUN(optimistic_conflict);
    RUN(concurrent_writers_lose_nothing);
    RUN(lock_timeout_reports_busy);
    RUN(ids);
    RUN(errors_are_classified);
    return CHECK_SUMMARY();
}
