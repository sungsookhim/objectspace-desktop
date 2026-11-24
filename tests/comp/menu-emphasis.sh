#!/bin/sh
# The owner's 0.4 notes, headless: a pop-up menu's object outlined (OS/2's
# source emphasis: an icon's cell; the folder's own area; the desktop's),
# gone when the menu closes; a scroll bar's trough held pages until the
# thumb is under the pointer, a click one page.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-emph-XXXX")
cat > "$S/script" <<EOS
4 dclick 54 30
5.5 move 1200 700
6 shot $S/before.png
6.5 click 124 40 right
7.3 shot $S/item.png
7.5 key Escape
8 click 905 520 right
8.8 shot $S/folder.png
9 key Escape
9.5 click 600 700 right
10.3 shot $S/desk.png
10.5 key Escape
11 move 1200 700
11.5 shot $S/closed.png
12 click 948 450
12.8 shot $S/click.png
13 down 948 500
15.5 shot $S/held.png
15.7 up 948 500
16 quit
EOS
PRE='for i in $(seq 300); do echo x > "$T/desk/Projects/f$i.txt"; done' \
DESKENV="OSD_NO_LAUNCHPAD=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=19 tests/comp/run.sh "$S/script" true > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# the outline: pixels changed from the shot before, in a strip down its
# left side (its black shows grey on a folder's layer)
pxdiff() { python3 -W ignore -c "
from PIL import Image
a, b = Image.open('$1').convert('RGB'), Image.open('$2').convert('RGB')
print(sum(1 for x in range($3, $4) for y in range($5, $6) if sum(abs(p - q) for p, q in zip(a.getpixel((x, y)), b.getpixel((x, y)))) > 60))"; }
# the thumb: its rows in the scroll bar (lighter than the trough)
thumb() { python3 -W ignore -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
ys = [y for y in range(148, 544) if sum(im.getpixel((948, y))) // 3 > 190]
print(min(ys), max(ys))"; }
more() { [ "$(pxdiff "$1" "$2" $3 $4 $5 $6)" -gt 40 ]; }
check "an icon's menu: its cell outlined" "more '$S/before.png' '$S/item.png' 84 90 10 110"
check "the folder's own menu: its area outlined" "more '$S/before.png' '$S/folder.png' 322 330 150 540"
check "the desktop's menu: the desktop outlined" "more '$S/before.png' '$S/desk.png' 0 5 150 680"
check "closed: no outline" "[ \$(pxdiff '$S/before.png' '$S/closed.png' 0 5 150 680) -le 5 ]"
set -- $(thumb "$S/closed.png"); t0=$1
set -- $(thumb "$S/click.png"); t1=$1
set -- $(thumb "$S/held.png"); h0=$1 h1=$2
check "a click in the trough: one page (the thumb $t0 to $t1)" "[ $t1 -gt $t0 ] && [ $t1 -lt 300 ]"
check "held: paged until the thumb is under the pointer ($h0-$h1 at 500)" "[ $h0 -le 500 ] && [ $h1 -ge 500 ]"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ]
