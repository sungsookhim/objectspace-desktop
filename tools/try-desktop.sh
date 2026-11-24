#!/bin/sh
# Try the Objectspace desktop without logging out: a nested GNOME Shell in a window
# of your session, with the objectspace@sungsookhim.github.io extension, a private osdd, and
# private settings (your own GNOME settings, extensions and Objectspace state are
# not touched). The desktop shows FOLDER (default: a scratch folder with a
# few files); close the window to end it.
#
# Needs: sudo dnf install mutter-devkit
# usage: tools/try-desktop.sh [FOLDER] [WIDTHxHEIGHT]   (one screen, of that size)
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
command -v mutter-devkit >/dev/null 2>&1 || [ -x /usr/libexec/mutter-devkit ] || {
    echo "Needs the nested-session viewer: sudo dnf install mutter-devkit" >&2; exit 1; }
for f in "$REPO/build/osd-shell" "$REPO/build/osdd"; do
    [ -x "$f" ] || { echo "$f is missing: run 'make && make gui' first" >&2; exit 1; }
done
SIZE=${2:-1280x800}
case "$SIZE" in
    *[!0-9x]*|x*|*x|*x*x*|"") echo "size must look like 1600x900, not '$SIZE'" >&2; exit 2 ;;
    *x*) ;;
    *) echo "size must look like 1600x900, not '$SIZE'" >&2; exit 2 ;;
esac
T=$(mktemp -d "${TMPDIR:-/tmp}/osd-try-XXXX")
PARENT_RT=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
PARENT_WL="$PARENT_RT/${WAYLAND_DISPLAY:-wayland-0}"
RT=$(mktemp -d "$PARENT_RT/osd-try-XXXX"); chmod 700 "$RT"
DESK=${1:-$T/Desktop}
if [ -z "$1" ]; then
    mkdir -p "$DESK/Projects"
    echo "Some notes" > "$DESK/notes.txt"
    printf '#!/bin/sh\necho hello\nsleep 3\n' > "$DESK/hello.sh"; chmod +x "$DESK/hello.sh"
fi
mkdir -p "$T/config" "$T/data/gnome-shell/extensions/objectspace@sungsookhim.github.io" "$T/cache" "$RT/objectspace"
E="$T/data/gnome-shell/extensions/objectspace@sungsookhim.github.io"
cp "$REPO/gnome-extension/objectspace@sungsookhim.github.io/metadata.json" "$REPO/gnome-extension/objectspace@sungsookhim.github.io/extension.js" "$E/"
cp -r "$REPO/gnome-extension/objectspace@sungsookhim.github.io/schemas" "$E/" && glib-compile-schemas "$E/schemas"
printf '{"command": ["env", "OSD_UI_DEBUG_EXIT=1", "%s", "--desktop", "%s"]}\n' "$REPO/build/osd-shell" "$DESK" > "$E/config.json"

cleanup() {
    "$REPO/build/osdctl" shutdown >/dev/null 2>&1 || true
    for p in $(pgrep -u "$(id -u)"); do
        { tr '\0' '\n' < /proc/$p/environ; } 2>/dev/null | grep -q "XDG_RUNTIME_DIR=$RT" && kill $p 2>/dev/null
    done
    sleep 1
    for p in $(pgrep -u "$(id -u)"); do
        { tr '\0' '\n' < /proc/$p/environ; } 2>/dev/null | grep -q "XDG_RUNTIME_DIR=$RT" && kill -9 $p 2>/dev/null
    done
    for m in $(grep -o "$RT/[^ ]*" /proc/mounts); do fusermount3 -u "$m" 2>/dev/null; done
    rm -rf "$RT"
    echo "Ended. Logs: $T/shell.log, $T/osdd.log"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# The viewer shows the nested screen through your PipeWire (its socket is
# in your own runtime folder).
export PIPEWIRE_RUNTIME_DIR="$PARENT_RT"
export XDG_RUNTIME_DIR="$RT" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_CACHE_HOME="$T/cache"
export OSD_CONFIG_DIR="$T/objectspace/config" OSD_STATE_DIR="$T/objectspace/state" OSD_RUNTIME_DIR="$RT/objectspace"
unset DISPLAY JOURNAL_STREAM INVOCATION_ID
"$REPO/build/osdd" 2> "$T/osdd.log" &
# The nested shell's viewer opens in your session (by the socket's full path).
WAYLAND_DISPLAY="$PARENT_WL" dbus-run-session -- sh -c "
    gsettings set org.gnome.shell enabled-extensions \"['objectspace@sungsookhim.github.io']\"
    gsettings set org.gnome.shell disable-user-extensions false
    gsettings set org.gnome.shell welcome-dialog-last-shown-version '999'
    exec gnome-shell --devkit --devkit-args=\"--monitor-size=$SIZE\" --wayland --no-x11 --wayland-display osd-try
" > "$T/shell.log" 2>&1
