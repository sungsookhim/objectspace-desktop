/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * Connect to a share (0.8 step 4): the address asked for, then, when the
 * server asks for one, the user name and password (Connect, or Connect and
 * remember: kept in GNOME's keyring, as GNOME's Files keeps it); the
 * share's folder opened. The service connects (osdd_network.c: gio mount)
 * and remembers the share in Network; a remembered share opened while not
 * connected comes here for its password if the keyring has none.
 */
#define _GNU_SOURCE
#include "shell.h"

struct conn {
    char address[PATH_MAX];   /* typed, or "" for a share object's */
    char target[PATH_MAX];    /* a remembered share's object ("vobj:ID"), or "" */
    char user[256];
    ui_dialog *dlg;
    int tries;
};

static void ask_user(struct conn *k);

static void connected(void *arg, const cv *reply)
{
    struct conn *k = arg;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (ok && ok->k == CV_BOOL && ok->b) {
        const cv *res = cv_get(reply, "result");
        const char *folder = res ? cv_text(res, "folder") : NULL;
        if (folder && *folder) open_folder(folder, VIEW_LAST);
        events_raise("object.open", k->address[0] ? k->address : k->target, "share", (ui_rect){ 0, 0, 0, 0 });
        free(k);
        return;
    }
    const char *e = reply ? cv_text(reply, "error") : NULL, *msg = reply ? cv_text(reply, "message") : NULL;
    if (e && (!strcmp(e, "password needed") || (!strcmp(e, "refused") && k->tries < 3))) {
        if (!strcmp(e, "refused")) {   /* (said, then asked again) */
            const char *b[] = { "OK" };
            ask("Connect to a share", msg && *msg ? msg : "The server refused the user name or password.", b, 1, 0, NULL, NULL);
        }
        ask_user(k);
        return;
    }
    const char *b[] = { "OK" };
    events_raise("alert.error", "", "", (ui_rect){ 0, 0, 0, 0 });
    ask("Connect to a share", msg && *msg ? msg : reply ? "It could not be connected." : "Lost the connection to osdd.", b, 1, 0, NULL, NULL);
    free(k);
}

static void pass_chosen(void *arg, int choice)
{
    struct conn *k = arg;
    if (choice == 2) { free(k); return; }   /* Cancel */
    k->tries++;
    svc_connect(k->address[0] ? k->address : NULL, k->target[0] ? k->target : NULL, k->user, ui_dialog_text(k->dlg), choice == 1,
                connected, k);   /* (the password wiped when the dialog goes) */
}

static void user_chosen(void *arg, int choice)
{
    struct conn *k = arg;
    if (choice != 0) { free(k); return; }
    snprintf(k->user, sizeof k->user, "%s", ui_dialog_text(k->dlg));
    const char *b[] = { "Connect", "Connect and remember", "Cancel" };
    char m[PATH_MAX + 64];
    snprintf(m, sizeof m, "The password for %s:", k->user[0] ? k->user : "it");
    k->dlg = ui_dialog_open_entry(app, "Connect to a share", m, true, b, 3, 0, pass_chosen, k);
    last_dialog = k->dlg;
}

static void ask_user(struct conn *k)
{
    const char *b[] = { "Next", "Cancel" };
    char m[PATH_MAX + 160];
    snprintf(m, sizeof m, "%s asks for a user name and password. The user name (DOMAIN\\user for a Windows domain):",
             k->address[0] ? k->address : "The share");
    k->dlg = ui_dialog_open_entry(app, "Connect to a share", m, false, b, 2, 0, user_chosen, k);
    const char *me = getenv("USER");
    ui_dialog_set_text(k->dlg, k->user[0] ? k->user : me ? me : "");
    last_dialog = k->dlg;
}

static void address_chosen(void *arg, int choice)
{
    struct conn *k = arg;
    if (choice != 0 || !*ui_dialog_text(k->dlg)) { free(k); return; }
    snprintf(k->address, sizeof k->address, "%s", ui_dialog_text(k->dlg));
    if (!strstr(k->address, "://")) {   /* "server/share": a Windows share, as GNOME's Files takes it */
        char a[PATH_MAX];
        snprintf(a, sizeof a, "smb://%s", k->address[0] == '\\' ? k->address + 2 : k->address);
        for (char *p = a; *p; p++) if (*p == '\\') *p = '/';
        snprintf(k->address, sizeof k->address, "%s", a);
    }
    svc_connect(k->address, NULL, NULL, NULL, false, connected, k);
}

void netconnect_open(void)
{
    struct conn *k = calloc(1, sizeof *k);
    const char *b[] = { "Connect", "Cancel" };
    k->dlg = ui_dialog_open_entry(app, "Connect to a share",
                                  "The share's address: smb://server/share (Windows), nfs://server/export, sftp://user@host/folder, "
                                  "dav://host/folder (WebDAV) or ftp://host",
                                  false, b, 2, 0, address_chosen, k);
    ui_dialog_set_text(k->dlg, "smb://");
    last_dialog = k->dlg;
    events_raise("window.open", "Connect to a share", "window", (ui_rect){ 0, 0, 0, 0 });
}

void netconnect_password(const char *target)
{
    struct conn *k = calloc(1, sizeof *k);
    snprintf(k->target, sizeof k->target, "%s", target);
    ask_user(k);
}
