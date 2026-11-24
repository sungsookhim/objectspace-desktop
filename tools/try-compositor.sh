#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Try osd-compositor (G5) without logging out: it runs in a window of your
# session, with Objectspace's desktop inside on FOLDER (default: a scratch folder
# with a few files), a private osdd and private Objectspace state. Start programs
# from the desktop, or pass a command to run as well, e.g.
#   tools/try-compositor.sh ~/Desktop 1600x900 gnome-calculator
# Ctrl+Alt+BackSpace (or closing the window) ends it. Inside: click to
# focus, drag the title to move, the border to size, double-click the
# title to maximize, Alt+Tab, Alt+F4, Ctrl+Esc for the Window List, Shift+Esc
# for a framed window's system menu.
#
# usage: tools/try-compositor.sh [FOLDER] [WIDTHxHEIGHT] [COMMAND...]
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
for f in "$REPO/build/osd-compositor" "$REPO/build/osd-shell" "$REPO/build/osdd"; do
    [ -x "$f" ] || { echo "$f is missing: run 'make && make gui && make compositor' first" >&2; exit 1; }
done
T=$(mktemp -d "${TMPDIR:-/tmp}/osd-trycomp-XXXX")
RT=$(mktemp -d "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/osd-trycomp-XXXX"); chmod 700 "$RT"
DESK=${1:-$T/Desktop}
if [ -z "$1" ]; then
    mkdir -p "$DESK/Projects"
    echo "Some notes" > "$DESK/notes.txt"
fi
SIZE=${2:-1280x800}
[ $# -gt 0 ] && shift
[ $# -gt 0 ] && shift
mkdir -p "$T/config" "$T/state"
export OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT"
export OSD_TEMPLATES_DIR="$T/templates" OSD_SETUP_DIR="$T/setup" OSD_LAUNCHER_DIR="$T/launcher"
"$REPO/build/osdd" 2> "$T/osdd.log" &
WP=$!
cleanup() {
    "$REPO/build/osdctl" shutdown >/dev/null 2>&1 || kill $WP 2>/dev/null
    for p in $(pgrep -u "$(id -u)"); do   # what it started (never your other programs)
        { tr '\0' '\n' < /proc/$p/environ; } 2>/dev/null | grep -q "^OSD_RUNTIME_DIR=$RT\$" && kill $p 2>/dev/null
    done
    rm -rf "$RT"
}
trap cleanup EXIT
for i in $(seq 50); do "$REPO/build/osdctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
DESKTOP="'$REPO/build/osd-shell' --desktop '$DESK' 2>> '$T/shell.log'"
echo "Objectspace compositor: logs in $T"
if [ $# -gt 0 ]; then
    OSD_COMP_SIZE=$SIZE "$REPO/build/osd-compositor" -d "$DESKTOP" -s "sleep 1; $*" 2> "$T/compositor.log"
else
    OSD_COMP_SIZE=$SIZE "$REPO/build/osd-compositor" -d "$DESKTOP" 2> "$T/compositor.log"
fi
