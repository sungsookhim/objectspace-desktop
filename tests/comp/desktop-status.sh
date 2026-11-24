#!/bin/sh
# The desktop's status line (0.4 step 5), headless: a message (notes.txt
# moved to the trash) in a box at the bottom of the desktop, gone after a
# few seconds and not back when the selection changes; an action not done
# (Messages) shown there too; a click closes it. A script's (osdctl say)
# shown; the Messages object's setting off: WPS's own not shown, a
# script's still.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-dm-XXXX")
W="$REPO/build/osdctl"
cat > "$S/script" <<EOS
3 click 124 40
3.5 key Delete
5 key Return
6.5 shot $S/msg.png
12.5 click 54 30
13 shot $S/gone.png
15 shot $S/action.png
15.5 click 640 770
16.3 shot $S/clicked.png
17.8 shot $S/said.png
18 quit
EOS
PRE="'$W' settings_set \"\$T/desk/Projects\" 'on:object.open=call:osdNoSuchMethod' > /dev/null 2>&1" \
DESKENV="OSD_NO_LAUNCHPAD=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=21 tests/comp/run.sh "$S/script" \
    "sleep 13.5; '$W' open \"\$T/desk/Projects\" > /dev/null 2>&1; sleep 3.5; '$W' say 'Backup finished' > \"\$T/say.log\" 2>&1; sleep 10" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# the box: the face colour (204) in the middle of the bottom (the desktop
# is 1280x800 here: the box over y 756..784)
box() { python3 -W ignore -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
print(sum(1 for x in range(400, 880, 4) for y in range(756, 784, 2) if im.getpixel((x, y)) == (204, 204, 204)))"; }
check "moved to the trash: the box" "[ \$(box '$S/msg.png') -gt 50 ]"
check "after a few seconds: gone (a click on an icon does not bring it back)" "[ \$(box '$S/gone.png') -eq 0 ]"
check "an action not done: the box again" "[ \$(box '$S/action.png') -gt 50 ]"
check "clicked: gone" "[ \$(box '$S/clicked.png') -eq 0 ]"
check "osdctl say: the box" "grep -q '\"ok\":true' '$T/say.log' && [ \$(box '$S/said.png') -gt 50 ]"
# the setting off (the Messages object's): deleting shows nothing; a script's still shows
cat > "$S/off" <<EOS
3 click 124 40
3.5 key Delete
5 key Return
6.5 shot $S/off-msg.png
8 shot $S/off-said.png
8.5 quit
EOS
PRE="m=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdMessages']['address'])\"); '$W' settings_set \"\$m\" desktop_line=false > /dev/null 2>&1" \
DESKENV="OSD_NO_LAUNCHPAD=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=11 tests/comp/run.sh "$S/off" \
    "sleep 7; '$W' say 'Backup finished' > /dev/null 2>&1; sleep 10" > "$S/off.txt" 2>&1
check "the setting off: moved to the trash, no box" "[ \$(box '$S/off-msg.png') -eq 0 ]"
check "the setting off: a script's message still shown" "[ \$(box '$S/off-said.png') -gt 50 ]"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ]
