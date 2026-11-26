#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# One desktop across all screens (0.7 step 3), headless: two screens side
# by side (1280x800, then 1024x768 to its right). The wallpaper on both;
# the desktop's icons on the first (the primary); Projects dragged (with
# button 2, OS/2's) onto
# the second stays there; the second unplugged: Projects shown on the
# first (at its right edge); a screen plugged in again where the second
# was: Projects back on it (its place kept).
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-deskscr-XXXX")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
cat > "$S/script" <<EOS
5 move 1000 600
5.5 shot $S/first.png HEADLESS-1
6 shot $S/second.png HEADLESS-2
6.5 down 44 40 right
6.8 move 400 200 right
7.1 move 1200 250 right
7.4 move 1500 300 right
7.7 up 1500 300 right
8.5 move 1000 600
9 shot $S/moved.png HEADLESS-2
9.5 unplug HEADLESS-2
11 shot $S/gone.png HEADLESS-1
11.5 plug 1024x768
13.5 windows
14 shot $S/back.png HEADLESS-3
14.5 quit
EOS
SIZE=1280x800,1024x768 DESKENV="OSD_THEBAR=1 OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=17 \
    tests/comp/run.sh "$S/script" "sleep 30" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
# pixels near a colour in an area (every other pixel)
count() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
r, g, b = $6
x1, y1 = min($4, im.size[0]), min($5, im.size[1])
print(sum(1 for x in range($2, x1, 2) for y in range($3, y1, 2) if all(abs(a - c) < 40 for a, c in zip(im.getpixel((x, y)), (r, g, b)))))"; }
TEAL='(0, 128, 128)'
YELLOW='(240, 200, 60)'
echo "  yellow (folder) pixels: first $(count "$S/first.png" 0 0 300 120 "$YELLOW"), moved $(count "$S/moved.png" 120 200 360 380 "$YELLOW"), gone $(count "$S/gone.png" 1080 160 1280 440 "$YELLOW"), back $(count "$S/back.png" 120 200 360 380 "$YELLOW")"
check "the wallpaper on the second screen too" "[ \$(count '$S/second.png' 0 0 1024 768 '$TEAL') -gt 150000 ]"
check "the desktop's icons on the first (Projects at its top left), none on the second" "[ \$(count '$S/first.png' 0 0 300 120 '$YELLOW') -gt 40 ] && [ \$(count '$S/second.png' 0 0 1024 768 '$YELLOW') -lt 5 ]"
check "Projects dragged onto the second: there" "[ \$(count '$S/moved.png' 120 200 360 380 '$YELLOW') -gt 40 ]"
check "the second unplugged: Projects on the first, at its right edge" "[ \$(count '$S/gone.png' 1080 160 1280 440 '$YELLOW') -gt 40 ]"
check "a screen there again: Projects back on it" "[ \$(count '$S/back.png' 120 200 360 380 '$YELLOW') -gt 40 ]"
check "no sanitizer reports" "! grep -q Sanitizer '$T/shell.log'"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ] && rm -rf "$S" "$T"
exit $fails
