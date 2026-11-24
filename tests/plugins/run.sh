#!/bin/sh
# Plug-ins as the SDK's users write them (docs/object-properties-design.md
# 2.7): the sample OsdImageFile (sdk/sample) loaded by a private wpsd, in its
# sandboxed host; played by tests/plugins/sample.py. No sudo needed.
cd "$(dirname "$0")/../.."
REPO=$PWD
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-plug-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/wps-plug-XXXX")
export T OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" OSD_SAVE_DELAY_MS=100
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" OSD_TEMPLATES_DIR="$T/templates"
export OSD_UDISKS_BUS="unix:path=$RT/none" OSD_PLUGIN_DIRS="$REPO/build/tests/sample"
mkdir -p "$T/desk" "$T/xdg/config" "$T/xdg/data"
"$REPO/build/wpsd" 2> "$T/wpsd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/wpsctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
python3 tests/plugins/sample.py
R=$?
"$REPO/build/wpsctl" shutdown >/dev/null 2>&1; wait $WP
grep -E "ERROR|runtime error" "$T/wpsd.log"
[ $R = 0 ] && rm -rf "$T" "$RT" || { echo "kept: $T"; grep -i plug "$T/wpsd.log" | tail -5; rm -rf "$RT"; }
exit $R
