#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Sharp X11 programs on scaled screens (0.7 step 4), headless, a screen
# of 1280x800 at scale 2 (640x400 logical). On (the default): XWayland's
# root is the screen's pixels (1280x800); an X11 window of 360x240 pixels
# is drawn as 360x240 on the screen (180x120 logical: not stretched); a
# click in its middle reaches it there (about 180,120 in its pixels).
# Off (OSD_X11_SCALE=0): the root 640x400, the window stretched to
# 720x480 pixels, a click in its middle still about 180,120. An X11 menu
# (one taking the keyboard, as xterm's) closed: the keyboard back to its
# window.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-xscale-XXXX")
X="$PWD/build/tests/x11-client"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# one run: NAME, then the compositor's settings
run() {
    n=$1; shift
    cat > "$S/$n.script" <<EOS
4 windows
4.2 shot $S/$n.png
EOS
    env "$@" OSD_SCALE=2 NODESK=1 SECS=8 tests/comp/run.sh "$S/$n.script" "sleep 1; '$X' Sharp 3060a0 2> '$S/$n.client'" > "$S/$n.txt" 2>&1
}
run on
run off OSD_X11_SCALE=0
# the window's box (logical): W H X Y
box() { grep "window 'Sharp'" "$S/$1.txt" | tail -1 | sed -E 's/.* ([0-9]+)x([0-9]+)\+(-?[0-9]+)\+(-?[0-9]+).*/\1 \2 \3 \4/'; }
# its colour's extent on the screen (pixels): W H
extent() { python3 -c "
from PIL import Image
im = Image.open('$S/$1.png').convert('RGB')
pts = [(x, y) for x in range(0, im.width, 2) for y in range(0, im.height, 2) if im.getpixel((x, y)) == (0x30, 0x60, 0xa0)]
xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
print(max(xs) - min(xs) + 2, max(ys) - min(ys) + 2) if pts else print(0, 0)"; }
# a click in the window's client area: its middle, for each run
for n in on off; do
    set -- $(box $n)
    cx=$(( $3 + $1 / 2 )); cy=$(( $4 + $2 / 2 + 10 ))
    printf '4 click %s %s\n4.5 quit\n' $cx $cy > "$S/$n-click.script"
    env $( [ $n = off ] && echo OSD_X11_SCALE=0 ) OSD_SCALE=2 NODESK=1 SECS=7 tests/comp/run.sh "$S/$n-click.script" "sleep 1; '$X' Sharp 3060a0 2> '$S/$n-click.client'" > "$S/$n-click.txt" 2>&1
done
# an X11 menu (xterm's: it takes the keyboard while it shows): closed, the
# window has the keyboard again
printf '4 click 320 200\n5 key a\n5.3 quit\n' > "$S/menu.script"
OSD_SCALE=2 NODESK=1 SECS=7 tests/comp/run.sh "$S/menu.script" "sleep 1; '$X' Menu 3060a0 menu 2> '$S/menu.client'" > "$S/menu.txt" 2>&1
echo "  on: root $(sed -n 's/.*root //p' "$S/on.client"), box $(box on), drawn $(extent on), $(grep -o 'pressed at .*' "$S/on-click.client")"
echo "  off: root $(sed -n 's/.*root //p' "$S/off.client"), box $(box off), drawn $(extent off), $(grep -o 'pressed at .*' "$S/off-click.client")"
near() { p=$(grep -o 'pressed at [0-9]*,[0-9]*' "$S/$1-click.client" | head -1 | sed 's/pressed at //'); x=${p%,*}; y=${p#*,}; [ -n "$p" ] && [ $x -ge 150 ] && [ $x -le 210 ] && [ $y -ge 90 ] && [ $y -le 150 ]; }
check "on: XWayland's root in the screen's pixels (1280x800)" "grep -q 'root 1280x800' '$S/on.client'"
check "on: the window 360x240 pixels on the screen (not stretched)" "[ \"\$(extent on)\" = '360 240' ]"
check "on: a click in its middle reaches it there (about 180,120)" "near on"
check "off: the root logical (640x400)" "grep -q 'root 640x400' '$S/off.client'"
check "off: the window stretched (720x480 pixels)" "[ \"\$(extent off)\" = '720 480' ]"
check "off: a click in its middle (about 180,120)" "near off"
check "an X11 menu closed: the window has the keyboard again" "grep -q 'menu closed' '$S/menu.client' && grep -q 'key 38' '$S/menu.client'"
check "the compositor said the scale" "grep -q 'X11 programs at scale 2' '$S/on.txt'"
echo "$fails failed ($S)"
if [ $fails = 0 ]; then
    for f in "$S"/*.txt; do t=$(sed -n 's/^T=//p' "$f"); [ -n "$t" ] && rm -rf "$t"; done
    rm -rf "$S"
fi
exit $fails
