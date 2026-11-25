/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* MD5 for the thumbnail cache's names (gui/shell/md5.c): RFC 1321's test
 * vectors and the Thumbnail Managing Standard's own example. */
#include <string.h>

#include "check.h"
#include "md5.h"

static int same(const char *in, const char *want)
{
    char out[33];
    md5_hex(in, out);
    return !strcmp(out, want);
}

TEST(rfc1321_vectors)
{
    CHECK(same("", "d41d8cd98f00b204e9800998ecf8427e"));
    CHECK(same("a", "0cc175b9c0f1b6a831c399e269772661"));
    CHECK(same("abc", "900150983cd24fb0d6963f7d28e17f72"));
    CHECK(same("message digest", "f96b697d7cb7938d525a2f31aaf161d0"));
    CHECK(same("12345678901234567890123456789012345678901234567890123456789012345678901234567890",
               "57edf4a22be3c955ac49da2e2107b67a"));
}

TEST(the_standards_example)
{
    /* "file:///home/jens/photos/me.png" -> c6ee772d9e49320e97ec29a7eb5b1697.png */
    CHECK(same("file:///home/jens/photos/me.png", "c6ee772d9e49320e97ec29a7eb5b1697"));
}

int main(void)
{
    RUN(rfc1321_vectors);
    RUN(the_standards_example);
    return CHECK_SUMMARY();
}
