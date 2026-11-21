/*
 * Drives (v0.2-design 5): wpsd's udisks2 client.
 *
 * A thread of its own holds a D-Bus connection to udisks2 (the system
 * bus, or $WPS_UDISKS_BUS for tests), reads its objects (Block,
 * Filesystem, Drive, Encrypted) and follows its signals, keeping a list
 * of volumes: file systems and LUKS containers, not those udisks says to
 * ignore (EFI partitions and the like). Calls (mount, unmount, eject,
 * power off, unlock, lock) are asynchronous; their results come back on
 * the loop thread. Without udisks2, the list holds the mounted file
 * systems from mountinfo and nothing can be mounted.
 */
#ifndef WPS_UDISKS_H
#define WPS_UDISKS_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wpo_loop.h"

typedef struct {
    char uuid[80];          /* identity: the file system's (or LUKS container's) UUID */
    char object[256];       /* udisks2's Block object ("" without udisks2) */
    char fs_object[256];    /* the one with the file system: itself, or a LUKS volume's open side ("" locked) */
    char drive_object[256]; /* its Drive ("" none) */
    char device[64];        /* /dev/sdb1 */
    char label[128], fstype[32];
    uint64_t size;
    char mount[PATH_MAX];   /* where it is mounted ("" not mounted); "/" first if it is */
    char kind[12];          /* "disk", "usb", "optical", "sd", "root" */
    bool removable, ejectable, can_poweroff, system;
    bool encrypted, unlocked;
} wps_drive_info;

/* Start following udisks2; changed() runs on the loop thread whenever the
 * list changes (also once at start). */
void wps_udisks_init(wpo_loop *loop, void (*changed)(void));
void wps_udisks_shutdown(void);
bool wps_udisks_available(void);

/* A copy of the list (free() it); its length. */
size_t wps_udisks_list(wps_drive_info **out);
bool wps_udisks_find(const char *uuid, wps_drive_info *out);

/* "mount", "unmount", "eject" (unmounts first), "poweroff", "unlock"
 * (pass: the passphrase), "lock". done(arg, error, message, mount) on the
 * loop thread: error 0, or an errno-like value with udisks' message
 * (EACCES: not authorized; EBUSY: in use). mount: where it is mounted. */
typedef void (*wps_udisks_fn)(void *arg, int error, const char *message, const char *mount);
void wps_udisks_call(const char *uuid, const char *action, const char *pass, wps_udisks_fn done, void *arg);

/* A title as GNOME's Files gives one: the label, else "500 GB Volume". */
void wps_drive_title(const wps_drive_info *d, char *out, size_t n);

#endif
