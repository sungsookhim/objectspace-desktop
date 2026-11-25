#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# TheBar's trays (0.6 step 2), headless: the first tray, Default, on the bar
# (My System, Programs, Command Prompt, Find); a click on an object opens
# it; the tray button goes to the next tray (kept in TheBar's settings);
# an object dragged onto the bar joins the tray shown (a shadow); the
# Trays setting off: the tray's part gone from the bar.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-trays-XXXX")
W="$REPO/build/osdctl"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# The bar (1280x800, at the bottom): the tray button at 266..336 (its
# arrow at 328), the tray's objects from 341, one every 26 pixels (My
# System's at 354), y 785. notes.txt on the desktop at 124,40.
cat > "$S/script" <<EOS
4 move 700 400
4.5 shot $S/first.png
5 click 354 785
7 windows
7.5 key alt+F4
8.5 click 290 785
10 move 700 400
10.5 shot $S/work.png
11 down 124 40 right
11.3 move 200 300
11.6 move 350 785
12 up 350 785 right
14 shot $S/dropped.png
15.5 shot $S/off.png
16 quit
EOS
SETUP="'$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdTheBar']['address'])\""
PRE="mkdir -p \"\$T/xdg/data/objectspace/Trays/Work\"" \
DESKENV="OSD_THEBAR=1 OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=19 tests/comp/run.sh "$S/script" \
    "sleep 13; tb=\$($SETUP); '$W' get \"\$tb\" tray_current > \"\$T/current.txt\" 2>&1;
     '$W' list \"\$T/xdg/data/objectspace/Trays/Work\" 0 50 details > \"\$T/work.json\" 2>&1;
     sleep 1.5; '$W' settings_set \"\$tb\" trays=false > /dev/null 2>&1; sleep 10" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
px() { python3 -c "from PIL import Image; print('%d %d %d' % Image.open('$1').convert('RGB').getpixel(($2, $3)))"; }
# icons drawn: how many different colours in the tray's objects' part
colours() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
print(len({im.getpixel((x, y)) for x in range(343, 445, 2) for y in range(772, 798, 2)}))"; }
check "the first tray's objects drawn on the bar" "[ \$(colours '$S/first.png') -gt 12 ]"
check "a click on My System opens it" "grep -q \"window 'My System\" '$S/out.txt'"
check "the tray button: the next tray (Work), kept in TheBar's settings" "grep -q 'Work' '$T/current.txt'"
check "... an empty tray: nothing drawn but its room" "[ \$(colours '$S/work.png') -lt 8 ]"
check "notes.txt dragged onto the bar: a shadow in the tray shown" \
    "python3 -c \"import json,sys; d = json.load(open('$T/work.json')); sys.exit(0 if any(x.get('shadow') and (x.get('target_path') or '').endswith('/notes.txt') for x in d['result']['items']) else 1)\""
check "... drawn there" "[ \$(colours '$S/dropped.png') -gt 8 ]"
check "... the original stays on the desktop" "[ -f '$T/desk/notes.txt' ]"
check "Trays off: the tray's part gone" "[ \"\$(px '$S/off.png' 300 785)\" = \"\$(px '$S/off.png' 600 785)\" ]"
check "no sanitizer reports" "! grep -q Sanitizer '$T/shell.log'"
echo "$fails failed (screenshots: $S, logs: $T)"
[ $fails = 0 ] && rm -rf "$S" "$T"
exit $fails
