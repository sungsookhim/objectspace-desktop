#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# The pointer's implicit grab, headless: a rubber band started in a folder
# window and let go outside it ends (the window still gets the release),
# instead of following the pointer afterwards.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-ptr-XXXX")
# Projects (empty) opens at 320,96 (640x480); its list: x 325..938, y 123..548.
cat > "$S/script" <<EOS
2.5 dclick 54 30
4 down 500 300
4.3 move 700 450
4.6 move 1100 650
4.8 shot $S/during.png
4.9 up 1100 650
5.2 move 800 500
5.6 shot $S/after.png
6 quit
EOS
DESKENV="OSD_THEBAR=1 OSD_NO_GSETTINGS=1 OSD_NO_LAUNCHER=1" SECS=9 tests/comp/run.sh "$S/script" true > "$S/out.txt" 2>&1
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# pixels of the empty list that are not its white background
marks() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
print(sum(1 for x in range(330, 935) for y in range(128, 545) if im.getpixel((x, y)) != (255, 255, 255)))"; }
check "the band shows while dragged out of the window" "[ \$(marks '$S/during.png') -gt 50 ]"
check "let go outside: the band is gone" "[ \$(marks '$S/after.png') -eq 0 ]"
echo "$fails failed (screenshots: $S)"
[ $fails = 0 ]
