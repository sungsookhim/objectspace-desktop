#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# The plug-in SDK as its users get it (sdk/README.md): installed into a
# scratch root (make install-sdk), a skeleton from osdctl plugin new built
# against it (and the sample), loaded by a private osdd.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-sdk-XXXX")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
make -s install-sdk DESTDIR="$S/root" PREFIX=/usr > "$S/install.log" 2>&1
SDK="$S/root/usr/share/objectspace/sdk" INC="$S/root/usr/include/objectspace"
check "the SDK installed (headers, interfaces, osdc, sample)" "[ -f '$INC/osdo.h' ] && [ -f '$INC/OsdDataFile.h' ] && [ -f '$SDK/classes/OsdObject.osdi' ] && [ -x '$SDK/osdc.py' ] && [ -f '$SDK/sample/Makefile' ]"
(cd "$S" && "$REPO/build/osdctl" plugin new MyNotes OsdDataFile > "$S/new.log" 2>&1)
check "osdctl plugin new: a skeleton" "[ -f '$S/MyNotes/MyNotes.osdi' ] && [ -f '$S/MyNotes/MyNotes.c' ] && grep -q 'NAME    ?= MyNotes' '$S/MyNotes/Makefile'"
make -s -C "$S/MyNotes" OSD_SDK="$SDK" OSD_INC="$INC" > "$S/build.log" 2>&1
check "... built with the SDK alone" "[ -f '$S/MyNotes/mynotes.so' ]"
cp -r "$SDK/sample" "$S/sample" && make -s -C "$S/sample" OSD_SDK="$SDK" OSD_INC="$INC" >> "$S/build.log" 2>&1
check "the sample built with the SDK alone" "[ -f '$S/sample/osdimagefile.so' ]"
# loaded by a private osdd
T="$S/t" RT=$(mktemp -d "$XDG_RUNTIME_DIR/osd-sdk-XXXX")
mkdir -p "$T/desk" "$T/xdg/config" "$T/xdg/data"
export OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data"
export OSD_TEMPLATES_DIR="$T/templates" OSD_UDISKS_BUS="unix:path=$RT/none" OSD_PLUGIN_DIRS="$S/MyNotes"
"$REPO/build/osdd" 2> "$S/osdd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/osdctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
for i in $(seq 30); do grep -q "mynotes ready" "$S/osdd.log" && break; sleep 0.1; done
"$REPO/build/osdctl" settings class:MyNotes > "$S/class.json" 2>&1
check "loaded: its class's notebook has its Note" "grep -q '\"key\":\"note\"' '$S/class.json' && grep -q 'All of class MyNotes' '$S/class.json'"
"$REPO/build/osdctl" shutdown >/dev/null 2>&1; wait $WP
rm -rf "$RT"
echo "$fails failed ($S)"
[ $fails = 0 ] && rm -rf "$S"
[ $fails = 0 ]
