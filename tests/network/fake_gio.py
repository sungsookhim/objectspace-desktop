#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""gio and secret-tool standing in for tests/network (as either, by its
name): "gio mount ADDRESS" mounts into OSD_GVFS_DIR as GNOME's would (a
folder named as gvfs names them), asking for a user, a domain and a
password as gio does for a share that needs one (smb://filer/secure:
alice, secret; unless the keyring holds alice's); "gio mount -u PATH"
takes the folder away; "secret-tool store|clear ATTRS" keep the keyring in
$T/keyring. Every call logged in $T/gio.log."""
import os
import sys

T = os.environ["T"]
GVFS = os.environ["OSD_GVFS_DIR"]
me = os.path.basename(sys.argv[0])
with open(T + "/gio.log", "a") as f:
    f.write(me + " " + " ".join(sys.argv[1:]) + "\n")

if me == "secret-tool":
    op, attrs = sys.argv[1], sys.argv[2:]
    if op == "store":
        attrs = attrs[2:]   # (--label LABEL)
        pw = sys.stdin.read()
        with open(T + "/keyring", "a") as f:
            f.write(" ".join(attrs) + "\t" + pw + "\n")
    elif op == "clear":
        key = " ".join(attrs)
        lines = open(T + "/keyring").read().splitlines() if os.path.exists(T + "/keyring") else []
        with open(T + "/keyring", "w") as f:
            f.writelines(l + "\n" for l in lines if not l.startswith(key))
    sys.exit(0)

if sys.argv[1:3] == ["mount", "-u"]:
    os.rename(sys.argv[3], T + "/unmounted-" + os.path.basename(sys.argv[3]))
    sys.exit(0)
addr = sys.argv[2]
if "nohost" in addr:
    print("gio: %s: Failed to retrieve share list from server: Name or service not known" % addr, flush=True)
    sys.exit(2)
if addr.endswith("/public"):
    os.makedirs(GVFS + "/smb-share:server=filer,share=public", exist_ok=True)
    sys.exit(0)
if addr.endswith("/secure"):
    keyring = open(T + "/keyring").read() if os.path.exists(T + "/keyring") else ""
    if "server filer user alice" in keyring and "alice@" in addr:   # (as gvfs: the keyring's first)
        os.makedirs(GVFS + "/smb-share:server=filer,share=secure,user=alice", exist_ok=True)
        sys.exit(0)
    print("Authentication Required\nEnter user and password for share \u201csecure\u201d on \u201cfiler\u201d:", flush=True)
    for _ in range(2):
        sys.stdout.write("User [nobody]: "); sys.stdout.flush()
        user = sys.stdin.readline()
        sys.stdout.write("Domain [WORKGROUP]: "); sys.stdout.flush()
        sys.stdin.readline()
        sys.stdout.write("Password: "); sys.stdout.flush()
        pw = sys.stdin.readline()
        if not pw:
            break
        if user.strip() == "alice" and pw.strip() == "secret":
            os.makedirs(GVFS + "/smb-share:server=filer,share=secure,user=alice", exist_ok=True)
            sys.exit(0)
    print("gio: %s: Failed to mount Windows share: Permission denied" % addr, flush=True)
    sys.exit(2)
print("gio: %s: No such file or directory" % addr, flush=True)
sys.exit(2)
