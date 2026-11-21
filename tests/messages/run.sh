#!/bin/sh
# Messages (v0.2-plan step 7): wpsd's message list, kept across a
# restart, and the Messages, LaunchPad and WarpCenter objects; played by
# tests/messages/scenario.py (part 1, a restart, part 2).
cd "$(dirname "$0")/../.."
REPO=$PWD
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-msgs-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/wps-msgs-XXXX")
export T WPS_CONFIG_DIR="$T/config" WPS_STATE_DIR="$T/state" WPS_RUNTIME_DIR="$RT" WPS_SAVE_DELAY_MS=100
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" XDG_DATA_DIRS="$T/share" WPS_TEMPLATES_DIR="$T/templates"
export WPS_UDISKS_BUS="unix:path=$RT/none"
start() {
    "$REPO/build/wpsd" 2>> "$T/wpsd.log" &
    WP=$!
    for i in $(seq 50); do "$REPO/build/wpsctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
}
stop() { "$REPO/build/wpsctl" shutdown >/dev/null 2>&1; wait $WP; }
start
python3 tests/messages/scenario.py 1
R=$?
stop
start
python3 tests/messages/scenario.py 2 || R=1
stop
[ $R = 0 ] && rm -rf "$T" "$RT" || { echo "kept: $T"; tail -5 "$T/wpsd.log"; rm -rf "$RT"; }
exit $R
