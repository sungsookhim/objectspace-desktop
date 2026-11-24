#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Plug-ins as the SDK's users write them (docs/object-properties-design.md
# 2.7): the sample OsdImageFile (sdk/sample) loaded by a private osdd, in its
# sandboxed host; played by tests/plugins/sample.py. No sudo needed.
cd "$(dirname "$0")/../.."
REPO=$PWD
T=$(mktemp -d "${TMPDIR:-/tmp}/osd-plug-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/osd-plug-XXXX")
export T OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" OSD_SAVE_DELAY_MS=100
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" OSD_TEMPLATES_DIR="$T/templates"
export OSD_UDISKS_BUS="unix:path=$RT/none" OSD_PLUGIN_DIRS="$REPO/build/tests/sample"
mkdir -p "$T/desk" "$T/xdg/config" "$T/xdg/data"
"$REPO/build/osdd" 2> "$T/osdd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/osdctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
python3 tests/plugins/sample.py
R=$?
"$REPO/build/osdctl" shutdown >/dev/null 2>&1; wait $WP
grep -E "ERROR|runtime error" "$T/osdd.log"
[ $R = 0 ] && rm -rf "$T" "$RT" || { echo "kept: $T"; grep -i plug "$T/osdd.log" | tail -5; rm -rf "$RT"; }
exit $R
