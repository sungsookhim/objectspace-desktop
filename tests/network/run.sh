#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Network shares (0.8 step 3): osdd with GNOME's virtual file systems'
# folder stood in for by a plain one (OSD_GVFS_DIR; a share: a folder named
# as gvfs names them), taken as without attributes (OSD_NET_NO_XATTR), its
# folders looked at every second (OSD_NET_POLL_MS), and a `gio` of the
# test's own (Disconnect); played by tests/network/scenario.py, which
# starts osdd again with the share at another place.
cd "$(dirname "$0")/../.."
REPO=$PWD
T=$(mktemp -d "${TMPDIR:-/tmp}/osd-network-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/osd-net-XXXX")
mkdir -p "$T/desk" "$T/gvfs" "$T/gvfs2" "$T/bin"
# gio and secret-tool of the test's own (tests/network/fake_gio.py): a
# server "filer" with "public" (no password) and "secure" (alice, secret);
# the keyring a file
cp tests/network/fake_gio.py "$T/bin/gio"
cp tests/network/fake_gio.py "$T/bin/secret-tool"
chmod +x "$T/bin/gio" "$T/bin/secret-tool"
export T REPO PATH="$T/bin:$PATH" OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" OSD_SAVE_DELAY_MS=100
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" OSD_TEMPLATES_DIR="$T/templates"
export OSD_UDISKS_BUS="unix:path=$RT/none" OSD_GVFS_DIR="$T/gvfs" OSD_NET_NO_XATTR=1 OSD_NET_POLL_MS=1000
python3 tests/network/scenario.py
R=$?
"$REPO/build/osdctl" shutdown >/dev/null 2>&1
sleep 0.3
[ $R = 0 ] && rm -rf "$T" "$RT" || { echo "kept: $T"; tail -5 "$T/osdd.log"; rm -rf "$RT"; }
exit $R
