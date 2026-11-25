#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# A folder window's menu bar (0.6 step 3), headless: on (the folder's View
# page): Folder, Edit, View, Selected, Help under the title bar, Selected
# greyed with nothing selected; View, Details view: the window's view
# changes. Off (the default): no bar; an Alt tap shows it for a moment,
# the keyboard on Folder and no menu open; Right, Down: View's menu;
# Escape: the menu and the bar gone. System's Folders page: Menu bar.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-mb-XXXX")
W="$REPO/build/osdctl"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
px() { python3 -c "from PIL import Image; print('%d %d %d' % Image.open('$1').convert('RGB').getpixel(($2, $3)))"; }
# Projects (its icon at 54,30) opens at 320,106 (640x480): the bar's row
# at y 146 (Folder at 355, View at 444, Selected at 500), white below it;
# View's Details view at 480,218.
cat > "$S/on" <<EOS
3 dclick 54 30
5 move 1000 700
5.5 shot $S/on.png
6 click 444 146
6.8 shot $S/view.png
7 click 480 218
8 windows
8.5 quit
EOS
PRE="'$W' set \"\$T/desk/Projects\" 'menu_bar=1' > /dev/null 2>&1" \
DESKENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=11 tests/comp/run.sh "$S/on" "sleep 20" > "$S/on.txt" 2>&1
T1=$(sed -n 's/^T=//p' "$S/on.txt")
cat > "$S/off" <<EOS
3 dclick 54 30
5 move 1000 700
5.5 shot $S/off.png
6 key Alt_L
6.8 shot $S/alt.png
7 key Right
7.3 key Down
8 shot $S/view2.png
8.3 key Escape
9 shot $S/gone.png
9.5 quit
EOS
DESKENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=12 tests/comp/run.sh "$S/off" \
    "sleep 2; '$W' settings \"\$T/desk/Projects\" > \"\$T/props.log\" 2>&1;
     sys=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdSystem']['address'])\");
     '$W' settings \"\$sys\" > \"\$T/sys.log\" 2>&1; sleep 20" > "$S/off.txt" 2>&1
T2=$(sed -n 's/^T=//p' "$S/off.txt")
# the bar's row: the face colour (not the list's white) at its left
bar() { [ "$(px "$1" 330 146)" != "255 255 255" ] && [ "$(px "$1" 330 146)" = "$(px "$1" 975 146)" ] || [ "$(px "$1" 600 146)" != "255 255 255" ]; }
check "on: the menu bar under the title bar" "bar '$S/on.png'"
# dark text pixels in a title's box on the bar (Folder at 340..380, Selected at 470..535)
dark() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
print(sum(1 for x in range($2, $3) for y in range(138, 156) if sum(im.getpixel((x, y))) < 120))"; }
check "... Selected greyed (nothing selected): no dark text, Folder's dark" "[ \$(dark '$S/on.png' 470 535) -eq 0 ] && [ \$(dark '$S/on.png' 340 380) -gt 10 ]"
check "... View opened: its menu drawn below it" "[ \"\$(px '$S/view.png' 480 250)\" != \"\$(px '$S/on.png' 480 250)\" ]"
check "... View, Details view: the window's view changed" "grep -q \"window 'Projects - Details View'\" '$S/on.txt'"
check "off (the default): no bar, the list right under the title bar" "[ \"\$(px '$S/off.png' 600 146)\" = '255 255 255' ]"
check "... the folder's View page has Menu bar" "grep -q '\"key\":\"menu_bar\"' '$T2/props.log'"
check "... System's Folders page has Menu bar (all folders)" "grep -q '\"key\":\"menu_bar\"' '$T2/sys.log'"
check "... an Alt tap: the bar, no menu open" "[ \"\$(px '$S/alt.png' 600 146)\" != '255 255 255' ] && [ \"\$(px '$S/alt.png' 380 200)\" = \"\$(px '$S/off.png' 380 200)\" ]"
check "... Right, Down: View's menu open" "[ \"\$(px '$S/view2.png' 480 218)\" != \"\$(px '$S/alt.png' 480 218)\" ]"
check "... Escape: the menu and the bar gone" "[ \"\$(px '$S/gone.png' 600 146)\" = '255 255 255' ]"
# Alt+F4 on a second folder window (Sub, opened from Projects over it): it
# closes, and Alt's release reaches Projects, now active - no tap there.
cat > "$S/f4" <<EOS
3 dclick 54 30
5 dclick 372 176
7 windows
7.5 key alt+F4
8.5 windows
9 move 1000 700
9.5 shot $S/f4.png
10 quit
EOS
PRE="mkdir -p \"\$T/desk/Projects/Sub\"" \
DESKENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=13 tests/comp/run.sh "$S/f4" "sleep 20" > "$S/f4.txt" 2>&1
T3=$(sed -n 's/^T=//p' "$S/f4.txt")
check "Alt+F4 on another folder window: it closed" "grep -q \"window 'Sub\" '$S/f4.txt' && [ \$(grep -c \"window 'Sub\" '$S/f4.txt') -lt 3 ]"
check "... and Projects shows no menu bar (Alt's release no tap there)" "[ \"\$(px '$S/f4.png' 600 146)\" = '255 255 255' ]"
check "no sanitizer reports" "! grep -q Sanitizer '$T1/shell.log' '$T2/shell.log' '$T3/shell.log'"
echo "$fails failed (screenshots: $S, logs: $T1 $T2 $T3)"
[ $fails = 0 ] && rm -rf "$S" "$T1" "$T2" "$T3"
exit $fails
