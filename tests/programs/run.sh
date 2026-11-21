#!/bin/sh
# Programs (v0.2-plan step 5): wpsd with desktop entries in a private
# XDG_DATA_DIRS, played by tests/programs/scenario.py.
cd "$(dirname "$0")/../.."
REPO=$PWD
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-progs-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/wps-progs-XXXX")
mkdir -p "$T/desk" "$T/share/applications"
export T WPS_CONFIG_DIR="$T/config" WPS_STATE_DIR="$T/state" WPS_RUNTIME_DIR="$RT" WPS_SAVE_DELAY_MS=100
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" XDG_DATA_DIRS="$T/share" WPS_TEMPLATES_DIR="$T/templates"
export WPS_PROGRAMS_DESKTOP=GNOME WPS_UDISKS_BUS="unix:path=$RT/none"
python3 tests/programs/scenario.py setup
"$REPO/build/wpsd" 2> "$T/wpsd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/wpsctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
python3 tests/programs/scenario.py
R=$?
"$REPO/build/wpsctl" shutdown >/dev/null 2>&1; wait $WP
[ $R = 0 ] && rm -rf "$T" "$RT" || { echo "kept: $T"; tail -5 "$T/wpsd.log"; rm -rf "$RT"; }
exit $R
