#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# More than one screen (0.7 step 2), headless: two screens side by side
# (1280x800, then 1024x768 to its right). TheBar on the first (the
# primary); a window opened with the pointer on the second opens there;
# Super+Shift+Left: to the first, Right: back; Alt+F10 maximized fills the
# second's usable area (all of it: TheBar is on the first); the second
# unplugged: the window on the first, filling its usable area (less
# TheBar); OSD_COMP_PRIMARY=HEADLESS-2: TheBar on the second; GNOME's
# monitors.xml with the second on the left and primary: TheBar there.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-screens-XXXX")
C="$PWD/build/tests/ssd-client"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
cat > "$S/script" <<EOS
2 move 1700 300
6 windows
6.3 shot $S/second.png HEADLESS-2
6.6 key super+shift+Left
7 windows
7.3 key super+shift+Right
7.6 windows
8 key alt+F10
8.5 windows
9 unplug HEADLESS-2
9.8 windows
10.2 quit
EOS
ENV="OSD_THEBAR=1 OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER="
SIZE=1280x800,1024x768 DESKENV="$ENV" SECS=13 tests/comp/run.sh "$S/script" "sleep 4; '$C' Moving 3060a0" > "$S/out.txt" 2>&1
printf '2 windows\n3 quit\n' > "$S/script2"
SIZE=1280x800,1024x768 DESKENV="$ENV" OSD_COMP_PRIMARY=HEADLESS-2 SECS=6 tests/comp/run.sh "$S/script2" "sleep 1" > "$S/out2.txt" 2>&1
# GNOME's arrangement (monitors.xml): the second on the left, the primary
cat > "$S/monitors.xml" <<EOX
<monitors version="2">
  <configuration>
    <layoutmode>logical</layoutmode>
    <logicalmonitor><x>1024</x><y>0</y><scale>1</scale>
      <monitor><monitorspec><connector>HEADLESS-1</connector><vendor>x</vendor><product>x</product><serial>1</serial></monitorspec><mode><width>1280</width><height>800</height><rate>60</rate></mode></monitor>
    </logicalmonitor>
    <logicalmonitor><x>0</x><y>0</y><scale>1</scale><primary>yes</primary>
      <monitor><monitorspec><connector>HEADLESS-2</connector><vendor>x</vendor><product>x</product><serial>2</serial></monitorspec><mode><width>1024</width><height>768</height><rate>60</rate></mode></monitor>
    </logicalmonitor>
  </configuration>
</monitors>
EOX
PRE="mkdir -p \"\$XDG_CONFIG_HOME\"; cp '$S/monitors.xml' \"\$XDG_CONFIG_HOME/\"" SIZE=1280x800,1024x768 DESKENV="$ENV" SECS=6 tests/comp/run.sh "$S/script2" "sleep 1" > "$S/out3.txt" 2>&1
# the window's box in the Nth "windows now" answer: W H X Y
win() { awk -v n="$1" '/windows now:/ { k++ } k == n && /window .Moving./ { print }' "$S/out.txt" | sed -E 's/.* ([0-9]+)x([0-9]+)\+(-?[0-9]+)\+(-?[0-9]+).*/\1 \2 \3 \4/' | head -1; }
bar() { grep -E "window '[^']*' app [^ ]*TheBar" "$1" | tail -1 | sed -E 's/.* ([0-9]+)x([0-9]+)\+(-?[0-9]+)\+(-?[0-9]+).*/\1 \2 \3 \4/'; }
echo "  windows: $(win 1) | $(win 2) | $(win 3) | $(win 4) | $(win 5); TheBar: $(bar "$S/out.txt") then $(bar "$S/out2.txt")"
check "two screens" "grep -q 'screen HEADLESS-2: 1024x768' '$S/out.txt'"
check "TheBar on the first (the primary), its width" "(set -- \$(bar '$S/out.txt'); [ \"\$1\" = 1280 ] && [ \"\$3\" = 0 ])"
check "a window opened with the pointer on the second: there" "(set -- \$(win 1); [ \"\$3\" -ge 1280 ])"
check "Super+Shift+Left: on the first" "(set -- \$(win 2); [ \"\$3\" -lt 1280 ] && [ \"\$3\" -ge 0 ])"
check "Super+Shift+Right: on the second again" "(set -- \$(win 3); [ \"\$3\" -ge 1280 ])"
check "Alt+F10: maximized, the second's whole area" "(set -- \$(win 4); [ \"\$1\" = 1024 ] && [ \"\$2\" = 768 ] && [ \"\$3\" = 1280 ] && [ \"\$4\" = 0 ])"
check "the second unplugged: the window on the first, filling its usable area (less TheBar)" "(set -- \$(win 5); [ \"\$1\" = 1280 ] && [ \"\$3\" = 0 ] && [ \"\$2\" -lt 800 ] && [ \"\$2\" -gt 700 ])"
check "OSD_COMP_PRIMARY=HEADLESS-2: TheBar on the second" "(set -- \$(bar '$S/out2.txt'); [ \"\$1\" = 1024 ] && [ \"\$3\" = 1280 ])"
check "GNOME's arrangement: the second on the left and the primary, TheBar on it" "(set -- \$(bar '$S/out3.txt'); [ \"\$1\" = 1024 ] && [ \"\$3\" = 0 ])"
check "the second screen's picture: the window drawn there" "python3 -c \"
from PIL import Image
im = Image.open('$S/second.png').convert('RGB')
import sys; sys.exit(0 if im.size == (1024, 768) and sum(1 for x in range(0, 1024, 4) for y in range(0, 768, 4) if im.getpixel((x, y)) == (0x30, 0x60, 0xa0)) > 200 else 1)\""
# a window opened maximized (LibreOffice remembers it was): restored, a
# size of its own (the session ended: wlroots aborted on a negative size)
printf '3 windows\n3.5 key alt+F10\n4.5 windows\n5 quit\n' > "$S/script4"
SIZE=3840x2160 NODESK=1 SECS=7 tests/comp/run.sh "$S/script4" "sleep 1; '$C' Opened 3060a0 max" > "$S/out4.txt" 2>&1
check "opened maximized, then restored: a size of its own, the compositor still there" "grep -q \"window 'Opened'.* maximized\" '$S/out4.txt' && grep \"window 'Opened'\" '$S/out4.txt' | tail -1 | grep -qv maximized && ! grep -q 'Assertion' '$S/out4.txt'"
for f in out out2 out3; do T=$(sed -n 's/^T=//p' "$S/$f.txt"); check "no sanitizer reports ($f)" "! grep -q Sanitizer '$T/shell.log' 2>/dev/null"; [ $fails = 0 ] && [ -n "$T" ] && rm -rf "$T"; done
echo "$fails failed ($S)"
[ $fails = 0 ] && rm -rf "$S"
exit $fails
