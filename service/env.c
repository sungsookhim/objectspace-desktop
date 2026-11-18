#define _GNU_SOURCE
#include "env.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "attrs.h"

int wps_mkdirs(const char *path, unsigned mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

static void dir_from(char *out, size_t n, const char *env, const char *xdg, const char *fallback, const char *sub)
{
    const char *v = getenv(env);
    if (v && *v) { snprintf(out, n, "%s", v); return; }
    const char *x = getenv(xdg);
    const char *home = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s%s", x, sub);
    else snprintf(out, n, "%s/%s%s", home ? home : "/tmp", fallback, sub);
}

static int read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *ok = fgets(buf, (int)n, f);
    fclose(f);
    if (!ok) return -1;
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

int wps_env_load(wps_env *e, char *err, size_t errlen)
{
    memset(e, 0, sizeof *e);
    dir_from(e->config_dir, sizeof e->config_dir, "WPS_CONFIG_DIR", "XDG_CONFIG_HOME", ".config", "/wps");
    dir_from(e->state_dir, sizeof e->state_dir, "WPS_STATE_DIR", "XDG_DATA_HOME", ".local/share", "/wps");
    const char *rt = getenv("WPS_RUNTIME_DIR");
    if (!rt || !*rt) rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) snprintf(e->runtime_dir, sizeof e->runtime_dir, "%s", rt);
    else snprintf(e->runtime_dir, sizeof e->runtime_dir, "/tmp/wps-%u", (unsigned)getuid());
    if (wps_mkdirs(e->config_dir, 0700) < 0 || wps_mkdirs(e->state_dir, 0700) < 0 ||
        wps_mkdirs(e->runtime_dir, 0700) < 0) {
        snprintf(err, errlen, "cannot create directories: %s", strerror(errno));
        return -1;
    }

    char path[PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/identity", e->config_dir);
    if (read_line(path, e->identity, sizeof e->identity) < 0 || strlen(e->identity) != WPS_ID_LEN) {
        char id[WPS_ID_LEN + 1];
        wps_new_id(id);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            dprintf(fd, "%s\n", id);
            close(fd);
        }
        /* Re-read: another process may have created it first. */
        if (read_line(path, e->identity, sizeof e->identity) < 0 || strlen(e->identity) != WPS_ID_LEN) {
            snprintf(err, errlen, "cannot read or create %s", path);
            return -1;
        }
    }

    const char *h = getenv("WPS_HOST_ID");
    if (h && *h) snprintf(e->host, sizeof e->host, "%s", h);
    else if (read_line("/etc/machine-id", e->host, sizeof e->host) < 0) gethostname(e->host, sizeof e->host);
    for (char *p = e->host; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-') *p = '_';
    snprintf(e->writer, sizeof e->writer, "%s/%s", e->identity, e->host);
    return 0;
}
