#!/bin/sh
# osd-compositor, headless, with Objectspace's desktop (a private osdd and folder)
# and other programs, played by a script (see osd-compositor.c).
#   tests/comp/run.sh SCRIPT [COMMAND...]
# COMMAND (default: a GTK 4 dialog and the test client) runs
# inside, with WAYLAND_DISPLAY; $T is the test folder (desk/ the desktop),
# its logs and screenshots stay there (printed at the end).
# Settings: SIZE (1280x800), SECS (time out, 30), PRE (a command before the desktop). GTK apps use their software
# renderer (GSK_RENDERER=cairo): the headless screen has no GPU buffers.
cd "$(dirname "$0")/../.."
REPO=$PWD
SCRIPT=$(readlink -f "$1"); shift
T=${KEEP_T:-$(mktemp -d "${TMPDIR:-/tmp}/osd-comp-XXXX")}   # KEEP_T: a test folder again (a second login)
RT=$(mktemp -d "$XDG_RUNTIME_DIR/osd-comp-XXXX")
mkdir -p "$T/desk/Projects" "$T/config" "$T/state" "$T/xdg/config" "$T/xdg/data" "$T/cache"
echo hi > "$T/desk/notes.txt"
export T OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" OSD_SAVE_DELAY_MS=100
export OSD_TEMPLATES_DIR="$T/templates" OSD_SETUP_DIR="$T/setup" OSD_LAUNCHER_DIR="$T/launcher"
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" XDG_CACHE_HOME="$T/cache"
"$REPO/build/osdd" 2> "$T/osdd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/osdctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
# PRE: a command run once osdd is up, before the desktop (to prepare objects)
[ -n "$PRE" ] && sh -c "$PRE"
CMD=${*:-"GDK_BACKEND=wayland GSK_RENDERER=cairo GTK_A11Y=none zenity --info --text 'A GTK 4 dialog' 2> '$T/app.log' & sleep 0.5; '$REPO/build/tests/ssd-client' Framed 3060a0 2> '$T/ssd.log'"}
# Objectspace's desktop, as the session runs it (-d); DESKENV: settings for it only
# (e.g. OSD_SHELL_SELFTEST=winlist), NODESK=1: none
DESK="env $DESKENV '$REPO/build/osd-shell' --desktop '$T/desk' 2>> '$T/shell.log'"
[ -n "$NODESK" ] && DESK=
OSD_COMP_HEADLESS=${SIZE:-1280x800} OSD_COMP_SCRIPT="$SCRIPT" timeout "${SECS:-30}" "$REPO/build/osd-compositor" ${DESK:+-d "$DESK"} -s "$CMD" 2> "$T/comp.log"
echo "compositor exit $?"
grep -v "^ssd-client: configured" "$T/comp.log"
"$REPO/build/osdctl" shutdown >/dev/null 2>&1; wait $WP 2>/dev/null
# what it started and is still running (by its runtime folder: never others)
for p in $(pgrep -u "$(id -u)"); do
  { tr '\0' '\n' < /proc/$p/environ; } 2>/dev/null | grep -q "^OSD_RUNTIME_DIR=$RT\$" && kill $p 2>/dev/null
done
rm -rf "$RT"
echo "T=$T"
