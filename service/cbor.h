/*
 * A small CBOR (RFC 8949) encoder and decoder for the service protocol.
 * Supports what the protocol uses: integers, floats, text, byte strings,
 * arrays, maps, booleans and null. No tags, no indefinite lengths.
 *
 * Messages on the socket are framed as a 4-byte big-endian length followed
 * by one CBOR item (a map).
 */
#ifndef WPS_CBOR_H
#define WPS_CBOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- encoding into a growable buffer */
typedef struct {
    unsigned char *d;
    size_t n, cap;
    bool oom;
} cbuf;

void cb_init(cbuf *b);
void cb_free(cbuf *b);
void cb_uint(cbuf *b, uint64_t v);
void cb_int(cbuf *b, int64_t v);
void cb_double(cbuf *b, double v);
void cb_text(cbuf *b, const char *s);
void cb_textn(cbuf *b, const char *s, size_t n);
void cb_bytes(cbuf *b, const void *p, size_t n);
void cb_bool(cbuf *b, bool v);
void cb_null(cbuf *b);
void cb_array(cbuf *b, size_t n);   /* followed by n items */
void cb_map(cbuf *b, size_t n);     /* followed by n key/value pairs */

/* ---- decoding into a tree */
typedef enum { CV_NULL, CV_BOOL, CV_INT, CV_FLOAT, CV_TEXT, CV_BYTES, CV_ARRAY, CV_MAP } cv_kind;

typedef struct cv {
    cv_kind k;
    bool b;
    int64_t i;
    double d;
    char *s;            /* text (NUL-terminated) or bytes */
    size_t len;         /* text/bytes length */
    struct cv *items;   /* array items; for maps: key, value, key, value, ... */
    size_t n;           /* array length, or number of map pairs */
    size_t nitems;      /* allocated item slots (freeing covers partial decodes) */
} cv;

/* 0 on success. Limits nesting depth and sizes; never reads past `len`. */
int cv_decode(const void *buf, size_t len, cv *out);
void cv_free(cv *v);
const cv *cv_get(const cv *map, const char *key);     /* NULL if absent */
const char *cv_text(const cv *map, const char *key);  /* NULL if absent or not text */
bool cv_int(const cv *map, const char *key, int64_t *out);

/* Append a JSON rendering of `v` (for wpsctl and logs). */
void cv_json(const cv *v, cbuf *out);

#endif
