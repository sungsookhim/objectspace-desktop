#!/bin/sh
# One GUI self-test (WPS_SHELL_SELFTEST=MODE): a private wpsd, then the
# ASan shell in that mode on a fresh folder <D> prepared by SETUP (run in
# <D>). Run it inside a compositor that shows its windows, e.g.
#   tests/gnome/with-gnome.sh -- tests/gui/selftest.sh live 'echo a > a.txt'
# Settings: SHELL_ARGS (before <D>, e.g. --desktop), TEMPLATES=1 (<D>/Templates
# is the templates folder, opened as a second window), WPS_WALLPAPER,
# WPS_SHELL_SHOT=file.png (with WPS_UI_NO_LAYER=1), KEEP=1 (keep <D> and the logs).
# Modes and their set-ups: see the self-tests in gui/shell/wps-shell.c.
#
# usage: tests/gui/selftest.sh MODE [SETUP]
set -e
cd "$(dirname "$0")/../.."
W=$(mktemp -d "${TMPDIR:-/tmp}/wps-selftest-$1-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/wps-st-XXXX")
# Remove the folder at the end (KEEP=1 keeps it, with the logs): big
# self-tests make hundreds of thousands of files.
[ -n "$KEEP" ] || trap 'rm -rf "$W"' EXIT
mkdir -p "$W/d" "$W/xdg/config" "$W/xdg/data/applications" "$W/xdg/sys"
printf '#!/bin/sh\nsleep 2\n' > "$W/hold.sh"; chmod +x "$W/hold.sh"
printf '[Desktop Entry]\nType=Application\nName=Hold\nExec=%s %%f\n' "$W/hold.sh" > "$W/xdg/data/applications/wps-hold.desktop"
printf '[Default Applications]\ntext/plain=wps-hold.desktop\n' > "$W/xdg/config/mimeapps.list"
[ -n "$2" ] && (cd "$W/d" && sh -c "$2")
export WPS_TEMPLATES_DIR="$W/templates"   # never the user's own
[ -n "$TEMPLATES" ] && export WPS_TEMPLATES_DIR="$W/d/Templates" && SHELL_ARGS="$W/d/Templates"
export WPS_CONFIG_DIR="$W/config" WPS_STATE_DIR="$W/state" WPS_RUNTIME_DIR="$RT" WPS_SAVE_DELAY_MS=100
XDG_CONFIG_HOME="$W/xdg/config" XDG_DATA_HOME="$W/xdg/data" XDG_CONFIG_DIRS="$W/xdg/sys" build/wpsd 2> "$W/wpsd.log" &
P=$!
for i in $(seq 50); do build/wpsctl ping >/dev/null 2>&1 && break; sleep 0.1; done
WPS_SELFTEST_TRASH="$W/xdg/data/Trash" WPS_SHELL_SELFTEST=$1 XDG_CACHE_HOME="$W/cache" timeout 60 build/wps-shell-asan ${SHELL_ARGS:-} "$W/d" 2>&1 | grep -E "selftest|ERROR|Sanitizer|runtime error" || true
build/wpsctl shutdown >/dev/null 2>&1 || true
wait $P || true
grep -E "ERROR|runtime error" "$W/wpsd.log" || true
rmdir "$RT" 2>/dev/null || true
