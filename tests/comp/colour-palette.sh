#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# The Colour Palette (0.4 step 7), headless: its shadow on the desktop
# opens it (a shadow of a settings object); a sample (red) dragged with
# button 2 onto the desktop: its colour (GNOME's background no longer
# used); magenta onto a folder window's title bar (on its maximize
# button: the title bar's row): its own, shown while it is active; a
# double click on a sample: Edit Colour.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-cpal-XXXX")
W="$REPO/build/osdctl"
# The shadow (made at 7 s: the desktop's first-start arrangement done, even on a slow machine) lands at 490,40 (after My System and Programs); the palette opens at 412,164: its first
# sample at 447,215, red (the 7th) at 444,255.
# Projects (54,30) opens at 320,106: its title bar at y 121, right of the
# palette at 930; magenta (the 16th sample) at 600,295.
cat > "$S/script" <<EOS
3.8 dclick 54 30
8.5 dclick 490 40
10.3 windows
10.5 down 444 255 right
10.7 move 600 400
10.9 move 1100 650
11.2 up 1100 650 right
11.6 dclick 447 215
12.8 windows
13 key Escape
13.5 down 600 295 right
13.7 move 800 200
13.9 move 930 121
14.2 up 930 121 right
14.6 click 700 121
15.3 move 700 700
15.6 shot $S/red.png
16 quit
EOS
DESKENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=19 tests/comp/run.sh "$S/script" \
    "sleep 7; cp=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdColorPalette']['address'])\"); '$W' shadow \"\$cp\" \"\$T/desk\" > \"\$T/sh.log\" 2>&1; sleep 7.5; '$W' settings \"\$T/desk\" > \"\$T/after.log\" 2>&1; sleep 1; '$W' settings \"\$T/desk/Projects\" > \"\$T/proj.log\" 2>&1; sleep 5" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
px() { python3 -c "from PIL import Image; print('%d %d %d' % Image.open('$1').convert('RGB').getpixel(($2, $3)))"; }
check "its shadow on the desktop opens it" "grep -q \"window 'Colour Palette'\" '$S/out.txt'"
check "red dragged onto the desktop: the desktop red" "[ \"\$(px '$S/red.png' 1100 700)\" = '255 0 0' ]"
check "... kept: its colour, GNOME's background off" "grep -q '\"key\":\"bg_color\"[^}]*\"value\":\"#ff0000\"' '$T/after.log' && grep -q '\"key\":\"bg_gnome\"[^}]*\"value\":false' '$T/after.log'"
check "magenta onto Projects' title bar: its own (active)" "[ \"\$(px '$S/red.png' 800 121)\" = '255 0 255' ]"
check "... kept as Projects' own" "grep -q '\"key\":\"title_bar_color\"[^}]*\"value\":\"#ff00ff\"' '$T/proj.log'"
check "a double click on a sample: Edit Colour" "grep -q \"window 'Edit Colour'\" '$S/out.txt'"
echo "$fails failed (screenshots: $S, logs: $T)"
[ $fails = 0 ]
