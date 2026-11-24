#!/bin/sh
# Drives against a fake udisks2 on a private bus (v0.2-plan step 4):
#   tests/drives/run.sh [SCENARIO.py]   (default: tests/drives/scenario.py)
# Runs a private dbus-daemon, tests/fake_udisks.py on it, and osdd with
# OSD_UDISKS_BUS; the scenario gets T, OSD_RUNTIME_DIR and FAKE_BUS.
cd "$(dirname "$0")/../.."
REPO=$PWD
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-drives-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/wps-drives-XXXX")
mkdir -p "$T/desk" "$T/mnt"
export T OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" OSD_SAVE_DELAY_MS=100
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" OSD_TEMPLATES_DIR="$T/templates"
dbus-daemon --session --address="unix:path=$RT/bus" --nofork --nopidfile 2> "$T/dbus.log" &
DB=$!
for i in $(seq 50); do [ -S "$RT/bus" ] && break; sleep 0.1; done
export FAKE_BUS="unix:path=$RT/bus"
python3 tests/fake_udisks.py "$FAKE_BUS" "$T/mnt" > "$T/fake.log" 2>&1 &
FK=$!
for i in $(seq 50); do grep -q ready "$T/fake.log" 2>/dev/null && break; sleep 0.1; done
OSD_UDISKS_BUS="$FAKE_BUS" "$REPO/build/osdd" 2> "$T/osdd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/osdctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
python3 "${1:-tests/drives/scenario.py}"
R=$?
"$REPO/build/osdctl" shutdown >/dev/null 2>&1; wait $WP
kill $FK $DB 2>/dev/null; wait 2>/dev/null
[ $R = 0 ] && rm -rf "$T" "$RT" || { echo "kept: $T"; tail -5 "$T/fake.log" "$T/osdd.log"; rm -rf "$RT"; }
exit $R
