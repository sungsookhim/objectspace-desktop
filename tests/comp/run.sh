#!/bin/sh
# wps-compositor, headless, with WPS's desktop (a private wpsd and folder)
# and other programs, played by a script (see wps-compositor.c).
#   tests/comp/run.sh SCRIPT [COMMAND...]
# COMMAND (default: a GTK 4 dialog and the test client) runs
# inside, with WAYLAND_DISPLAY; $T is the test folder (desk/ the desktop),
# its logs and screenshots stay there (printed at the end).
# Settings: SIZE (1280x800), SECS (time out, 30). GTK apps use their software
# renderer (GSK_RENDERER=cairo): the headless screen has no GPU buffers.
cd "$(dirname "$0")/../.."
REPO=$PWD
SCRIPT=$(readlink -f "$1"); shift
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-comp-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/wps-comp-XXXX")
mkdir -p "$T/desk/Projects" "$T/config" "$T/state" "$T/xdg/config" "$T/xdg/data" "$T/cache"
echo hi > "$T/desk/notes.txt"
export T WPS_CONFIG_DIR="$T/config" WPS_STATE_DIR="$T/state" WPS_RUNTIME_DIR="$RT" WPS_SAVE_DELAY_MS=100
export WPS_TEMPLATES_DIR="$T/templates" WPS_SETUP_DIR="$T/setup" WPS_LAUNCHPAD_DIR="$T/launchpad"
export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" XDG_CACHE_HOME="$T/cache"
"$REPO/build/wpsd" 2> "$T/wpsd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/wpsctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
CMD=${*:-"GDK_BACKEND=wayland GSK_RENDERER=cairo GTK_A11Y=none zenity --info --text 'A GTK 4 dialog' 2> '$T/app.log' & sleep 0.5; '$REPO/build/tests/ssd-client' Framed 3060a0 2> '$T/ssd.log'"}
# WPS's desktop, as the session runs it (-d); DESKENV: settings for it only
# (e.g. WPS_SHELL_SELFTEST=winlist), NODESK=1: none
DESK="env $DESKENV '$REPO/build/wps-shell' --desktop '$T/desk' 2>> '$T/shell.log'"
[ -n "$NODESK" ] && DESK=
WPS_COMP_HEADLESS=${SIZE:-1280x800} WPS_COMP_SCRIPT="$SCRIPT" timeout "${SECS:-30}" "$REPO/build/wps-compositor" ${DESK:+-d "$DESK"} -s "$CMD" 2> "$T/comp.log"
echo "compositor exit $?"
grep -v "^ssd-client: configured" "$T/comp.log"
"$REPO/build/wpsctl" shutdown >/dev/null 2>&1; wait $WP 2>/dev/null
# what it started and is still running (by its runtime folder: never others)
for p in $(pgrep -u "$(id -u)"); do
  { tr '\0' '\n' < /proc/$p/environ; } 2>/dev/null | grep -q "^WPS_RUNTIME_DIR=$RT\$" && kill $p 2>/dev/null
done
rm -rf "$RT"
echo "T=$T"
