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
/* Its Computer page (WPComputer.wpi): what this computer is. */
char *WPComputer__wpQueryInfo(wpo_object *self, const char *which)
{
    (void)self;
    char v[300] = "";
    if (!strcmp(which, "host")) gethostname(v, sizeof v - 1);
    else if (!strcmp(which, "os")) os_name(v, sizeof v);
    else if (!strcmp(which, "kernel")) {
        struct utsname u;
        if (uname(&u) != 0) memset(&u, 0, sizeof u);
        snprintf(v, sizeof v, "%s %s", u.release, u.machine);
    } else if (!strcmp(which, "cpu")) {
        char cpu[256];
        cpu_name(cpu, sizeof cpu);
        snprintf(v, sizeof v, "%ld%s%s", sysconf(_SC_NPROCESSORS_ONLN), cpu[0] ? " x " : "", cpu);
    } else if (!strcmp(which, "memory")) {
        struct sysinfo si;
        if (sysinfo(&si) == 0) snprintf(v, sizeof v, "%.1f GB", (double)si.totalram * si.mem_unit / 1e9);
    }
    return wps_strdup(v);
}

char *WPComputer__wpQueryInfoHost(wpo_object *self) { return WPComputer_wpQueryInfo(self, "host"); }
char *WPComputer__wpQueryInfoOS(wpo_object *self) { return WPComputer_wpQueryInfo(self, "os"); }
char *WPComputer__wpQueryInfoKernel(wpo_object *self) { return WPComputer_wpQueryInfo(self, "kernel"); }
char *WPComputer__wpQueryInfoCPU(wpo_object *self) { return WPComputer_wpQueryInfo(self, "cpu"); }
char *WPComputer__wpQueryInfoMemory(wpo_object *self) { return WPComputer_wpQueryInfo(self, "memory"); }

char *WPComputer__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Computer");
}
