/* Tests for starting files with their default application: Exec lines,
 * the handler lookup, and lock files. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "launch.h"

/* What launch.c uses from the rest of the service. */
bool osd_plugins_is_host(pid_t pid) { (void)pid; return false; }
const char *osd_mime_for_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && !strcmp(dot, ".txt") ? "text/plain" : dot && !strcmp(dot, ".png") ? "image/png" : NULL;
}
int osdo_loop_watch_fd(osdo_loop *l, int fd, short ev, osdo_fd_fn fn, void *arg) { (void)l; (void)fd; (void)ev; (void)fn; (void)arg; return 0; }
void osdo_loop_unwatch_fd(osdo_loop *l, int fd) { (void)l; (void)fd; }

static char dir[4096];

static void put(const char *rel, const char *text)
{
    char p[4200];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    for (char *s = p + strlen(dir) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(p, 0755); *s = '/'; }
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
}

static char *joined(char **argv)
{
    static char out[4096];
    out[0] = 0;
    for (int i = 0; argv[i]; i++) {
        if (i) strcat(out, "|");
        strcat(out, argv[i]);
    }
    return out;
}

TEST(exec_lines)
{
    char buf[1024], *argv[16];
    CHECK(osd_split_exec("gedit %U", "/a b/c.txt", buf, sizeof buf, argv, 16) == 2);
    CHECK_STR(joined(argv), "gedit|/a b/c.txt");
    osd_split_exec("app --file=%f -x", "/f", buf, sizeof buf, argv, 16);
    CHECK_STR(joined(argv), "app|--file=/f|-x");
    osd_split_exec("viewer", "/f", buf, sizeof buf, argv, 16);        /* no field code: appended */
    CHECK_STR(joined(argv), "viewer|/f");
    osd_split_exec("prog %u", NULL, buf, sizeof buf, argv, 16);       /* a program object: no file */
    CHECK_STR(joined(argv), "prog");
    osd_split_exec("prog %i %c %k --x", "/f", buf, sizeof buf, argv, 16);
    CHECK_STR(joined(argv), "prog|--x|/f");
    osd_split_exec("sh -c \"echo 100%% done\" \"\"", NULL, buf, sizeof buf, argv, 16);
    CHECK_STR(joined(argv), "sh|-c|echo 100% done|");
    osd_split_exec("flatpak run --file-forwarding org.x.App @@u %U @@", "/f", buf, sizeof buf, argv, 16);
    CHECK_STR(joined(argv), "flatpak|run|--file-forwarding|org.x.App|@@u|/f|@@");
}

TEST(default_handler)
{
    char e[4300];
    snprintf(e, sizeof e, "%s/config", dir); setenv("XDG_CONFIG_HOME", e, 1);
    snprintf(e, sizeof e, "%s/sys", dir); setenv("XDG_CONFIG_DIRS", e, 1);
    snprintf(e, sizeof e, "%s/data", dir); setenv("XDG_DATA_HOME", e, 1);
    snprintf(e, sizeof e, "%s/share", dir); setenv("XDG_DATA_DIRS", e, 1);
    setenv("XDG_CURRENT_DESKTOP", "Test:GNOME", 1);
    put("sys/x", "");
    put("data/applications/plain.desktop", "[Desktop Entry]\nExec=plain %f\n");
    put("data/applications/mine.desktop", "[Desktop Entry]\nName=Mine\nExec=mine --open %F\n");
    put("share/applications/vendor/img.desktop", "[Desktop Entry]\nExec=img %u\n");
    put("share/applications/term.desktop", "[Desktop Entry]\nExec=vim %f\nTerminal=true\n");
    put("config/mimeapps.list", "[Default Applications]\ntext/plain=plain.desktop\n");
    /* the desktop-specific list comes first; a missing app is skipped */
    put("config/gnome-mimeapps.list", "[Added Associations]\ntext/plain=x.desktop\n[Default Applications]\ntext/plain=gone.desktop;mine.desktop\n");
    put("share/applications/mimeinfo.cache", "[MIME Cache]\nimage/png=vendor-img.desktop;\n");
    char buf[1024], *argv[16];
    CHECK(osd_handler_argv("/d/a.txt", buf, sizeof buf, argv, 16) == 3);
    CHECK_STR(joined(argv), "mine|--open|/d/a.txt");
    CHECK(osd_handler_argv("/d/a.png", buf, sizeof buf, argv, 16) == 2);   /* from the cache, in a vendor folder */
    CHECK_STR(joined(argv), "img|/d/a.png");
    CHECK(osd_handler_argv("/d/a.unknown", buf, sizeof buf, argv, 16) == 0);
    put("config/gnome-mimeapps.list", "[Default Applications]\ntext/plain=term.desktop\n");
    const char *path0 = getenv("PATH");
    char saved[4096];
    snprintf(saved, sizeof saved, "%s", path0 ? path0 : "");
    setenv("OSD_TERMINAL", "", 1);
    setenv("PATH", "/nonexistent", 1);
    CHECK(osd_handler_argv("/d/a.txt", buf, sizeof buf, argv, 16) == 0);    /* a terminal program, no terminal: xdg-open */
    setenv("PATH", saved, 1);
}

TEST(lock_files)
{
    char p[4200];
    put("l/report.ods", "x");
    snprintf(p, sizeof p, "%s/l/report.ods", dir);
    CHECK(!osd_lock_file_present(p));
    put("l/.~lock.report.ods#", "x");
    CHECK(osd_lock_file_present(p));
    put("l/notes.txt", "x");
    put("l/.notes.txt.swp", "x");
    snprintf(p, sizeof p, "%s/l/notes.txt", dir);
    CHECK(osd_lock_file_present(p));
    put("l/a.org", "x");
    snprintf(p, sizeof p, "%s/l/.#a.org", dir);
    CHECK(symlink("me@host.1:1", p) == 0);
    snprintf(p, sizeof p, "%s/l/a.org", dir);
    CHECK(osd_lock_file_present(p));
    put("l/~$port.docx", "x");   /* Office drops two characters of long names */
    snprintf(p, sizeof p, "%s/l/Report.docx", dir);
    CHECK(osd_lock_file_present(p));
    CHECK(osd_is_lock_file(".~lock.report.ods#") && osd_is_lock_file(".notes.txt.swp") && osd_is_lock_file(".#a.org"));
    CHECK(!osd_is_lock_file("report.ods") && !osd_is_lock_file(".bashrc") && !osd_is_lock_file(".swp"));
}

TEST(terminals)
{
    /* text-mode programs: binaries without window libraries, and scripts */
    CHECK(osd_program_needs_terminal("/usr/bin/true"));
    if (access("/usr/bin/vi", R_OK) == 0)   /* (not in a minimal build root) */
        CHECK(osd_program_needs_terminal("/usr/bin/vi"));
    CHECK(!osd_program_needs_terminal("/usr/bin/ptyxis"));   /* links GTK */
    put("t/s.sh", "#!/bin/sh\n");
    char p[4200];
    snprintf(p, sizeof p, "%s/t/s.sh", dir);
    CHECK(osd_program_needs_terminal(p));
    char *argv[16] = { "vim", "/f", NULL };
    setenv("OSD_TERMINAL", "myterm --hold -e", 1);
    CHECK(osd_terminal_wrap(argv, 2, 16, "Vim", false) == 5);
    CHECK_STR(joined(argv), "myterm|--hold|-e|vim|/f");
    char *argv2[16] = { "vim", NULL };
    setenv("OSD_TERMINAL", "", 1);
    setenv("PATH", dir, 1);   /* no terminal installed */
    CHECK(osd_terminal_wrap(argv2, 1, 16, NULL, false) == 0);
    put("ptyxis", "");
    snprintf(p, sizeof p, "%s/ptyxis", dir);
    chmod(p, 0755);
    CHECK(osd_terminal_wrap(argv2, 1, 16, NULL, false) == 4);
    CHECK_STR(joined(argv2), "ptyxis|-s|--|vim");
    /* a Terminal=true default application comes wrapped */
    put("data/applications/vimish.desktop", "[Desktop Entry]\nExec=vim %f\nTerminal=true\n");
    put("config/gnome-mimeapps.list", "[Default Applications]\ntext/plain=vimish.desktop\n");
    char buf[1024], *a3[16];
    CHECK(osd_handler_argv("/d/a.txt", buf, sizeof buf, a3, 16) == 5);
    CHECK_STR(joined(a3), "ptyxis|-s|--|vim|/d/a.txt");
    /* Close window on exit off with another terminal: through sh */
    char *a4[16] = { "vim", NULL };
    CHECK(osd_terminal_wrap(a4, 1, 16, NULL, true) == 8);
    CHECK(!strcmp(a4[0], "ptyxis") && !strcmp(a4[3], "sh") && !strcmp(a4[7], "vim"));
    /* WPS Terminal (0.4.1: the default) once it is there: titled by the
     * object, --hold for Close window on exit off */
    put("wps-terminal", "");
    snprintf(p, sizeof p, "%s/wps-terminal", dir);
    chmod(p, 0755);
    char *a5[16] = { "vim", "/f", NULL };
    CHECK(osd_terminal_wrap(a5, 2, 16, "Notes", true) == 7);
    CHECK_STR(joined(a5), "wps-terminal|--title|Notes|--hold|--|vim|/f");
    CHECK_STR(osd_terminal_now(), "wps-terminal");
    osd_terminal_choose("system default", "");   /* another choice: the list again */
    char *a6[16] = { "vim", NULL };
    CHECK(osd_terminal_wrap(a6, 1, 16, NULL, false) == 4);
    CHECK_STR(joined(a6), "ptyxis|-s|--|vim");
    osd_terminal_choose("", "");
}

int main(int argc, char **argv)
{
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "/tmp/wps-test-launch");
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd) != 0) return 2;
    RUN(exec_lines);
    RUN(default_handler);
    RUN(lock_files);
    RUN(terminals);
    return CHECK_SUMMARY();
}
