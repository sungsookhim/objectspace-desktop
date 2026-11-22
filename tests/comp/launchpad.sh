#!/bin/sh
# The LaunchPad and the WarpCenter in 0.3 (v0.3-plan step 10), headless:
# tooltips after resting on a LaunchPad button and on the clock; Super
# pressed alone opens the WPS menu, Super with a click in between does not.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-lp-XXXX")
# (the LaunchPad: 351x74+520+718; its first object button at 700..754 x 733..789)
cat > "$S/script" <<EOS
3 move 720 758
3.2 move 721 758
4.5 shot $S/tip-lp.png
5 move 1250 785
5.2 move 1251 785
6.5 shot $S/tip-clock.png
7 move 640 300
7.2 key Super_L
8 shot $S/super.png
8.5 click 640 300
9 shot $S/closed.png
9.1 press Super_L
9.2 click 640 300
9.4 release Super_L
10 shot $S/super-click.png
10.5 windows
11 quit
EOS
DESKENV="WPS_WARPCENTER=1 WPS_NO_GSETTINGS=1 WPS_LAUNCHPAD=1" SECS=13 tests/comp/run.sh "$S/script" true > "$S/out.txt" 2>&1
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
px() { python3 -c "from PIL import Image; print('%d %d %d' % Image.open('$1').convert('RGB').getpixel(($2, $3)))"; }
yellow() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
ok = lambda p: p[0] >= 250 and p[1] >= 250 and 215 <= p[2] <= 232   # (the tooltip's pale yellow)
exit(0 if any(ok(im.getpixel((x, y))) for x in range($2, $3, 2) for y in range($4, $5, 2)) else 1)"; }
check "a LaunchPad button's tooltip (above it)" "yellow '$S/tip-lp.png' 690 780 690 735"
check "the clock's tooltip: the date" "yellow '$S/tip-clock.png' 1080 1280 740 770"
check "Super alone: the WPS menu (above the Desktop button)" "[ \"\$(px '$S/super.png' 150 700)\" = '204 204 204' ]"
check "the menu closed by a click elsewhere" "[ \"\$(px '$S/closed.png' 150 700)\" != '204 204 204' ]"
check "Super with a click: no menu" "[ \"\$(px '$S/super-click.png' 150 700)\" != '204 204 204' ]"
echo "$fails failed (screenshots: $S)"
[ $fails = 0 ]
