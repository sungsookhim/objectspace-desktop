#!/bin/sh
# Messages (v0.2-plan step 7): osdd's message list, kept across a
# restart, and the Messages, Launcher and TheBar objects; played by
# tests/messages/scenario.py (part 1, a restart, part 2).
cd "$(dirname "$0")/../.."
REPO=$PWD
T=$(mktemp -d "${TMPDIR:-/tmp}/osd-msgs-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/osd-msgs-XXXX")
export T OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" OSD_SAVE_DELAY_MS=100
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" XDG_DATA_DIRS="$T/share" OSD_TEMPLATES_DIR="$T/templates"
export OSD_UDISKS_BUS="unix:path=$RT/none"
start() {
    "$REPO/build/osdd" 2>> "$T/osdd.log" &
    WP=$!
    for i in $(seq 50); do "$REPO/build/osdctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
}
stop() { "$REPO/build/osdctl" shutdown >/dev/null 2>&1; wait $WP; }
start
python3 tests/messages/scenario.py 1
R=$?
stop
start
python3 tests/messages/scenario.py 2 || R=1
stop
[ $R = 0 ] && rm -rf "$T" "$RT" || { echo "kept: $T"; tail -5 "$T/osdd.log"; rm -rf "$RT"; }
exit $R
