/* Test plug-in for M5: menu item, a slow path, a settings change, and a
 * probe of the sandbox it runs in. */
#include "TestFolder.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "wps_classes.h"

char *TestFolder__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    char *title = WPObject_wpQueryTitle(self);
    if (title && !strcmp(title, "SLOW")) sleep(60);   /* a hung plug-in */
    free(title);
    char *base = TestFolder_parent_wpModifyPopupMenu(self, menu);
    char *r = wps_concat(base, "90\tFrom the plug-in\n");
    free(base);
    return r;
}

bool TestFolder__wpStamp(wpo_object *self)
{
    return WPObject_wpSetTitle(self, "Stamped by the plug-in");
}

char *TestFolder__wpSandboxProbe(wpo_object *self, const char *data_dir)
{
    (void)self;
    char out[512], p[4096];
    const char *home = getenv("HOME");
    snprintf(p, sizeof p, "%s", home ? home : "/root");
    int h = open(p, O_RDONLY | O_DIRECTORY);
    int he = h < 0 ? errno : 0;
    if (h >= 0) close(h);
    int s = socket(AF_INET, SOCK_STREAM, 0), ne = 0;
    if (s >= 0) {
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(80) };
        inet_pton(AF_INET, "1.1.1.1", &a.sin_addr);
        ne = connect(s, (struct sockaddr *)&a, sizeof a) < 0 ? errno : 0;
        close(s);
    } else {
        ne = errno;
    }
    snprintf(p, sizeof p, "%s/probe.txt", data_dir);
    int d = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int de = d < 0 ? errno : 0;
    if (d >= 0) close(d);
    snprintf(out, sizeof out, "home=%s net=%s data=%s", he ? strerror(he) : "readable",
             ne ? strerror(ne) : "connected", de ? strerror(de) : "writable");
    return wps_strdup(out);
}

wpo_status wpo_plugin_init(uint32_t abi)
{
    if (abi != WPO_ABI_VERSION) return WPO_EVERSION;
    wpo_status st = TestFolder_register();
    if (st == WPO_OK) st = wpo_replace_class("WPFolder", "TestFolder");
    return st;
}
