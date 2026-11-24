#!/bin/sh
# Programs (v0.2-plan step 5): osdd with desktop entries in a private
# XDG_DATA_DIRS, played by tests/programs/scenario.py.
cd "$(dirname "$0")/../.."
REPO=$PWD
T=$(mktemp -d "${TMPDIR:-/tmp}/osd-progs-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/osd-progs-XXXX")
mkdir -p "$T/desk" "$T/share/applications"
export T OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" OSD_SAVE_DELAY_MS=100
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" XDG_DATA_DIRS="$T/share" OSD_TEMPLATES_DIR="$T/templates"
export OSD_PROGRAMS_DESKTOP=GNOME OSD_UDISKS_BUS="unix:path=$RT/none"
# (an executable's text mode: this "terminal" notes what it was asked to run)
printf '#!/bin/sh\nprintf "%%s\\n" "$@" > "%s/term.txt"\n' "$T" > "$T/fake-term" && chmod +x "$T/fake-term"
export OSD_TERMINAL="$T/fake-term"
python3 tests/programs/scenario.py setup
"$REPO/build/osdd" 2> "$T/osdd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/osdctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
python3 tests/programs/scenario.py
R=$?
"$REPO/build/osdctl" shutdown >/dev/null 2>&1; wait $WP
[ $R = 0 ] && rm -rf "$T" "$RT" || { echo "kept: $T"; tail -5 "$T/osdd.log"; rm -rf "$RT"; }
exit $R
