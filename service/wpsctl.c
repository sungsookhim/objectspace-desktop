/*
 * wpsctl - command-line client for wpsd. Prints each reply as one line of
 * JSON and exits 0 if the reply was ok, 1 if not, 2 on usage errors.
 *
 *   wpsctl ping | stats | flush | sync | scan | shutdown
 *   wpsctl resolve TARGET
 *   wpsctl call [--expect N] TARGET METHOD [ARG...]
 *   wpsctl describe CLASS
 *   wpsctl shadow TARGET FOLDER
 *   wpsctl create FOLDER CLASS TITLE [COMMAND]
 *   wpsctl list FOLDER [OFFSET [COUNT [details]]]
 *   wpsctl select FOLDER all [EXCEPT...] | select FOLDER ITEM...
 *   wpsctl move|copy SELECTION DEST
 *   wpsctl job ID | jobs | job_cancel ID | job_pause ID | job_resume ID
 *   wpsctl job_decide ID skip|rename [all]
 *   wpsctl menu TARGET | menu_select TARGET ITEM | settings TARGET | settings_set TARGET KEY=VALUE...
 *   wpsctl watch [folder PATH]... [object TARGET]... [jobs]
 *   wpsctl open TARGET [confirm] | from_template TEMPLATE FOLDER TITLE | rename TARGET NAME
 *
 * TARGET is a path or "id:<ID>". Arguments: true, false, integers, decimal
 * numbers, @PATH or @id:ID for an object, =TEXT to force text, anything
 * else is text. The socket is $WPS_SOCKET, else $WPS_RUNTIME_DIR/wps.sock,
 * else $XDG_RUNTIME_DIR/wps.sock.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "cbor.h"

static int usage(void)
{
    fprintf(stderr,
            "usage: wpsctl ping|stats|flush|sync|scan|shutdown\n"
            "       wpsctl resolve TARGET\n"
            "       wpsctl call [--expect N] TARGET METHOD [ARG...]\n"
            "       wpsctl describe CLASS\n"
            "       wpsctl shadow TARGET FOLDER\n"
            "       wpsctl create FOLDER CLASS TITLE [COMMAND]\n"
            "       wpsctl list FOLDER [OFFSET [COUNT [details]]]\n"
            "       wpsctl select FOLDER all [EXCEPT...] | select FOLDER ITEM...\n"
            "       wpsctl move|copy SELECTION DEST\n"
            "       wpsctl job ID | jobs | job_cancel|job_pause|job_resume ID\n"
            "       wpsctl job_decide ID skip|rename [all]\n"
            "       wpsctl menu TARGET | menu_select TARGET ITEM | settings TARGET\n"
            "       wpsctl settings_set TARGET KEY=VALUE...\n"
            "       wpsctl watch [folder PATH]... [object TARGET]... [jobs]\n"
            "       wpsctl open TARGET [confirm] | from_template TEMPLATE FOLDER TITLE | rename TARGET NAME\n");
    return 2;
}

static int connect_socket(void)
{
    char path[PATH_MAX];
    const char *s = getenv("WPS_SOCKET");
    const char *rt = getenv("WPS_RUNTIME_DIR");
    if (!rt || !*rt) rt = getenv("XDG_RUNTIME_DIR");
    if (s && *s) snprintf(path, sizeof path, "%s", s);
    else if (rt && *rt) snprintf(path, sizeof path, "%s/wps.sock", rt);
    else snprintf(path, sizeof path, "/tmp/wps-%u/wps.sock", (unsigned)getuid());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof sa.sun_path) {
        fprintf(stderr, "wpsctl: socket path too long: %s\n", path);
        if (fd >= 0) close(fd);
        return -1;
    }
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "wpsctl: cannot connect to %s: %s\n", path, strerror(errno));
        return -1;
    }
    return fd;
}

static int io_all(int fd, void *buf, size_t n, bool writing)
{
    unsigned char *p = buf;
    while (n) {
        ssize_t r = writing ? write(fd, p, n) : read(fd, p, n);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void put_arg(cbuf *b, const char *a)
{
    char *end;
    if (strcmp(a, "true") == 0) { cb_bool(b, true); return; }
    if (strcmp(a, "false") == 0) { cb_bool(b, false); return; }
    if (a[0] == '=') { cb_text(b, a + 1); return; }
    if (a[0] == '@') { cb_map(b, 1); cb_text(b, "obj"); cb_text(b, a + 1); return; }
    errno = 0;
    long long v = strtoll(a, &end, 10);
    if (*a && !*end && !errno) { cb_int(b, v); return; }
    double d = strtod(a, &end);
    if (*a && !*end) { cb_double(b, d); return; }
    cb_text(b, a);
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *op = argv[1];
    cbuf b;
    cb_init(&b);
    int i = 2;
    bool watching = false;
    if (!strcmp(op, "ping") || !strcmp(op, "stats") || !strcmp(op, "flush") ||
        !strcmp(op, "sync") || !strcmp(op, "scan") || !strcmp(op, "shutdown")) {
        if (argc != 2) return usage();
        cb_map(&b, 2);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
    } else if (!strcmp(op, "resolve") || !strcmp(op, "describe")) {
        if (argc != 3) return usage();
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, op[0] == 'r' ? "target" : "class"); cb_text(&b, argv[2]);
    } else if (!strcmp(op, "select")) {
        /* select FOLDER all [EXCEPT...] | select FOLDER ITEM... */
        if (argc < 4) return usage();
        bool all = !strcmp(argv[3], "all");
        int first = all ? 4 : 3;
        cb_map(&b, 5);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, "select");
        cb_text(&b, "folder"); cb_text(&b, argv[2]);
        cb_text(&b, "all"); cb_bool(&b, all);
        cb_text(&b, all ? "except" : "items");
        cb_array(&b, (size_t)(argc - first));
        for (int k = first; k < argc; k++) cb_text(&b, argv[k]);
    } else if (!strcmp(op, "move") || !strcmp(op, "copy")) {
        if (argc != 4) return usage();
        cb_map(&b, 4);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "selection"); cb_int(&b, atoll(argv[2]));
        cb_text(&b, "dest"); cb_text(&b, argv[3]);
    } else if (!strcmp(op, "job") || !strcmp(op, "job_cancel") || !strcmp(op, "job_pause") ||
               !strcmp(op, "job_resume")) {
        if (argc != 3) return usage();
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "job"); cb_int(&b, atoll(argv[2]));
    } else if (!strcmp(op, "jobs")) {
        cb_map(&b, 2);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, "jobs");
    } else if (!strcmp(op, "job_decide")) {
        if (argc < 4) return usage();
        cb_map(&b, 5);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, "job_decide");
        cb_text(&b, "job"); cb_int(&b, atoll(argv[2]));
        cb_text(&b, "decision"); cb_text(&b, argv[3]);
        cb_text(&b, "all"); cb_bool(&b, argc > 4 && !strcmp(argv[4], "all"));
    } else if (!strcmp(op, "open")) {
        if (argc < 3 || argc > 4) return usage();
        cb_map(&b, 4);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, "open");
        cb_text(&b, "target"); cb_text(&b, argv[2]);
        cb_text(&b, "confirm"); cb_bool(&b, argc == 4 && !strcmp(argv[3], "confirm"));
    } else if (!strcmp(op, "rename")) {
        if (argc != 4) return usage();
        cb_map(&b, 4);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "target"); cb_text(&b, argv[2]);
        cb_text(&b, "name"); cb_text(&b, argv[3]);
    } else if (!strcmp(op, "from_template")) {
        if (argc != 5) return usage();
        cb_map(&b, 5);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "target"); cb_text(&b, argv[2]);
        cb_text(&b, "folder"); cb_text(&b, argv[3]);
        cb_text(&b, "title"); cb_text(&b, argv[4]);
    } else if (!strcmp(op, "menu") || !strcmp(op, "settings")) {
        if (argc != 3) return usage();
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "target"); cb_text(&b, argv[2]);
    } else if (!strcmp(op, "menu_select")) {
        if (argc != 4) return usage();
        cb_map(&b, 4);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "target"); cb_text(&b, argv[2]);
        cb_text(&b, "item"); cb_int(&b, atoll(argv[3]));
    } else if (!strcmp(op, "settings_set")) {
        /* settings_set TARGET KEY=VALUE... (numbers and true/false are typed) */
        if (argc < 4) return usage();
        cb_map(&b, 4);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "target"); cb_text(&b, argv[2]);
        cb_text(&b, "values");
        cb_map(&b, (size_t)(argc - 3));
        for (int k = 3; k < argc; k++) {
            char *eq = strchr(argv[k], '=');
            if (!eq) return usage();
            cb_textn(&b, argv[k], (size_t)(eq - argv[k]));
            put_arg(&b, eq + 1);
        }
    } else if (!strcmp(op, "watch")) {
        /* watch [folder PATH]... [object TARGET]... [jobs]: print events until interrupted */
        size_t nf = 0, no = 0;
        bool jobs = false;
        for (int k = 2; k < argc; k++) {
            if (!strcmp(argv[k], "jobs")) jobs = true;
            else if (!strcmp(argv[k], "folder") && k + 1 < argc) { nf++; k++; }
            else if (!strcmp(argv[k], "object") && k + 1 < argc) { no++; k++; }
            else return usage();
        }
        cb_map(&b, 5);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, "subscribe");
        cb_text(&b, "folders"); cb_array(&b, nf);
        for (int k = 2; k < argc; k++) if (!strcmp(argv[k], "folder")) cb_text(&b, argv[++k]); else if (!strcmp(argv[k], "object")) k++;
        cb_text(&b, "objects"); cb_array(&b, no);
        for (int k = 2; k < argc; k++) if (!strcmp(argv[k], "object")) cb_text(&b, argv[++k]); else if (!strcmp(argv[k], "folder")) k++;
        cb_text(&b, "jobs"); cb_bool(&b, jobs);
        watching = true;
    } else if (!strcmp(op, "list")) {
        if (argc < 3 || argc > 6) return usage();
        bool details = argc == 6 && !strcmp(argv[5], "details");
        cb_map(&b, 6);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, "list");
        cb_text(&b, "target"); cb_text(&b, argv[2]);
        cb_text(&b, "offset"); cb_int(&b, argc > 3 ? atoll(argv[3]) : 0);
        cb_text(&b, "count"); cb_int(&b, argc > 4 ? atoll(argv[4]) : 100);
        cb_text(&b, "details"); cb_bool(&b, details);
    } else if (!strcmp(op, "create")) {
        if (argc < 5 || argc > 6) return usage();
        cb_map(&b, argc == 6 ? 6 : 5);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, "create");
        cb_text(&b, "folder"); cb_text(&b, argv[2]);
        cb_text(&b, "class"); cb_text(&b, argv[3]);
        cb_text(&b, "title"); cb_text(&b, argv[4]);
        if (argc == 6) { cb_text(&b, "command"); cb_text(&b, argv[5]); }
    } else if (!strcmp(op, "shadow")) {
        if (argc != 4) return usage();
        cb_map(&b, 4);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, "shadow");
        cb_text(&b, "target"); cb_text(&b, argv[2]);
        cb_text(&b, "folder"); cb_text(&b, argv[3]);
    } else if (!strcmp(op, "call")) {
        long long expect = -1;
        if (i + 1 < argc && !strcmp(argv[i], "--expect")) { expect = atoll(argv[i + 1]); i += 2; }
        if (argc - i < 2) return usage();
        cb_map(&b, expect >= 0 ? 6 : 5);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, "call");
        cb_text(&b, "target"); cb_text(&b, argv[i]);
        cb_text(&b, "method"); cb_text(&b, argv[i + 1]);
        cb_text(&b, "args"); cb_array(&b, (size_t)(argc - i - 2));
        for (int k = i + 2; k < argc; k++) put_arg(&b, argv[k]);
        if (expect >= 0) { cb_text(&b, "expect"); cb_int(&b, expect); }
    } else {
        return usage();
    }

    int fd = connect_socket();
    if (fd < 0) { cb_free(&b); return 1; }
    unsigned char hdr[4] = { (unsigned char)(b.n >> 24), (unsigned char)(b.n >> 16),
                             (unsigned char)(b.n >> 8), (unsigned char)b.n };
    if (io_all(fd, hdr, 4, true) || io_all(fd, b.d, b.n, true) || io_all(fd, hdr, 4, false)) {
        fprintf(stderr, "wpsctl: connection failed\n");
        cb_free(&b);
        return 1;
    }
    uint32_t n = (uint32_t)hdr[0] << 24 | (uint32_t)hdr[1] << 16 | (uint32_t)hdr[2] << 8 | hdr[3];
    unsigned char *body = n <= (1u << 24) ? malloc(n ? n : 1) : NULL;
    cv reply;
    if (!body || io_all(fd, body, n, false) || cv_decode(body, n, &reply)) {
        fprintf(stderr, "wpsctl: bad reply\n");
        free(body);
        cb_free(&b);
        return 1;
    }
    cbuf j;
    cb_init(&j);
    cv_json(&reply, &j);
    printf("%.*s\n", (int)j.n, (const char *)j.d);
    fflush(stdout);
    /* watch: then print each event as one line of JSON, until interrupted. */
    while (watching) {
        unsigned char h[4];
        if (io_all(fd, h, 4, false)) break;
        uint32_t len = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
        unsigned char *ev = len <= (1u << 24) ? malloc(len ? len : 1) : NULL;
        cv e;
        if (!ev || io_all(fd, ev, len, false) || cv_decode(ev, len, &e)) { free(ev); break; }
        cbuf o;
        cb_init(&o);
        cv_json(&e, &o);
        printf("%.*s\n", (int)o.n, (const char *)o.d);
        fflush(stdout);
        cb_free(&o);
        cv_free(&e);
        free(ev);
    }
    const cv *ok = cv_get(&reply, "ok");
    int rc = ok && ok->k == CV_BOOL && ok->b ? 0 : 1;
    cv_free(&reply);
    cb_free(&j);
    cb_free(&b);
    free(body);
    close(fd);
    return rc;
}
