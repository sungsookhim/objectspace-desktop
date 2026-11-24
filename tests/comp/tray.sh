#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# TheBar's tray, headless (v0.2-plan step 8): programs' status
# icons (tray_item.py, on a private session bus) register with the
# desktop's StatusNotifierWatcher. Item A shows its red pixmap, then blue
# when it needs attention; B a theme icon; Passive C nothing. Clicks:
# button 1 Activate, the middle button SecondaryActivate, button 2 A's
# dbusmenu menu as an Objectspace menu, whose Quit reaches the program as an Event.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-tray-XXXX")
cat > "$S/script" <<EOS
2.4 shot $S/start.png
2.5 click 1042 785
3 click 1067 785 middle
3.5 click 1067 785
4.5 click 1042 785 right
5.5 shot $S/menu.png
6 click 1065 759
6.5 shot $S/attention.png
7.5 quit
EOS
DESKENV="OSD_THEBAR=1 OSD_NO_GSETTINGS=1 OSD_NO_LAUNCHER=1" SECS=11 dbus-run-session -- tests/comp/run.sh "$S/script" \
    "python3 '$REPO/tests/comp/tray_item.py' \$T/tray.log 10" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
px() { python3 -c "from PIL import Image; print('%d %d %d' % Image.open('$1').convert('RGB').getpixel(($2, $3)))"; }
check "the watcher says a host is there" "grep -q '^host True' '$T/tray.log'"
check "three items registered" "grep -q '^items 3' '$T/tray.log'"
check "A's pixmap, red, in the tray (C, passive, takes no room)" "[ \"\$(px '$S/start.png' 1042 785)\" = '220 20 20' ]"
check "button 1: Activate" "grep -q '^activate A' '$T/tray.log'"
check "the middle button: SecondaryActivate" "grep -q '^secondary B' '$T/tray.log'"
check "an item registered by its path (Ayatana) is reached" "grep -q '^activate B' '$T/tray.log'"
check "button 2: the menu, as an Objectspace menu" "[ \"\$(px '$S/menu.png' 1040 759)\" != '220 20 20' ] && [ -s '$S/menu.png' ]"
check "a menu choice reaches the program (Event clicked)" "grep -q '^event 5 clicked' '$T/tray.log'"
check "needing attention: the blue pixmap" "[ \"\$(px '$S/attention.png' 1042 785)\" = '20 20 220' ]"
echo "$fails failed (screenshots: $S, logs: $T)"
[ $fails = 0 ]
