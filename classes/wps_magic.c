/* A file's type from its contents (wps_magic.h): shared-mime-info's magic
 * rules (/usr/share/mime/magic), read once, matched against the file's
 * first 4 KB; with none matching, an empty file or text (no zero bytes, no
 * control characters but whitespace) is text/plain. Used by the service
 * (WPDataFile) and the GUI (mime.c) alike, for names that say nothing. */
#define _GNU_SOURCE
#include "wps_magic.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct rule {
    int indent;
    uint32_t start, range;
    uint16_t len;
    unsigned char *value, *mask;
};
struct section {
    int priority;
    char *type;
    struct rule *r;
    size_t n;
};
static struct section *sections;
static size_t nsections;
static bool loaded;

static void load(const char *path)
{
    loaded = true;
    FILE *f = fopen(path ? path : "/usr/share/mime/magic", "rb");
    if (!f) return;
    char head[12];
    if (fread(head, 1, 12, f) != 12 || memcmp(head, "MIME-Magic\0\n", 12)) { fclose(f); return; }
    struct section *cur = NULL;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '[') {   /* [priority:type] */
            char line[256];
            if (!fgets(line, sizeof line, f)) break;
            char *colon = strchr(line, ':'), *close = strchr(line, ']');
            if (!colon || !close) { cur = NULL; continue; }
            *close = 0;
            sections = realloc(sections, (nsections + 1) * sizeof *sections);
            cur = &sections[nsections++];
            memset(cur, 0, sizeof *cur);
            cur->priority = atoi(line);
            cur->type = strdup(colon + 1);
            continue;
        }
        /* [indent]>start=<len:2 BE><value>[&mask][~word][+range]\n */
        int indent = 0;
        ungetc(c, f);
        while ((c = fgetc(f)) != EOF && c >= '0' && c <= '9') indent = indent * 10 + (c - '0');
        if (c != '>') break;
        uint32_t start = 0;
        while ((c = fgetc(f)) != EOF && c >= '0' && c <= '9') start = start * 10 + (uint32_t)(c - '0');
        if (c != '=') break;
        int hi = fgetc(f), lo = fgetc(f);
        if (hi == EOF || lo == EOF) break;
        uint16_t len = (uint16_t)((hi << 8) | lo);
        unsigned char *value = malloc(len ? len : 1), *mask = NULL;
        if (fread(value, 1, len, f) != len) { free(value); break; }
        uint32_t range = 1;
        while ((c = fgetc(f)) != EOF && c != '\n') {
            if (c == '&') {
                mask = malloc(len ? len : 1);
                if (fread(mask, 1, len, f) != len) break;
            } else if (c == '~') {
                while ((c = fgetc(f)) != EOF && c >= '0' && c <= '9') {}
                ungetc(c, f);
            } else if (c == '+') {
                range = 0;
                while ((c = fgetc(f)) != EOF && c >= '0' && c <= '9') range = range * 10 + (uint32_t)(c - '0');
                ungetc(c, f);
            }
        }
        if (!cur) { free(value); free(mask); continue; }
        cur->r = realloc(cur->r, (cur->n + 1) * sizeof *cur->r);
        cur->r[cur->n++] = (struct rule){ indent, start, range ? range : 1, len, value, mask };
    }
    fclose(f);
}

static bool rule_matches(const struct rule *r, const unsigned char *buf, size_t n)
{
    for (uint32_t off = r->start; off < r->start + r->range && off + r->len <= n; off++) {
        size_t k = 0;
        for (; k < r->len; k++) {
            unsigned char b = buf[off + k], v = r->value[k];
            if (r->mask) { b &= r->mask[k]; v &= r->mask[k]; }
            if (b != v) break;
        }
        if (k == r->len) return true;
    }
    return false;
}

/* Rules from i on at `indent`: does one match, with (if it has any) one of
 * its nested rules too? */
static bool rules_match(const struct section *s, size_t *i, int indent, const unsigned char *buf, size_t n)
{
    bool any = false;
    while (*i < s->n && s->r[*i].indent == indent) {
        bool m = rule_matches(&s->r[*i], buf, n);
        (*i)++;
        if (*i < s->n && s->r[*i].indent > indent) {
            bool child = rules_match(s, i, indent + 1, buf, n);
            m = m && child;
            while (*i < s->n && s->r[*i].indent > indent) (*i)++;
        }
        if (m) any = true;
    }
    return any;
}

const char *wps_magic_type(const unsigned char *buf, size_t n)
{
    if (!loaded) load(getenv("WPS_MIME_MAGIC"));
    const char *best = NULL;
    int bestp = -1;
    for (size_t s = 0; s < nsections; s++) {
        if (sections[s].priority <= bestp) continue;
        size_t i = 0;
        if (rules_match(&sections[s], &i, 0, buf, n)) { best = sections[s].type; bestp = sections[s].priority; }
    }
    return best;
}

static bool is_text(const unsigned char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (buf[i] == 0 || (buf[i] < 0x20 && !strchr("\t\n\r\f\v\x1b", buf[i]))) return false;
    return true;
}

const char *wps_mime_sniff(const char *path)
{
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) : -1;
    if (fd < 0) return NULL;
    struct stat st;
    unsigned char buf[4096];
    ssize_t n = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? read(fd, buf, sizeof buf) : -1;
    close(fd);
    if (n < 0) return NULL;
    if (n == 0) return "text/plain";   /* an empty file (a new Data File): text, as GNOME has it */
    const char *t = wps_magic_type(buf, (size_t)n);
    if (t) return t;
    return is_text(buf, (size_t)n) ? "text/plain" : NULL;
}
