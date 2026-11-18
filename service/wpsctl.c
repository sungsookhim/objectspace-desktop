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
            "       wpsctl list FOLDER [OFFSET [COUNT [details]]]\n");
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
    const cv *ok = cv_get(&reply, "ok");
    int rc = ok && ok->k == CV_BOOL && ok->b ? 0 : 1;
    cv_free(&reply);
    cb_free(&j);
    cb_free(&b);
    free(body);
    close(fd);
    return rc;
}
