/*
 * osdctl - command-line client for osdd. Prints each reply as one line of
 * JSON and exits 0 if the reply was ok, 1 if not, 2 on usage errors.
 *
 *   osdctl ping | stats | flush | sync | scan | shutdown
 *   osdctl resolve TARGET
 *   osdctl call [--expect N] TARGET METHOD [ARG...]
 *   osdctl describe CLASS
 *   osdctl shadow TARGET FOLDER
 *   osdctl create FOLDER CLASS TITLE [COMMAND]
 *   osdctl list FOLDER [OFFSET [COUNT [details]]]
 *   osdctl select FOLDER all [EXCEPT...] | select FOLDER ITEM...
 *   osdctl move|copy SELECTION DEST
 *   osdctl job ID | jobs | job_cancel ID | job_pause ID | job_resume ID
 *   osdctl job_decide ID skip|rename [all]
 *   osdctl menu TARGET | menu_select TARGET ITEM | settings TARGET | settings_set TARGET KEY=VALUE...
 *   osdctl watch [folder PATH]... [object TARGET]... [jobs]
 *   osdctl open TARGET [confirm] | from_template TEMPLATE FOLDER TITLE | rename TARGET NAME
 *   osdctl restore-defaults [DESKTOP]   the default objects back in place (the desktop
 *                                       last opened, if not named)
 *   osdctl icon pack INPUT... OUT.osdicon   an icon package (roadmap 0.4): PNGs and an
 *                                       SVG, or folders of them; the standard sizes added
 *   osdctl icon list PACKAGE            its sizes
 *   osdctl icon render PACKAGE SIZE OUT.png   the icon at a size, by the size rules
 *   (the icon commands work on files: no osdd)
 *
 * TARGET is a path or "id:<ID>". Arguments: true, false, integers, decimal
 * numbers, @PATH or @id:ID for an object, =TEXT to force text, anything
 * else is text. The socket is $OSD_SOCKET, else $OSD_RUNTIME_DIR/objectspace.sock,
 * else $XDG_RUNTIME_DIR/objectspace.sock.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "cbor.h"
#include "osdicon.h"

static int usage(void)
{
    fprintf(stderr,
            "usage: osdctl ping|stats|flush|sync|scan|shutdown|setup\n"
            "       osdctl resolve TARGET\n"
            "       osdctl call [--expect N] TARGET METHOD [ARG...]\n"
            "       osdctl describe CLASS\n"
            "       osdctl shadow TARGET FOLDER\n"
            "       osdctl create FOLDER CLASS TITLE [COMMAND]\n"
            "       osdctl list FOLDER [OFFSET [COUNT [details]]]\n"
            "       osdctl find FOLDER PATTERN [sub] [any|folder|file|program] [LIMIT]\n"
            "       osdctl select FOLDER all [EXCEPT...] | select FOLDER ITEM...\n"
            "       osdctl move|copy SELECTION DEST\n"
            "       osdctl job ID | jobs | job_cancel|job_pause|job_resume ID\n"
            "       osdctl job_decide ID skip|rename [all]\n"
            "       osdctl menu TARGET | menu_select TARGET ITEM | settings TARGET\n"
            "       osdctl settings_set TARGET KEY=VALUE...\n"
            "       osdctl get TARGET NAME | set TARGET 'NAME=VALUE;NAME=VALUE' | props TARGET | reset TARGET 'NAME;NAME'\n"
            "         (TARGET also class:NAME or type:MIME: the settings of all objects of a class or a type)\n"
            "       osdctl watch [folder PATH]... [object TARGET]... [jobs]\n"
            "       osdctl open TARGET [confirm] | from_template TEMPLATE FOLDER TITLE | rename TARGET NAME\n"
            "       osdctl layout FOLDER | arrange FOLDER | place FOLDER TARGET X Y [TARGET X Y]...\n"
            "       osdctl say [--stay] TEXT   (a message at the bottom of the desktop; --stay: until clicked)\n"
            "       osdctl vmove vobj:ID FOLDER [copy]\n"
            "       osdctl restore-defaults [DESKTOP]\n"
            "       osdctl icon pack INPUT... OUT.osdicon | icon list PACKAGE | icon render PACKAGE SIZE OUT.png\n"
            "       osdctl plugin new NAME [PARENT]   (a plug-in's skeleton: sdk/README.md)\n");
    return 2;
}

static int connect_socket(void)
{
    char path[PATH_MAX];
    const char *s = getenv("OSD_SOCKET");
    const char *rt = getenv("OSD_RUNTIME_DIR");
    if (!rt || !*rt) rt = getenv("XDG_RUNTIME_DIR");
    if (s && *s) snprintf(path, sizeof path, "%s", s);
    else if (rt && *rt) snprintf(path, sizeof path, "%s/objectspace.sock", rt);
    else snprintf(path, sizeof path, "/tmp/objectspace-%u/objectspace.sock", (unsigned)getuid());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof sa.sun_path) {
        fprintf(stderr, "osdctl: socket path too long: %s\n", path);
        if (fd >= 0) close(fd);
        return -1;
    }
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "osdctl: cannot connect to %s: %s\n", path, strerror(errno));
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

/* osdctl icon ...: icon packages, on files (osdicon.h) */
static int icon_cmd(int argc, char **argv)
{
    if (argc >= 5 && !strcmp(argv[2], "pack")) {
        char err[512];
        if (osdicon_pack((const char *const *)argv + 3, (size_t)(argc - 4), argv[argc - 1], err, sizeof err)) {
            fprintf(stderr, "osdctl: icon pack: %s\n", err);
            return 1;
        }
        osdicon *w = osdicon_open(argv[argc - 1]);
        int sz[32];
        size_t n = w ? osdicon_sizes(w, sz, 32) : 0;
        printf("%s:", argv[argc - 1]);
        for (size_t i = 0; i < n; i++) printf(" %d", sz[i]);
        printf("%s\n", w && osdicon_has_svg(w) ? " svg" : "");
        osdicon_free(w);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[2], "list")) {
        osdicon *w = osdicon_open(argv[3]);
        if (!w) { fprintf(stderr, "osdctl: %s: not an icon package or PNG\n", argv[3]); return 1; }
        int sz[32];
        size_t n = osdicon_sizes(w, sz, 32);
        for (size_t i = 0; i < n; i++) printf("%s%d", i ? " " : "", sz[i]);
        printf("%s\n", osdicon_has_svg(w) ? (n ? " svg" : "svg") : "");
        osdicon_free(w);
        return 0;
    }
    if (argc == 6 && !strcmp(argv[2], "render")) {
        osdicon *w = osdicon_open(argv[3]);
        int px = atoi(argv[4]), how = 0;
        if (!w || px < 1 || px > 1024) {
            fprintf(stderr, "osdctl: icon render: a package and a size (1..1024)\n");
            osdicon_free(w);
            return 1;
        }
        cairo_surface_t *s = osdicon_render(w, px, &how);
        osdicon_free(w);
        if (!s || cairo_surface_write_to_png(s, argv[5]) != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "osdctl: icon render: cannot make %s\n", argv[5]);
            if (s) cairo_surface_destroy(s);
            return 1;
        }
        cairo_surface_destroy(s);
        static const char *const rule[] = { "", "exact", "svg", "a whole multiple, averaged down", "the nearest larger, scaled down",
                                            "the nearest smaller, scaled up" };
        printf("%s (%s)\n", argv[5], how >= 1 && how <= 5 ? rule[how] : "?");
        return 0;
    }
    return usage();
}

/* osdctl plugin new NAME [PARENT]: a folder NAME with a plug-in's skeleton
 * (the SDK's sample, sdk/sample: its .osdi, its C and its Makefile). */
static const char plugin_makefile[] =
    "# An Objectspace plug-in, built with the SDK (the objectspace-devel package): make, then\n"
    "# make install (your own plug-ins: ~/.config/objectspace/plugins), and log in again.\n"
    "# (osdctl plugin new NAME makes one like this; OSD_SDK and OSD_INC say\n"
    "# where the SDK is, for a checkout: make OSD_SDK=... OSD_INC=...)\n"
    "NAME    ?= OsdImageFile\n"
    "SO      ?= $(shell echo $(NAME) | tr A-Z a-z).so\n"
    "OSD_SDK ?= /usr/share/objectspace/sdk\n"
    "OSD_INC ?= /usr/include/objectspace\n"
    "CC      ?= cc\n"
    "PYTHON  ?= python3\n"
    "\n"
    "all: $(SO)\n"
    "\n"
    "gen/$(NAME).reg.c: $(NAME).osdi\n"
    "\tmkdir -p gen\n"
    "\t$(PYTHON) $(OSD_SDK)/osdc.py -I $(OSD_SDK)/classes -o gen $(NAME).osdi\n"
    "\n"
    "$(SO): $(NAME).c gen/$(NAME).reg.c\n"
    "\t$(CC) -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fPIC -shared -I$(OSD_INC) -Igen -o $@ $(NAME).c gen/$(NAME).reg.c\n"
    "\n"
    "install: $(SO)\n"
    "\tinstall -d $(HOME)/.config/objectspace/plugins\n"
    "\tinstall -m 755 $(SO) $(HOME)/.config/objectspace/plugins/\n"
    "\n"
    "clean:\n"
    "\trm -rf gen $(SO)\n"
    "\n"
    ".PHONY: all install clean\n";

static int write_file(const char *dir, const char *name, const char *text)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wx");
    if (!f) { fprintf(stderr, "osdctl: %s: %s\n", p, strerror(errno)); return -1; }
    fputs(text, f);
    return fclose(f);
}

static int plugin_cmd(int argc, char **argv)
{
    if (argc < 4 || argc > 5 || strcmp(argv[2], "new")) return usage();
    const char *name = argv[3], *parent = argc == 5 ? argv[4] : "OsdDataFile";
    bool ok = (name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z');
    for (const char *c = name; ok && *c; c++) ok = isalnum((unsigned char)*c) || *c == '_';
    if (!ok || strlen(name) > 60) { fprintf(stderr, "osdctl: a class name is letters, digits and _\n"); return 2; }
    if (mkdir(name, 0755) != 0) { fprintf(stderr, "osdctl: %s: %s\n", name, strerror(errno)); return 1; }
    char osdi[2048], c[4096], *mk = malloc(sizeof plugin_makefile + 128);
    snprintf(osdi, sizeof osdi,
             "# %s: an Objectspace plug-in class (sdk/README.md; the grammar: osdc.py).\n"
             "class %s : %s\n"
             "version 1.0\n"
             "# a property Objectspace keeps for you (its notebook's %s page, setup strings, Class...)\n"
             "property note : text page %s label Note\n"
             "classoverride osdclsQueryTitle\n", name, name, parent, name, name);
    snprintf(c, sizeof c,
             "/* %s (%s.osdi): make, then make install, and log in again. */\n"
             "#include \"%s.h\"\n\n"
             "#include \"osd_classes.h\"\n"
             "#include \"osd_state.h\"\n\n"
             "char *%s__osdclsQueryTitle(osdo_object *self)\n"
             "{\n    (void)self;\n    return osd_strdup(\"%s\");\n}\n\n"
             "osdo_status osdo_plugin_init(uint32_t abi)\n"
             "{\n    if (abi != OSDO_ABI_VERSION) return OSDO_EVERSION;\n    return %s_register();\n}\n",
             name, name, name, name, name, name);
    /* its Makefile: the sample's, for this name */
    const char *at = strstr(plugin_makefile, "NAME    ?= OsdImageFile");
    snprintf(mk, sizeof plugin_makefile + 128, "%.*sNAME    ?= %s%s", (int)(at - plugin_makefile), plugin_makefile, name,
             at + strlen("NAME    ?= OsdImageFile"));
    char f1[128], f2[128];
    snprintf(f1, sizeof f1, "%s.osdi", name);
    snprintf(f2, sizeof f2, "%s.c", name);
    int e = write_file(name, f1, osdi) | write_file(name, f2, c) | write_file(name, "Makefile", mk);
    free(mk);
    if (e) return 1;
    printf("%s/: %s.osdi, %s.c, Makefile - cd %s && make && make install\n", name, name, name, name);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "icon")) return icon_cmd(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "plugin")) return plugin_cmd(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--version")) { printf("osdctl %s\n", OSD_VERSION); return 0; }
    if (argc < 2) return usage();
    const char *op = argv[1];
    cbuf b;
    cb_init(&b);
    int i = 2;
    bool watching = false, finding = false;   /* finding: until the last batch */
    if (!strcmp(op, "ping") || !strcmp(op, "setup") || !strcmp(op, "stats") || !strcmp(op, "flush") ||
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
    } else if (!strcmp(op, "restore-defaults")) {
        if (argc > 3) return usage();
        cb_map(&b, argc == 3 ? 3 : 2);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, "restore_defaults");
        if (argc == 3) { cb_text(&b, "desktop"); cb_text(&b, argv[2]); }
    } else if (!strcmp(op, "vmove")) {
        if (argc < 4 || argc > 5) return usage();
        cb_map(&b, 5);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "target"); cb_text(&b, argv[2]);
        cb_text(&b, "folder"); cb_text(&b, argv[3]);
        cb_text(&b, "copy"); cb_bool(&b, argc == 5 && !strcmp(argv[4], "copy"));
    } else if (!strcmp(op, "say")) {   /* a message on the desktop (scripts, timers) */
        bool stay = argc == 4 && !strcmp(argv[2], "--stay");
        if (argc != 3 && !stay) return usage();
        cb_map(&b, 4);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "text"); cb_text(&b, argv[argc - 1]);
        cb_text(&b, "stay"); cb_bool(&b, stay);
    } else if (!strcmp(op, "layout") || !strcmp(op, "arrange")) {
        if (argc != 3) return usage();
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "target"); cb_text(&b, argv[2]);
    } else if (!strcmp(op, "place")) {
        if (argc < 6 || (argc - 3) % 3) return usage();
        size_t n = (size_t)(argc - 3) / 3;
        cb_map(&b, 4);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "folder"); cb_text(&b, argv[2]);
        cb_text(&b, "items");
        cb_array(&b, n);
        for (size_t i = 0; i < n; i++) {
            cb_map(&b, 3);
            cb_text(&b, "target"); cb_text(&b, argv[3 + 3 * i]);
            cb_text(&b, "x"); cb_int(&b, atoll(argv[4 + 3 * i]));
            cb_text(&b, "y"); cb_int(&b, atoll(argv[5 + 3 * i]));
        }
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
    } else if (!strcmp(op, "get") || !strcmp(op, "set")) {
        /* get TARGET NAME | set TARGET 'key=value;key=value' (declared properties) */
        if (argc != 4) return usage();
        cb_map(&b, 4);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "target"); cb_text(&b, argv[2]);
        cb_text(&b, !strcmp(op, "get") ? "name" : "setup"); cb_text(&b, argv[3]);
    } else if (!strcmp(op, "reset")) {
        /* reset TARGET 'NAME;NAME': their own values removed (the chain's show) */
        if (argc != 4) return usage();
        cb_map(&b, 4);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, op);
        cb_text(&b, "target"); cb_text(&b, argv[2]);
        cb_text(&b, "names"); cb_text(&b, argv[3]);
    } else if (!strcmp(op, "props")) {
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
    } else if (!strcmp(op, "find")) {
        /* find FOLDER PATTERN [sub] [KIND] [LIMIT]: the reply, then each batch until the last */
        if (argc < 4) return usage();
        bool sub = false;
        const char *kind = "any";
        long long limit = 1000;
        for (int k = 4; k < argc; k++) {
            if (!strcmp(argv[k], "sub")) sub = true;
            else if (!strcmp(argv[k], "any") || !strcmp(argv[k], "folder") || !strcmp(argv[k], "file") || !strcmp(argv[k], "program")) kind = argv[k];
            else if (atoll(argv[k]) > 0) limit = atoll(argv[k]);
            else return usage();
        }
        cb_map(&b, 7);
        cb_text(&b, "id"); cb_int(&b, 1);
        cb_text(&b, "op"); cb_text(&b, "find");
        cb_text(&b, "folder"); cb_text(&b, argv[2]);
        cb_text(&b, "name"); cb_text(&b, argv[3]);
        cb_text(&b, "subfolders"); cb_bool(&b, sub);
        cb_text(&b, "kind"); cb_text(&b, kind);
        cb_text(&b, "limit"); cb_int(&b, limit);
        watching = finding = true;
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
        fprintf(stderr, "osdctl: connection failed\n");
        cb_free(&b);
        return 1;
    }
    uint32_t n = (uint32_t)hdr[0] << 24 | (uint32_t)hdr[1] << 16 | (uint32_t)hdr[2] << 8 | hdr[3];
    unsigned char *body = n <= (1u << 24) ? malloc(n ? n : 1) : NULL;
    cv reply;
    if (!body || io_all(fd, body, n, false) || cv_decode(body, n, &reply)) {
        fprintf(stderr, "osdctl: bad reply\n");
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
    while (watching && !(finding && !(cv_get(&reply, "ok") && cv_get(&reply, "ok")->b))) {
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
        const cv *done = cv_get(&e, "done");
        bool last = finding && done && done->k == CV_BOOL && done->b;
        cv_free(&e);
        free(ev);
        if (last) break;
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
