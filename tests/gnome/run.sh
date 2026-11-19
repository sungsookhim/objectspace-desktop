#!/bin/sh
# The WPS desktop inside a private, headless GNOME Shell (its own D-Bus,
# dconf, runtime folder and Wayland socket; the user's session is not
# touched). The extension starts the desktop; a probe extension logs what
# GNOME sees (Alt+Tab list, stacking, window types and sizes) and takes a
# screenshot ($T/shot.png). A folder window opens on top; a file is added
# to the desktop after the overview has closed.
#   SELFTEST=desktop   the desktop runs WPS_SHELL_SELFTEST=desktop
#   KILLTEST=1         the desktop is killed once: the extension restarts it
#   WLDEBUG=1          the desktop's Wayland traffic goes to the log
#   DESKENV=NAME=VALUE one more setting for the desktop (e.g. WPS_DEBUG_DND=1)
#   PROBE_MODE=interact  a virtual pointer double-clicks and drags desktop
#                      icons (no folder window; set SECS=14 for the time it needs)
# usage: tests/gnome/run.sh   (after make && make gui)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-gnome-XXXX")
RT=$(mktemp -d "/run/user/$(id -u)/wps-hl-XXXX"); chmod 700 "$RT"
mkdir -p "$T/config" "$T/data/gnome-shell/extensions" "$T/cache" "$T/desk/Projects" "$T/w"
echo hi > "$T/desk/notes.txt"
E="$T/data/gnome-shell/extensions/wps@wps-linux"
mkdir -p "$E"; cp "$REPO/gnome-extension/wps@wps-linux/"* "$E/"
if [ -n "$SELFTEST" ]; then   # the desktop runs a self-test (its output in shell.log)
  printf '{"command": ["env", "WPS_SHELL_SELFTEST=%s", "XDG_CACHE_HOME=%s", "%s", "--desktop", "%s"]}\n' "$SELFTEST" "$T/cache" "$REPO/build/wps-shell-asan" "$T/desk" > "$E/config.json"
elif [ -n "$WLDEBUG" ]; then
  printf '{"command": ["env", "WAYLAND_DEBUG=1", "%s", "--desktop", "%s"]}\n' "$REPO/build/wps-shell" "$T/desk" > "$E/config.json"
elif [ -n "$DESKENV" ]; then   # e.g. DESKENV=WPS_DEBUG_DND=1
  printf '{"command": ["env", "%s", "%s", "--desktop", "%s"]}\n' "$DESKENV" "$REPO/build/wps-shell" "$T/desk" > "$E/config.json"
else
  printf '{"command": ["%s", "--desktop", "%s"]}\n' "$REPO/build/wps-shell" "$T/desk" > "$E/config.json"
fi
cp -r "$REPO/tests/gnome/wps-probe@local" "$T/data/gnome-shell/extensions/"
export XDG_RUNTIME_DIR="$RT" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_CACHE_HOME="$T/cache"
export WPS_CONFIG_DIR="$T/w/config" WPS_STATE_DIR="$T/w/state" WPS_RUNTIME_DIR="$RT/wps"
export PROBE_SHOT="$T/shot.png" WPS_WALLPAPER="${WALL:-/usr/share/backgrounds/f45/default/f45-01-day.jxl}"
unset WAYLAND_DISPLAY DISPLAY JOURNAL_STREAM INVOCATION_ID
mkdir -p "$RT/wps"
"$REPO/build/wpsd" 2> "$T/wpsd.log" &
WP=$!
dbus-run-session -- sh -c "
  gsettings set org.gnome.shell enabled-extensions \"['wps@wps-linux', 'wps-probe@local']\"
  gsettings set org.gnome.shell disable-user-extensions false
  gsettings set org.gnome.shell welcome-dialog-last-shown-version '999'
  gnome-shell --headless --wayland --no-x11 --virtual-monitor 1280x800 --wayland-display wps-hl > '$T/shell.log' 2>&1 &
  SH=\$!
  sleep 5
  [ -n \"\$PROBE_MODE\" ] || WAYLAND_DISPLAY=wps-hl '$REPO/build/wps-shell' '$T/desk/Projects' 2> '$T/folderwin.log' &
  if [ -n \"\$KILLTEST\" ]; then sleep 1; pkill -KILL -f -- 'wps-shell --deskto[p]'; fi
  sleep 4
  echo late > '$T/desk/added-later.txt'
  sleep ${SECS:-9}
  kill \$SH; sleep 1; kill -9 \$SH 2>/dev/null
" 
"$REPO/build/wpsctl" shutdown >/dev/null 2>&1; wait $WP 2>/dev/null
grep -E "wps:|probe:|selftest:|dnd:|timing:|JS ERROR|ERROR: AddressSanitizer|runtime error|wps-ui" "$T/shell.log" | grep -v "^$" | head -40
ls -la "$T/shot.png" 2>/dev/null
echo "T=$T"
# anything of this session still running, and its mounts
for p in $(pgrep -u "$(id -u)"); do tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -q "XDG_RUNTIME_DIR=$RT" && kill -9 $p 2>/dev/null; done
for m in $(grep -o "$RT/[^ ]*" /proc/mounts); do fusermount3 -u "$m" 2>/dev/null; done
rm -rf "$RT"
