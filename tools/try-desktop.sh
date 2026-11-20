#!/bin/sh
# Try the WPS desktop without logging out: a nested GNOME Shell in a window
# of your session, with the wps@wps-linux extension, a private wpsd, and
# private settings (your own GNOME settings, extensions and WPS state are
# not touched). The desktop shows FOLDER (default: a scratch folder with a
# few files); close the window to end it.
#
# Needs: sudo dnf install mutter-devkit
# usage: tools/try-desktop.sh [FOLDER] [WIDTHxHEIGHT]   (one screen, of that size)
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
command -v mutter-devkit >/dev/null 2>&1 || [ -x /usr/libexec/mutter-devkit ] || {
    echo "Needs the nested-session viewer: sudo dnf install mutter-devkit" >&2; exit 1; }
for f in "$REPO/build/wps-shell" "$REPO/build/wpsd"; do
    [ -x "$f" ] || { echo "$f is missing: run 'make && make gui' first" >&2; exit 1; }
done
SIZE=${2:-1280x800}
case "$SIZE" in
    *[!0-9x]*|x*|*x|*x*x*|"") echo "size must look like 1600x900, not '$SIZE'" >&2; exit 2 ;;
    *x*) ;;
    *) echo "size must look like 1600x900, not '$SIZE'" >&2; exit 2 ;;
esac
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-try-XXXX")
PARENT_RT=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
PARENT_WL="$PARENT_RT/${WAYLAND_DISPLAY:-wayland-0}"
RT=$(mktemp -d "$PARENT_RT/wps-try-XXXX"); chmod 700 "$RT"
DESK=${1:-$T/Desktop}
if [ -z "$1" ]; then
    mkdir -p "$DESK/Projects"
    echo "Some notes" > "$DESK/notes.txt"
    printf '#!/bin/sh\necho hello\nsleep 3\n' > "$DESK/hello.sh"; chmod +x "$DESK/hello.sh"
fi
mkdir -p "$T/config" "$T/data/gnome-shell/extensions/wps@wps-linux" "$T/cache" "$RT/wps"
E="$T/data/gnome-shell/extensions/wps@wps-linux"
cp "$REPO/gnome-extension/wps@wps-linux/metadata.json" "$REPO/gnome-extension/wps@wps-linux/extension.js" "$E/"
cp -r "$REPO/gnome-extension/wps@wps-linux/schemas" "$E/" && glib-compile-schemas "$E/schemas"
printf '{"command": ["env", "WPS_UI_DEBUG_EXIT=1", "%s", "--desktop", "%s"]}\n' "$REPO/build/wps-shell" "$DESK" > "$E/config.json"

cleanup() {
    "$REPO/build/wpsctl" shutdown >/dev/null 2>&1 || true
    for p in $(pgrep -u "$(id -u)"); do
        { tr '\0' '\n' < /proc/$p/environ; } 2>/dev/null | grep -q "XDG_RUNTIME_DIR=$RT" && kill $p 2>/dev/null
    done
    sleep 1
    for p in $(pgrep -u "$(id -u)"); do
        { tr '\0' '\n' < /proc/$p/environ; } 2>/dev/null | grep -q "XDG_RUNTIME_DIR=$RT" && kill -9 $p 2>/dev/null
    done
    for m in $(grep -o "$RT/[^ ]*" /proc/mounts); do fusermount3 -u "$m" 2>/dev/null; done
    rm -rf "$RT"
    echo "Ended. Logs: $T/shell.log, $T/wpsd.log"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# The viewer shows the nested screen through your PipeWire (its socket is
# in your own runtime folder).
export PIPEWIRE_RUNTIME_DIR="$PARENT_RT"
export XDG_RUNTIME_DIR="$RT" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_CACHE_HOME="$T/cache"
export WPS_CONFIG_DIR="$T/wps/config" WPS_STATE_DIR="$T/wps/state" WPS_RUNTIME_DIR="$RT/wps"
unset DISPLAY JOURNAL_STREAM INVOCATION_ID
"$REPO/build/wpsd" 2> "$T/wpsd.log" &
# The nested shell's viewer opens in your session (by the socket's full path).
WAYLAND_DISPLAY="$PARENT_WL" dbus-run-session -- sh -c "
    gsettings set org.gnome.shell enabled-extensions \"['wps@wps-linux']\"
    gsettings set org.gnome.shell disable-user-extensions false
    gsettings set org.gnome.shell welcome-dialog-last-shown-version '999'
    exec gnome-shell --devkit --devkit-args=\"--monitor-size=$SIZE\" --wayland --no-x11 --wayland-display wps-try
" > "$T/shell.log" 2>&1
