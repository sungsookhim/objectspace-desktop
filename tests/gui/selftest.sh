#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# One GUI self-test (OSD_SHELL_SELFTEST=MODE): a private osdd, then the
# ASan shell in that mode on a fresh folder <D> prepared by SETUP (run in
# <D>). Run it inside a compositor that shows its windows, e.g.
#   tests/gnome/with-gnome.sh -- tests/gui/selftest.sh live 'echo a > a.txt'
# Settings: SHELL_ARGS (before <D>, e.g. --desktop), TEMPLATES=1 (<D>/Templates
# is the templates folder, opened as a second window), OSD_WALLPAPER,
# OSD_SHELL_SHOT=file.png (with OSD_UI_NO_LAYER=1), KEEP=1 (keep <D> and the logs),
# AFTER="sub ..." (folders in <D> opened after it, as more windows),
# SELFTEST_SECS (the shell's time limit, 60).
# Modes and their set-ups: see the self-tests in gui/shell/osd-shell.c.
#
# usage: tests/gui/selftest.sh MODE [SETUP]
set -e
cd "$(dirname "$0")/../.."
W=$(mktemp -d "${TMPDIR:-/tmp}/osd-selftest-$1-XXXX")
RT=$(mktemp -d "$XDG_RUNTIME_DIR/osd-st-XXXX")
# Remove the folder at the end (KEEP=1 keeps it, with the logs): big
# self-tests make hundreds of thousands of files.
[ -n "$KEEP" ] || trap 'rm -rf "$W"' EXIT
mkdir -p "$W/d" "$W/xdg/config" "$W/xdg/data/applications" "$W/xdg/sys"
printf '#!/bin/sh\nsleep 2\n' > "$W/hold.sh"; chmod +x "$W/hold.sh"
printf '[Desktop Entry]\nType=Application\nName=Hold\nExec=%s %%f\n' "$W/hold.sh" > "$W/xdg/data/applications/osd-hold.desktop"
printf '[Default Applications]\ntext/plain=osd-hold.desktop\n' > "$W/xdg/config/mimeapps.list"
[ -n "$2" ] && (cd "$W/d" && sh -c "$2")
export OSD_TEMPLATES_DIR="$W/templates"   # never the user's own
export OSD_SETUP_DIR="$W/setup"            # nor their settings objects
export OSD_LAUNCHER_DIR="$W/launcher"    # nor their Launcher
[ -n "$TEMPLATES" ] && export OSD_TEMPLATES_DIR="$W/d/Templates" && SHELL_ARGS="$W/d/Templates"
export LSAN_OPTIONS="suppressions=$PWD/tests/gui/lsan.supp:print_suppressions=0"
export OSD_CONFIG_DIR="$W/config" OSD_STATE_DIR="$W/state" OSD_RUNTIME_DIR="$RT" OSD_SAVE_DELAY_MS=100
XDG_CONFIG_HOME="$W/xdg/config" XDG_DATA_HOME="$W/xdg/data" XDG_CONFIG_DIRS="$W/xdg/sys" build/osdd 2> "$W/osdd.log" &
P=$!
for i in $(seq 50); do build/osdctl ping >/dev/null 2>&1 && break; sleep 0.1; done
OSD_SELFTEST_TRASH="$W/xdg/data/Trash" OSD_SHELL_SELFTEST=$1 XDG_CACHE_HOME="$W/cache" timeout ${SELFTEST_SECS:-60} build/osd-shell-asan ${SHELL_ARGS:-} "$W/d" $(for a in $AFTER; do printf "%s " "$W/d/$a"; done) 2>&1 | grep -E "selftest|ERROR|Sanitizer|runtime error${SELFTEST_GREP:+|$SELFTEST_GREP}" || true
build/osdctl shutdown >/dev/null 2>&1 || true
wait $P || true
grep -E "ERROR|runtime error" "$W/osdd.log" || true
rmdir "$RT" 2>/dev/null || true
