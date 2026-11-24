#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Folder windows remember (0.4 step 5), headless in the Objectspace session: a
# folder resized, scrolled down a page and moved, then closed and opened
# again, comes back the same (place, size, scroll).
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-win-XXXX")
# Projects opens at 320,96 (640x480): its bottom right corner near 957,583,
# its title bar at y 121, its scroll bar's trough at x 948.
cat > "$S/script" <<EOS
4 dclick 54 30
6 click 948 450
6.5 down 956 582
6.8 move 930 560
7.1 move 896 542
7.4 up 896 542
8 down 600 121
8.3 move 560 200
8.6 move 460 300
8.9 up 460 300
9.5 move 1200 700
10 shot $S/before.png
10.5 key alt+F4
11.5 shot $S/closed.png
12 dclick 54 30
14 move 1200 700
14.5 shot $S/after.png
15 quit
EOS
PRE='for i in $(seq 300); do echo x > "$T/desk/Projects/f$i.txt"; done' \
DESKENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=18 tests/comp/run.sh "$S/script" true > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# the window: the bounding box of what differs from the desktop with it
# closed, below the desktop's icons (Projects' own shows it open)
box() { python3 -W ignore -c "
from PIL import Image, ImageChops
a, b = Image.open('$1').convert('RGB'), Image.open('$S/closed.png').convert('RGB')
d = ImageChops.difference(a, b).point(lambda v: 255 if v > 30 else 0).crop((0, 100, a.width, a.height)).getbbox()
print(*((d[0], d[1] + 100, d[2], d[3] + 100) if d else (0, 0, 0, 0)))"; }
# (its icons flow to its new width when it opens again: where the first
# icon starts under the title bar is the same if it scrolled back)
first() { python3 -W ignore -c "
from PIL import Image
a = Image.open('$1').convert('L')
print(next((y for y in range($3 + 26, $3 + 300) if any(a.getpixel((x, y)) < 100 for x in range($2 + 30, $2 + 70))), 0))"; }
set -- $(box "$S/before.png"); b="$*"
set -- $(box "$S/after.png"); a="$*"
check "moved and resized: not where it opened ($b)" "[ \"$b\" != '320 106 959 585' ] && [ \"$b\" != '0 0 0 0' ]"
check "opened again in the same place and size ($a)" "[ \"$a\" = \"$b\" ]"
set -- $a
y0=$(first "$S/before.png" $1 $2); y1=$(first "$S/after.png" $1 $2)
check "scrolled as it was (the first icon's top at $y1, was $y0)" "[ $y0 = $y1 ] && [ $y0 != 0 ]"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ]
