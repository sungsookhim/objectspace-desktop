/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#define _GNU_SOURCE
#include "OsdComputer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "osd_classes.h"
#include "osd_menu_ids.h"

char *osd_menu_without(char *menu, const int *ids, int n);

char *OsdComputer__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    static const int gone[] = { OSD_MI_CREATE_ANOTHER };
    return osd_menu_without(OsdComputer_parent_osdModifyPopupMenu(self, menu), gone, 1);
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
/* Its Computer page (OsdComputer.osdi): what this computer is. */
char *OsdComputer__osdQueryInfo(osdo_object *self, const char *which)
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
    return osd_strdup(v);
}

char *OsdComputer__osdQueryInfoHost(osdo_object *self) { return OsdComputer_osdQueryInfo(self, "host"); }
char *OsdComputer__osdQueryInfoOS(osdo_object *self) { return OsdComputer_osdQueryInfo(self, "os"); }
char *OsdComputer__osdQueryInfoKernel(osdo_object *self) { return OsdComputer_osdQueryInfo(self, "kernel"); }
char *OsdComputer__osdQueryInfoCPU(osdo_object *self) { return OsdComputer_osdQueryInfo(self, "cpu"); }
char *OsdComputer__osdQueryInfoMemory(osdo_object *self) { return OsdComputer_osdQueryInfo(self, "memory"); }

char *OsdComputer__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Computer");
}
