#!/bin/sh
# Open With (v0.3-plan step 7): osdd with desktop entries, mimeapps.list
# and MIME subclasses in a private XDG_DATA_DIRS, played by
# tests/openwith/scenario.py.
cd "$(dirname "$0")/../.."
REPO=$PWD
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-openwith-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/wps-ow-XXXX")
mkdir -p "$T/f" "$T/share/applications" "$T/share/mime" "$T/xdg/config" "$T/home"
export T HOME="$T/home" OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" OSD_SAVE_DELAY_MS=100
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" XDG_DATA_DIRS="$T/share" OSD_TEMPLATES_DIR="$T/templates" OSD_SETUP_DIR="$T/setup" OSD_TERMINAL=
export OSD_UDISKS_BUS="unix:path=$RT/none" LANG=ko_KR.UTF-8 LC_ALL= LC_MESSAGES= XDG_CURRENT_DESKTOP=WPS
python3 tests/openwith/scenario.py setup
"$REPO/build/osdd" 2> "$T/osdd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/osdctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
python3 tests/openwith/scenario.py; R=$?
"$REPO/build/osdctl" shutdown >/dev/null 2>&1; wait $WP
[ $R = 0 ] && [ -z "$KEEP" ] && rm -rf "$T" "$RT" || { echo "kept: $T"; tail -5 "$T/osdd.log"; rm -rf "$RT"; }
exit $R
