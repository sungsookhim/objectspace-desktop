#!/bin/sh
# A folder's sort order in osdd's listing (v0.3-plan step 4), played by
# tests/sort/scenario.py on a private osdd.
cd "$(dirname "$0")/../.."
REPO=$PWD
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-sort-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/wps-sort-XXXX")
mkdir -p "$T/f" "$T/config" "$T/home"
export T HOME="$T/home" OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" OSD_SAVE_DELAY_MS=100
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" OSD_TEMPLATES_DIR="$T/templates" OSD_UDISKS_BUS="unix:path=$RT/none"
"$REPO/build/osdd" 2> "$T/osdd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/osdctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
python3 tests/sort/scenario.py; R=$?
"$REPO/build/osdctl" shutdown >/dev/null 2>&1; wait $WP
[ $R = 0 ] && rm -rf "$T" "$RT" || { echo "kept: $T"; rm -rf "$RT"; }
exit $R
