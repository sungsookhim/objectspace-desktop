#!/bin/sh
# The WPS desktop inside a private, headless GNOME Shell (its own D-Bus,
# dconf, runtime folder and Wayland socket; the user's session is not
# touched). The extension starts the desktop; a probe extension logs what
# GNOME sees (Alt+Tab list, stacking, window types and sizes) and takes a
# screenshot ($T/shot.png). A folder window opens on top; a file is added
# to the desktop after the overview has closed.
#   SELFTEST=desktop   the desktop runs OSD_SHELL_SELFTEST=desktop
#   TERMINATE=<title>  (SELFTEST=winlist) a window to end from the Window List
#   KILLTEST=1         the desktop is killed once: the extension restarts it
#   WLDEBUG=1          the desktop's Wayland traffic goes to the log
#   DESKENV=NAME=VALUE one more setting for the desktop (e.g. OSD_DEBUG_DND=1)
#   SYSTEM="key=value ..." the System object's options (e.g. shadows=2)
#   PROBE_MODE=frame   click the folder window's maximize button (input through a shadow margin)
#   PROBE_MODE=interact  a virtual pointer double-clicks and drags desktop
#                      icons (no folder window; set SECS=14 for the time it needs)
# usage: tests/gnome/run.sh   (after make && make gui)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-gnome-XXXX")
RT=$(mktemp -d "/run/user/$(id -u)/wps-hl-XXXX"); chmod 700 "$RT"
mkdir -p "$T/config" "$T/data/gnome-shell/extensions" "$T/cache" "$T/desk/Projects" "$T/w"
echo hi > "$T/desk/notes.txt"
E="$T/data/gnome-shell/extensions/objectspace@sungsookhim.github.io"
mkdir -p "$E"; cp -r "$REPO/gnome-extension/objectspace@sungsookhim.github.io/"* "$E/"; glib-compile-schemas "$E/schemas"
if [ -n "$SELFTEST" ]; then   # the desktop runs a self-test (its output in shell.log)
  printf '{"command": ["env", "OSD_SELFTEST_TERMINATE=%s", "OSD_SHELL_SELFTEST=%s", "XDG_CACHE_HOME=%s", "LSAN_OPTIONS=suppressions=%s:print_suppressions=0", "%s", "--desktop", "%s"]}\n' "$TERMINATE" "$SELFTEST" "$T/cache" "$REPO/tests/gui/lsan.supp" "$REPO/build/osd-shell-asan" "$T/desk" > "$E/config.json"
elif [ -n "$WLDEBUG" ]; then
  printf '{"command": ["env", "WAYLAND_DEBUG=1", "%s", "--desktop", "%s"]}\n' "$REPO/build/osd-shell" "$T/desk" > "$E/config.json"
elif [ -n "$DESKENV" ]; then   # e.g. DESKENV=OSD_DEBUG_DND=1
  printf '{"command": ["env", "%s", "%s", "--desktop", "%s"]}\n' "$DESKENV" "$REPO/build/osd-shell" "$T/desk" > "$E/config.json"
elif [ -n "$ASAN" ]; then   # the plain desktop, under the sanitizers
  printf '{"command": ["env", "LSAN_OPTIONS=suppressions=%s:print_suppressions=0", "%s", "--desktop", "%s"]}\n' "$REPO/tests/gui/lsan.supp" "$REPO/build/osd-shell-asan" "$T/desk" > "$E/config.json"
else
  printf '{"command": ["%s", "--desktop", "%s"]}\n' "$REPO/build/osd-shell" "$T/desk" > "$E/config.json"
fi
cp -r "$REPO/tests/gnome/wps-probe@local" "$T/data/gnome-shell/extensions/"
export XDG_RUNTIME_DIR="$RT" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_CACHE_HOME="$T/cache"
export OSD_TEMPLATES_DIR="$T/templates" OSD_SETUP_DIR="$T/setup" OSD_LAUNCHPAD_DIR="$T/launchpad"
export OSD_CONFIG_DIR="$T/w/config" OSD_STATE_DIR="$T/w/state" OSD_RUNTIME_DIR="$RT/objectspace"
export OSD_DEBUG_COMMANDS=1 PROBE_SHOT="$T/shot.png" OSD_WALLPAPER="${WALL:-/usr/share/backgrounds/f45/default/f45-01-day.jxl}"
unset WAYLAND_DISPLAY DISPLAY JOURNAL_STREAM INVOCATION_ID
mkdir -p "$RT/objectspace"
"$REPO/build/osdd" 2> "$T/osdd.log" &
WP=$!
# SYSTEM="shadows=2 title_layout=1": the System object's options, before anything opens
if [ -n "$SYSTEM" ]; then
  for i in $(seq 50); do
    SA=$("$REPO/build/osdctl" setup 2>/dev/null | grep -o '"OsdSystem": *{"address": *"vobj:[^"]*"' | grep -o 'vobj:[^"]*')
    [ -n "$SA" ] && break; sleep 0.1
  done
  "$REPO/build/osdctl" settings_set "$SA" $SYSTEM > /dev/null
fi
dbus-run-session -- sh -c "
  gsettings set org.gnome.shell enabled-extensions \"['objectspace@sungsookhim.github.io', 'wps-probe@local']\"
  gsettings set org.gnome.shell disable-user-extensions false
  gsettings set org.gnome.shell welcome-dialog-last-shown-version '999'
  gnome-shell --headless --wayland --no-x11 --virtual-monitor 1280x800 --wayland-display wps-hl > '$T/shell.log' 2>&1 &
  SH=\$!
  sleep 5
  [ \"\$PROBE_MODE\" = interact ] || WAYLAND_DISPLAY=wps-hl '$REPO/build/osd-shell' '$T/desk/Projects' 2> '$T/folderwin.log' &
  # APP=gnome-text-editor: another program's window too (the window list's icons)
  [ -n \"\$APP\" ] && WAYLAND_DISPLAY=wps-hl \$APP > '$T/app.log' 2>&1 &
  if [ -n \"\$KILLTEST\" ]; then sleep 1; pkill -KILL -f -- 'osd-shell --deskto[p]'; fi
  sleep 4
  echo late > '$T/desk/added-later.txt'
  sleep ${SECS:-9}
  kill \$SH; sleep 1; kill -9 \$SH 2>/dev/null
" 
"$REPO/build/osdctl" shutdown >/dev/null 2>&1; wait $WP 2>/dev/null
grep -E "wps:|probe:|selftest:|dnd:|timing:|event:|JS ERROR|ERROR: AddressSanitizer|runtime error|wps-ui" "$T/shell.log" | grep -v "^$" | head -40
ls -la "$T/shot.png" 2>/dev/null
echo "T=$T"
# anything of this session still running, and its mounts
for p in $(pgrep -u "$(id -u)"); do tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -q "XDG_RUNTIME_DIR=$RT" && kill -9 $p 2>/dev/null; done
for m in $(grep -o "$RT/[^ ]*" /proc/mounts); do fusermount3 -u "$m" 2>/dev/null; done
rm -rf "$RT"
