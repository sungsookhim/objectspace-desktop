#!/bin/sh
# Try wps-compositor (G5) without logging out: it runs in a window of your
# session, with WPS's desktop inside on FOLDER (default: a scratch folder
# with a few files), a private wpsd and private WPS state. Start programs
# from the desktop, or pass a command to run as well, e.g.
#   tools/try-compositor.sh ~/Desktop 1600x900 gnome-calculator
# Ctrl+Alt+BackSpace (or closing the window) ends it. Inside: click to
# focus, drag the title to move, the border to size, double-click the
# title to maximize, Alt+Tab, Alt+F4.
#
# usage: tools/try-compositor.sh [FOLDER] [WIDTHxHEIGHT] [COMMAND...]
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
for f in "$REPO/build/wps-compositor" "$REPO/build/wps-shell" "$REPO/build/wpsd"; do
    [ -x "$f" ] || { echo "$f is missing: run 'make && make gui && make compositor' first" >&2; exit 1; }
done
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-trycomp-XXXX")
RT=$(mktemp -d "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/wps-trycomp-XXXX"); chmod 700 "$RT"
DESK=${1:-$T/Desktop}
if [ -z "$1" ]; then
    mkdir -p "$DESK/Projects"
    echo "Some notes" > "$DESK/notes.txt"
fi
SIZE=${2:-1280x800}
[ $# -gt 0 ] && shift
[ $# -gt 0 ] && shift
mkdir -p "$T/config" "$T/state"
export WPS_CONFIG_DIR="$T/config" WPS_STATE_DIR="$T/state" WPS_RUNTIME_DIR="$RT"
export WPS_TEMPLATES_DIR="$T/templates" WPS_SETUP_DIR="$T/setup" WPS_LAUNCHPAD_DIR="$T/launchpad"
"$REPO/build/wpsd" 2> "$T/wpsd.log" &
WP=$!
cleanup() {
    "$REPO/build/wpsctl" shutdown >/dev/null 2>&1 || kill $WP 2>/dev/null
    for p in $(pgrep -u "$(id -u)"); do   # what it started (never your other programs)
        { tr '\0' '\n' < /proc/$p/environ; } 2>/dev/null | grep -q "^WPS_RUNTIME_DIR=$RT\$" && kill $p 2>/dev/null
    done
    rm -rf "$RT"
}
trap cleanup EXIT
for i in $(seq 50); do "$REPO/build/wpsctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
CMD="'$REPO/build/wps-shell' --desktop '$DESK' 2> '$T/shell.log'"
[ $# -gt 0 ] && CMD="$CMD & sleep 1; $*"
echo "WPS compositor: logs in $T"
WPS_COMP_SIZE=$SIZE "$REPO/build/wps-compositor" -s "$CMD" 2> "$T/compositor.log"
