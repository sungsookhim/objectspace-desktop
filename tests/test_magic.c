/* A file's type from its contents (classes/osd_magic.h), with the system's
 * shared-mime-info magic rules. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "osd_magic.h"

static char dir[256];

static const char *sniff(const char *name, const void *data, size_t n)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (n) fwrite(data, 1, n, f);
    fclose(f);
    static char out[128];
    const char *t = osd_mime_sniff(p);
    snprintf(out, sizeof out, "%s", t ? t : "(none)");
    return out;
}

TEST(types)
{
    static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
    CHECK(!strcmp(sniff("picture", png, sizeof png), "image/png"));
    CHECK(!strcmp(sniff("document", "%PDF-1.7\n%\xe2\xe3\xcf\xd3\n", 15), "application/pdf"));
    const char *sh = sniff("script", "#!/bin/sh\necho hi\n", 18);   /* (newer shared-mime-info: text/, as GNOME says) */
    CHECK(!strcmp(sh, "text/x-shellscript") || !strcmp(sh, "application/x-shellscript"));
}

TEST(text_and_others)
{
    CHECK(!strcmp(sniff("Data File", "", 0), "text/plain"));   /* empty: text */
    CHECK(!strcmp(sniff("notes", "Some words, and more.\n", 22), "text/plain"));
    static const unsigned char bin[] = { 0, 1, 2, 3, 0xfe, 0xff, 0, 7 };
    CHECK(!strcmp(sniff("blob", bin, sizeof bin), "(none)"));
    CHECK(osd_mime_sniff("/nonexistent/x") == NULL);
}

int main(void)
{
    snprintf(dir, sizeof dir, "/tmp/osd-test-magic-%d", (int)getpid());
    mkdir(dir, 0755);
    if (access("/usr/share/mime/magic", R_OK) != 0) { printf("no /usr/share/mime/magic: skipped\n"); return 0; }
    RUN(types);
    RUN(text_and_others);
    return CHECK_SUMMARY();
}
