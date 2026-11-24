#!/bin/sh
# Every object's settings pages and menu (tests/props/snapshot.py) into OUT,
# from a private wpsd; get, set and props (tests/props/ops.py); then, with
# BASE, the snapshot compared with an earlier one.
#   tests/props/run.sh OUT [BASE]
cd "$(dirname "$0")/../.."
REPO=$PWD
mkdir -p "${1:?usage: tests/props/run.sh OUT [BASE]}"
OUT=$(readlink -f "$1")
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-props-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/wps-props-XXXX")
export T OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" OSD_SAVE_DELAY_MS=100
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" OSD_TEMPLATES_DIR="$T/templates"
export OSD_UDISKS_BUS="unix:path=$RT/none" OSD_PROGRAMS_DESKTOP=GNOME
mkdir -p "$T/desk" "$T/xdg/config" "$T/xdg/data"
"$REPO/build/wpsd" 2>> "$T/wpsd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/wpsctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
rm -f "$OUT"/*.txt
python3 ${SNAP:-tests/props/snapshot.py} "$OUT"
R=$?
python3 tests/props/ops.py || R=1
"$REPO/build/wpsctl" shutdown >/dev/null 2>&1; wait $WP
grep -E "ERROR|runtime error" "$T/wpsd.log"
rm -rf "$T" "$RT"
if [ -n "$2" ]; then
    if diff -r "$2" "$OUT" > "$OUT.diff"; then echo "same as $2"; rm -f "$OUT.diff"; else echo "DIFFERS from $2 (see $OUT.diff)"; R=1; fi
fi
exit $R
