#define _GNU_SOURCE
#include "WPComputer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "wps_classes.h"
#include "wps_menu_ids.h"

char *wps_menu_without(char *menu, const int *ids, int n);

char *WPComputer__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    static const int gone[] = { WPS_MI_CREATE_ANOTHER };
    return wps_menu_without(WPComputer_parent_wpModifyPopupMenu(self, menu), gone, 1);
}

static void os_name(char *out, size_t n)
{
    snprintf(out, n, "Linux");
    FILE *f = fopen("/etc/os-release", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, "PRETTY_NAME=", 12)) continue;
        char *v = line + 12;
        v[strcspn(v, "\r\n")] = 0;
        if (*v == '"') { v++; char *q = strrchr(v, '"'); if (q) *q = 0; }
        snprintf(out, n, "%s", v);
    }
    if (f) fclose(f);
}

static void cpu_name(char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f) && !out[0]) {
        if (strncmp(line, "model name", 10)) continue;
        char *v = strchr(line, ':');
        if (!v) continue;
        v++;
        while (*v == ' ' || *v == '\t') v++;
        v[strcspn(v, "\r\n")] = 0;
        snprintf(out, n, "%s", v);
    }
    if (f) fclose(f);
}

/* The machine, read only: host, system, kernel, processors, memory. */
char *WPComputer__wpAddSettingsPages(wpo_object *self, const char *pages)
{
    char *base = WPComputer_parent_wpAddSettingsPages(self, pages);
    char host[256] = "", os[256], cpu[256], mem[64] = "", procs[300];
    gethostname(host, sizeof host - 1);
    os_name(os, sizeof os);
    cpu_name(cpu, sizeof cpu);
    struct utsname u;
    if (uname(&u) != 0) memset(&u, 0, sizeof u);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    snprintf(procs, sizeof procs, "%ld%s%s", ncpu, cpu[0] ? " x " : "", cpu);
    struct sysinfo si;
    if (sysinfo(&si) == 0) snprintf(mem, sizeof mem, "%.1f GB", (double)si.totalram * si.mem_unit / 1e9);
    char page[1600];
    snprintf(page, sizeof page,
             "page\tComputer\n"
             "field\thost\tinfo\tName\t%s\t\n"
             "field\tos\tinfo\tSystem\t%s\t\n"
             "field\tkernel\tinfo\tKernel\t%s %s\t\n"
             "field\tcpu\tinfo\tProcessors\t%s\t\n"
             "field\tmemory\tinfo\tMemory\t%s\t\n",
             host, os, u.release, u.machine, procs, mem);
    /* first, as OS/2's notebooks begin with the object's own page */
    char *r = wps_concat(page, base ? base : "");
    free(base);
    return r;
}

char *WPComputer__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Computer");
}
