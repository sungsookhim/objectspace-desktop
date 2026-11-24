/* Startup (startup.h). */
#define _GNU_SOURCE
#include "startup.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void osd_startup_dir(char *out, size_t n)
{
    const char *xc = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xc && *xc) snprintf(out, n, "%s/autostart", xc);
    else snprintf(out, n, "%s/.config/autostart", home ? home : "");
}

static bool ends(const char *s, const char *suf)
{
    size_t l = strlen(s), m = strlen(suf);
    return l >= m && !strcmp(s + l - m, suf);
}

bool osd_startup_is_entry(const char *path)
{
    char dir[PATH_MAX];
    osd_startup_dir(dir, sizeof dir);
    size_t l = strlen(dir);
    return path && !strncmp(path, dir, l) && path[l] == '/' && !strchr(path + l + 1, '/') && ends(path, ".desktop");
}

static char *slurp(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 1 << 20) { close(fd); errno = EINVAL; return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    ssize_t r = b ? read(fd, b, (size_t)st.st_size) : -1;
    close(fd);
    if (r < 0) { free(b); return NULL; }
    b[r] = 0;
    if (len) *len = (size_t)r;
    return b;
}

/* A key of [Desktop Entry] ("" if absent). */
static void entry_value(const char *text, const char *key, char *out, size_t n)
{
    out[0] = 0;
    bool in = false;
    size_t kl = strlen(key);
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (line[0] == '[') in = len == 15 && !strncmp(line, "[Desktop Entry]", 15);
        else if (in && len > kl && !strncmp(line, key, kl) && line[kl] == '=') {
            snprintf(out, n, "%.*s", (int)(len - kl - 1), line + kl + 1);
            return;
        }
        line = end ? end + 1 : NULL;
    }
}

static bool has_key(const char *text, const char *key)
{
    bool in = false;
    size_t kl = strlen(key);
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (line[0] == '[') in = len == 15 && !strncmp(line, "[Desktop Entry]", 15);
        else if (in && len > kl && !strncmp(line, key, kl) && line[kl] == '=') return true;
        line = end ? end + 1 : NULL;
    }
    return false;
}

bool osd_startup_enabled(const char *path)
{
    char *t = slurp(path, NULL);
    if (!t) return false;
    char h[16], g[16];
    entry_value(t, "Hidden", h, sizeof h);
    entry_value(t, "X-GNOME-Autostart-enabled", g, sizeof g);
    free(t);
    return strcmp(h, "true") && strcmp(g, "false");
}

/* The text with keys of [Desktop Entry] set (value NULL: taken out); new
 * keys go right after its header. */
static char *with_keys(const char *text, const char *const *keys, const char *const *vals, size_t nk)
{
    size_t cap = strlen(text) + 1024, o = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    bool in = false, done[8] = { 0 };
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        bool skip = false;
        if (line[0] == '[') in = len == 15 && !strncmp(line, "[Desktop Entry]", 15);
        else if (in)
            for (size_t k = 0; k < nk; k++) {
                size_t kl = strlen(keys[k]);
                if (len > kl && !strncmp(line, keys[k], kl) && line[kl] == '=') {
                    skip = true;
                    if (vals[k] && !done[k]) o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", keys[k], vals[k]);
                    done[k] = true;
                }
            }
        if (!skip) o += (size_t)snprintf(out + o, cap - o, "%.*s\n", (int)len, line);
        if (line[0] == '[' && in)
            for (size_t k = 0; k < nk; k++)   /* (placed first: the old lines are dropped) */
                if (vals[k] && !has_key(text, keys[k])) { o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", keys[k], vals[k]); done[k] = true; }
        if (o >= cap) { free(out); return NULL; }
        line = end ? end + 1 : NULL;
    }
    return out;
}

static int write_new(const char *path, const char *text, bool replace)
{
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return errno;
    size_t l = strlen(text);
    bool ok = write(fd, text, l) == (ssize_t)l;
    if (close(fd) != 0) ok = false;
    if (!ok) { int e = errno ? errno : EIO; unlink(tmp); return e; }
    if (replace ? rename(tmp, path) != 0 : link(tmp, path) != 0) { int e = errno; unlink(tmp); return e; }
    if (!replace) unlink(tmp);
    return 0;
}

int osd_startup_set_enabled(const char *path, bool on)
{
    if (!osd_startup_is_entry(path)) return EINVAL;
    char *t = slurp(path, NULL);
    if (!t) return errno;
    static const char *const keys[] = { "Hidden", "X-GNOME-Autostart-enabled" };
    const char *vals[] = { on ? NULL : "true", on ? NULL : "false" };
    char *n = with_keys(t, keys, vals, 2);
    free(t);
    if (!n) return ENOMEM;
    int e = write_new(path, n, true);
    free(n);
    return e;
}

int osd_startup_add(const char *path, char *made, size_t n)
{
    struct stat st;
    if (stat(path, &st) != 0) return errno;
    if (!S_ISREG(st.st_mode)) return ENOEXEC;
    char dir[PATH_MAX], text_buf[PATH_MAX * 2 + 256];
    osd_startup_dir(dir, sizeof dir);
    mkdir(dir, 0755);
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    char *text = NULL;
    char stem[NAME_MAX + 1];
    snprintf(stem, sizeof stem, "%s", base);
    if (ends(path, ".desktop")) {
        char *t = slurp(path, NULL);
        if (!t) return errno;
        char type[32];
        entry_value(t, "Type", type, sizeof type);
        if (strcmp(type, "Application")) { free(t); return ENOEXEC; }
        static const char *const keys[] = { "X-WPS-Startup", "Hidden", "X-GNOME-Autostart-enabled" };
        static const char *const vals[] = { "true", NULL, NULL };
        text = with_keys(t, keys, vals, 3);
        free(t);
        if (!text) return ENOMEM;
        stem[strlen(stem) - 8] = 0;
    } else if (access(path, X_OK) == 0) {
        /* a script or a program: an entry running it (quoted as the spec says) */
        char q[PATH_MAX * 2];
        size_t o = 0;
        q[o++] = '"';
        for (const char *p = path; *p && o < sizeof q - 4; p++) {
            if (strchr("\"`$\\", *p)) q[o++] = '\\';
            q[o++] = *p;
        }
        q[o++] = '"';
        q[o] = 0;
        snprintf(text_buf, sizeof text_buf, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nX-WPS-Startup=true\n", base, q);
        text = strdup(text_buf);
        char *dot = strrchr(stem, '.');
        if (dot && dot != stem) *dot = 0;
    } else {
        return ENOEXEC;
    }
    int e = EEXIST;
    for (int k = 1; k < 100 && e == EEXIST; k++) {
        if (k == 1) snprintf(made, n, "%s/%s.desktop", dir, stem);
        else snprintf(made, n, "%s/%s-%d.desktop", dir, stem, k);
        e = write_new(made, text, false);
    }
    free(text);
    return e;
}
