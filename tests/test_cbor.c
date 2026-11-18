/* Tests for the CBOR codec: round trips, RFC 8949 vectors, hostile input. */
#include <stdlib.h>
#include <string.h>

#include "cbor.h"
#include "check.h"

static void json_of(const cv *v, char *out, size_t n)
{
    cbuf j;
    cb_init(&j);
    cv_json(v, &j);
    snprintf(out, n, "%.*s", (int)j.n, (const char *)j.d);
    cb_free(&j);
}

TEST(roundtrip)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 6);
    cb_text(&b, "id"); cb_int(&b, 7);
    cb_text(&b, "op"); cb_text(&b, "call");
    cb_text(&b, "neg"); cb_int(&b, -1000000);
    cb_text(&b, "big"); cb_uint(&b, 5000000000ull);
    cb_text(&b, "args"); cb_array(&b, 4); cb_bool(&b, true); cb_null(&b); cb_double(&b, 2.5); cb_text(&b, "x\"y");
    cb_text(&b, "obj"); cb_map(&b, 1); cb_text(&b, "obj"); cb_text(&b, "/tmp/a b");
    cv v;
    CHECK(!b.oom && cv_decode(b.d, b.n, &v) == 0);
    int64_t i;
    CHECK(cv_int(&v, "id", &i) && i == 7);
    CHECK(cv_int(&v, "neg", &i) && i == -1000000);
    CHECK(cv_int(&v, "big", &i) && i == 5000000000ll);
    CHECK_STR(cv_text(&v, "op"), "call");
    char js[512];
    json_of(cv_get(&v, "args"), js, sizeof js);
    CHECK_STR(js, "[true,null,2.5,\"x\\\"y\"]");
    CHECK_STR(cv_text(cv_get(&v, "obj"), "obj"), "/tmp/a b");
    cv_free(&v);
    cb_free(&b);
}

TEST(rfc_vectors)
{
    /* Encodings from RFC 8949 Appendix A. */
    struct { const char *hex; const char *json; } t[] = {
        { "00", "0" }, { "17", "23" }, { "1818", "24" }, { "1903e8", "1000" },
        { "1b000000e8d4a51000", "1000000000000" }, { "20", "-1" }, { "3903e7", "-1000" },
        { "f4", "false" }, { "f5", "true" }, { "f6", "null" }, { "6161", "\"a\"" },
        { "83010203", "[1,2,3]" }, { "a201020304", "{\"?\":2,\"?\":4}" },
        { "a26161016162820203", "{\"a\":1,\"b\":[2,3]}" },
    };
    for (size_t k = 0; k < sizeof t / sizeof t[0]; k++) {
        unsigned char buf[64];
        size_t n = strlen(t[k].hex) / 2;
        for (size_t j = 0; j < n; j++) sscanf(t[k].hex + 2 * j, "%2hhx", &buf[j]);
        cv v;
        char js[128] = "";
        if (cv_decode(buf, n, &v) == 0) { json_of(&v, js, sizeof js); cv_free(&v); }
        CHECK_STR(js, t[k].json);
    }
}

TEST(hostile_input)
{
    /* Truncations, absurd lengths, deep nesting, trailing bytes, tags. */
    const unsigned char huge_len[] = { 0x7b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    const unsigned char huge_arr[] = { 0x9a, 0xff, 0xff, 0xff, 0xff };
    const unsigned char trailing[] = { 0x01, 0x02 };
    const unsigned char tag[] = { 0xc1, 0x01 };
    const unsigned char trunc_text[] = { 0x65, 'a', 'b' };
    cv v;
    CHECK(cv_decode(huge_len, sizeof huge_len, &v) != 0);
    CHECK(cv_decode(huge_arr, sizeof huge_arr, &v) != 0);
    CHECK(cv_decode(trailing, sizeof trailing, &v) != 0);
    CHECK(cv_decode(tag, sizeof tag, &v) != 0);
    CHECK(cv_decode(trunc_text, sizeof trunc_text, &v) != 0);
    unsigned char deep[100];
    memset(deep, 0x81, sizeof deep);   /* [[[[...]]]] without an end */
    CHECK(cv_decode(deep, sizeof deep, &v) != 0);
    /* Random bytes never crash (ASan/UBSan watch this). */
    srand(1);
    unsigned char rnd[64];
    int decoded = 0;
    for (int round = 0; round < 20000; round++) {
        size_t n = (size_t)(rand() % (int)sizeof rnd) + 1;
        for (size_t i = 0; i < n; i++) rnd[i] = (unsigned char)rand();
        if (cv_decode(rnd, n, &v) == 0) { decoded++; cv_free(&v); }
    }
    CHECK(decoded >= 0);
}

int main(void)
{
    RUN(roundtrip);
    RUN(rfc_vectors);
    RUN(hostile_input);
    return CHECK_SUMMARY();
}
