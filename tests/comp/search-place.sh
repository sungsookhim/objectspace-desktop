#!/bin/sh
# The search field's place (0.4 step 5), headless: a folder's View page
# Search field at the bottom (a row over the status line), then floating
# (a box over the list's lower right, changed while shown), which stays
# where it is when the list scrolls.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-sp-XXXX")
W="$REPO/build/osdctl"
cat > "$S/script" <<EOS
3 dclick 54 30
4.5 key ctrl+f
5.5 shot $S/bottom.png
8 shot $S/floating.png
8.5 click 948 450
9.3 shot $S/scrolled.png
9.8 quit
EOS
PRE="for i in \$(seq 300); do echo x > \"\$T/desk/Projects/f\$i.txt\"; done; '$W' settings_set \"\$T/desk/Projects\" search_place=1 > \"\$T/set.log\" 2>&1" \
DESKENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=12 tests/comp/run.sh "$S/script" \
    "sleep 6; '$W' settings_set \"\$T/desk/Projects\" search_place=2 >> \"\$T/set.log\" 2>&1; sleep 10" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# Projects opens at 320,96 (640x480): the list's left at 323, its status
# line from about 562. The face colour (204) marks the field.
face() { python3 -W ignore -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
exit(0 if im.getpixel(($2, $3)) == (204, 204, 204) else 1)"; }
# the face colour's rows down a column (the floating box: where it is)
rows() { python3 -W ignore -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
ys = [y for y in range(400, 560) if im.getpixel(($2, y)) == (204, 204, 204)]
print(ys[0] if ys else 0, ys[-1] if ys else 0)"; }
check "both settings taken" "[ \$(grep -c '\"ok\":true' '$T/set.log') -eq 2 ]"
check "bottom: the row over the status line, none under the title bar" "face '$S/bottom.png' 345 545 && ! face '$S/bottom.png' 345 137"
f=$(rows "$S/floating.png" 900); s=$(rows "$S/scrolled.png" 900)
check "floating: a box over the list's lower right ($f), no row at the bottom's left" "[ \"$f\" != '0 0' ] && ! face '$S/floating.png' 345 545"
check "scrolled: the box where it was ($s)" "[ \"$s\" = \"$f\" ]"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ]
