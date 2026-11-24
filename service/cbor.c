/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "cbor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 32
#define MAX_ITEMS (1u << 20)

/* ------------------------------------------------------------- encoding */

void cb_init(cbuf *b) { memset(b, 0, sizeof *b); }
void cb_free(cbuf *b) { free(b->d); cb_init(b); }

static void put(cbuf *b, const void *p, size_t n)
{
    if (b->oom) return;
    if (b->n + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 256;
        while (ncap < b->n + n) ncap *= 2;
        unsigned char *nd = realloc(b->d, ncap);
        if (!nd) { b->oom = true; return; }
        b->d = nd;
        b->cap = ncap;
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void head(cbuf *b, unsigned major, uint64_t v)
{
    unsigned char h[9];
    size_t n;
    if (v < 24) { h[0] = (unsigned char)(major << 5 | v); n = 1; }
    else if (v <= 0xff) { h[0] = (unsigned char)(major << 5 | 24); h[1] = (unsigned char)v; n = 2; }
    else if (v <= 0xffff) { h[0] = (unsigned char)(major << 5 | 25); n = 3; }
    else if (v <= 0xffffffffu) { h[0] = (unsigned char)(major << 5 | 26); n = 5; }
    else { h[0] = (unsigned char)(major << 5 | 27); n = 9; }
    if (n > 2)
        for (size_t i = 1; i < n; i++) h[i] = (unsigned char)(v >> (8 * (n - 1 - i)));
    put(b, h, n);
}

void cb_uint(cbuf *b, uint64_t v) { head(b, 0, v); }
void cb_int(cbuf *b, int64_t v) { if (v >= 0) head(b, 0, (uint64_t)v); else head(b, 1, (uint64_t)(-(v + 1))); }
void cb_text(cbuf *b, const char *s) { cb_textn(b, s, strlen(s)); }
void cb_textn(cbuf *b, const char *s, size_t n) { head(b, 3, n); put(b, s, n); }
void cb_bytes(cbuf *b, const void *p, size_t n) { head(b, 2, n); put(b, p, n); }
void cb_bool(cbuf *b, bool v) { unsigned char c = v ? 0xf5 : 0xf4; put(b, &c, 1); }
void cb_null(cbuf *b) { unsigned char c = 0xf6; put(b, &c, 1); }
void cb_array(cbuf *b, size_t n) { head(b, 4, n); }
void cb_map(cbuf *b, size_t n) { head(b, 5, n); }

void cb_double(cbuf *b, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    unsigned char h[9] = { 0xfb };
    for (int i = 0; i < 8; i++) h[1 + i] = (unsigned char)(u >> (8 * (7 - i)));
    put(b, h, 9);
}

/* ------------------------------------------------------------- decoding */

struct rd { const unsigned char *p; size_t len, o; };

static int arg(struct rd *r, unsigned info, uint64_t *v)
{
    if (info < 24) { *v = info; return 0; }
    size_t n = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 0;
    if (!n || r->o + n > r->len) return -1;
    uint64_t x = 0;
    for (size_t i = 0; i < n; i++) x = x << 8 | r->p[r->o++];
    *v = x;
    return 0;
}

static int item(struct rd *r, cv *out, int depth)
{
    memset(out, 0, sizeof *out);
    if (depth > MAX_DEPTH || r->o >= r->len) return -1;
    unsigned char ib = r->p[r->o++];
    unsigned major = ib >> 5, info = ib & 31;
    uint64_t v;
    switch (major) {
    case 0:
        if (arg(r, info, &v) || v > INT64_MAX) return -1;
        out->k = CV_INT; out->i = (int64_t)v; return 0;
    case 1:
        if (arg(r, info, &v) || v > INT64_MAX) return -1;
        out->k = CV_INT; out->i = -1 - (int64_t)v; return 0;
    case 2: case 3:
        if (arg(r, info, &v) || v > r->len - r->o) return -1;
        out->k = major == 2 ? CV_BYTES : CV_TEXT;
        out->s = malloc((size_t)v + 1);
        if (!out->s) return -1;
        memcpy(out->s, r->p + r->o, (size_t)v);
        out->s[v] = 0;
        out->len = (size_t)v;
        r->o += (size_t)v;
        return 0;
    case 4: case 5: {
        if (arg(r, info, &v) || v > MAX_ITEMS) return -1;
        size_t count = major == 4 ? (size_t)v : (size_t)v * 2;
        if (count > r->len - r->o) return -1;   /* each item needs at least a byte */
        out->k = major == 4 ? CV_ARRAY : CV_MAP;
        out->n = (size_t)v;
        out->items = calloc(count ? count : 1, sizeof *out->items);
        if (!out->items) return -1;
        out->nitems = count;   /* unfilled slots stay zeroed, so freeing all is safe */
        for (size_t i = 0; i < count; i++)
            if (item(r, &out->items[i], depth + 1)) return -1;
        return 0;
    }
    case 7:
        if (info == 20 || info == 21) { out->k = CV_BOOL; out->b = info == 21; return 0; }
        if (info == 22 || info == 23) { out->k = CV_NULL; return 0; }
        if (info == 27) {
            if (r->o + 8 > r->len) return -1;
            uint64_t u = 0;
            for (int i = 0; i < 8; i++) u = u << 8 | r->p[r->o++];
            out->k = CV_FLOAT;
            memcpy(&out->d, &u, 8);
            return 0;
        }
        return -1;
    default:
        return -1;   /* tags are not used */
    }
}

int cv_decode(const void *buf, size_t len, cv *out)
{
    struct rd r = { buf, len, 0 };
    if (item(&r, out, 0) || r.o != len) { cv_free(out); return -1; }
    return 0;
}

void cv_free(cv *v)
{
    if (!v) return;
    for (size_t i = 0; i < v->nitems && v->items; i++) cv_free(&v->items[i]);
    free(v->items);
    free(v->s);
    memset(v, 0, sizeof *v);
}

const cv *cv_get(const cv *m, const char *key)
{
    if (!m || m->k != CV_MAP) return NULL;
    for (size_t i = 0; i < m->n; i++)
        if (m->items[2 * i].k == CV_TEXT && strcmp(m->items[2 * i].s, key) == 0) return &m->items[2 * i + 1];
    return NULL;
}

const char *cv_text(const cv *m, const char *key)
{
    const cv *v = cv_get(m, key);
    return v && v->k == CV_TEXT ? v->s : NULL;
}

bool cv_int(const cv *m, const char *key, int64_t *out)
{
    const cv *v = cv_get(m, key);
    if (!v || v->k != CV_INT) return false;
    *out = v->i;
    return true;
}

/* ----------------------------------------------------------------- JSON */

static void jstr(cbuf *o, const char *s, size_t n)
{
    put(o, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; put(o, e, 2); }
        else if (c == '\n') put(o, "\\n", 2);
        else if (c == '\t') put(o, "\\t", 2);
        else if (c < 0x20) { snprintf(e, sizeof e, "\\u%04x", c); put(o, e, 6); }
        else put(o, &s[i], 1);
    }
    put(o, "\"", 1);
}

void cv_json(const cv *v, cbuf *o)
{
    char num[64];
    switch (v->k) {
    case CV_NULL: put(o, "null", 4); break;
    case CV_BOOL: v->b ? put(o, "true", 4) : put(o, "false", 5); break;
    case CV_INT: snprintf(num, sizeof num, "%lld", (long long)v->i); put(o, num, strlen(num)); break;
    case CV_FLOAT: snprintf(num, sizeof num, "%.17g", v->d); put(o, num, strlen(num)); break;
    case CV_TEXT: jstr(o, v->s, v->len); break;
    case CV_BYTES: jstr(o, "<bytes>", 7); break;
    case CV_ARRAY:
        put(o, "[", 1);
        for (size_t i = 0; i < v->n; i++) { if (i) put(o, ",", 1); cv_json(&v->items[i], o); }
        put(o, "]", 1);
        break;
    case CV_MAP:
        put(o, "{", 1);
        for (size_t i = 0; i < v->n; i++) {
            if (i) put(o, ",", 1);
            const cv *k = &v->items[2 * i];
            if (k->k == CV_TEXT) jstr(o, k->s, k->len);
            else jstr(o, "?", 1);
            put(o, ":", 1);
            cv_json(&v->items[2 * i + 1], o);
        }
        put(o, "}", 1);
        break;
    }
}
