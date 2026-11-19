#!/bin/sh
# Run COMMAND inside a private, headless GNOME Shell (its own D-Bus,
# dconf, runtime folder and Wayland display; no extensions), then stop it
# and everything it started. For GUI self-tests: they need a compositor
# that shows their windows (GNOME sends no frames to hidden windows), and
# need none of the user's session.
#
# usage: tests/gnome/with-gnome.sh [WIDTHxHEIGHT] -- COMMAND [ARGS...]
#   COMMAND gets WAYLAND_DISPLAY and XDG_RUNTIME_DIR of that session; its
#   exit status is this script's.
SIZE=1280x800
case "$1" in *x*) SIZE=$1; shift ;; esac
[ "$1" = "--" ] && shift
[ $# -gt 0 ] || { echo "usage: $0 [WxH] -- COMMAND..." >&2; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/wps-wg-XXXX")
PARENT_RT=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
RT=$(mktemp -d "$PARENT_RT/wps-wg-XXXX"); chmod 700 "$RT"
mkdir -p "$T/config" "$T/data" "$T/cache"

cleanup() {
    for p in $(pgrep -u "$(id -u)"); do
        { tr '\0' '\n' < /proc/$p/environ; } 2>/dev/null | grep -q "XDG_RUNTIME_DIR=$RT" && kill -9 $p 2>/dev/null
    done
    for m in $(grep -o "$RT/[^ ]*" /proc/mounts); do fusermount3 -u "$m" 2>/dev/null; done
    rm -rf "$RT"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# The command runs with the caller's settings (WPS_*), but this session's
# display and runtime folder; a wpsd it needs must use the same runtime
# folder, so WPS_RUNTIME_DIR is kept as given.
PIPEWIRE_RUNTIME_DIR="$PARENT_RT" XDG_RUNTIME_DIR="$RT" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_CACHE_HOME="$T/cache" \
    dbus-run-session -- sh -c '
        gsettings set org.gnome.shell welcome-dialog-last-shown-version "999"
        gnome-shell --headless --wayland --no-x11 --virtual-monitor "$0" --wayland-display wps-wg > "$1/shell.log" 2>&1 &
        SH=$!
        for i in $(seq 100); do [ -S "$XDG_RUNTIME_DIR/wps-wg" ] && break; sleep 0.1; done
        sleep 2   # past the start-up overview animation
        # leave the overview GNOME starts in: windows there get no frames
        gdbus call --session --dest org.gnome.Shell --object-path /org/gnome/Shell \
            --method org.freedesktop.DBus.Properties.Set org.gnome.Shell OverviewActive "<false>" >/dev/null 2>&1
        shift 1
        WAYLAND_DISPLAY=wps-wg "$@" 2>&3
        st=$?
        kill $SH 2>/dev/null; sleep 1; kill -9 $SH 2>/dev/null
        exit $st
    ' "$SIZE" "$T" "$@" 3>&2 2>"$T/session.log"
