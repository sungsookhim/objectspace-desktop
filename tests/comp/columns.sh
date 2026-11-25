#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# The Column view (0.6 step 4), headless: Ctrl+4 shows Projects as a
# column with a path above it; Reports chosen: its contents in the next
# column; a file chosen there: its details beside it (the first column
# out of sight, two fitting); Left: back, the details gone; Right: into
# the next column, its first object chosen; closed and opened again: the
# Column view again (the folder keeps its last view).
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-cols-XXXX")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# Projects opens at 320,106 (640x480): the path at y 147, the columns
# from y 160, 220 wide (the first at 325..545), a row every 20 (Photos
# 170, Reports 190, plan.txt 210); in Reports: 2025 at 170, summary.md at 190.
cat > "$S/script" <<EOS
3 dclick 54 30
5 key ctrl+4
6 move 1000 700
6.5 shot $S/one.png
7 click 420 190
8 move 1000 700
8.3 shot $S/two.png
8.6 click 640 190
9.5 move 1000 700
9.8 shot $S/file.png
10.2 key Left
11 shot $S/left.png
11.3 key Right
12 shot $S/right.png
12.3 windows
12.5 key ctrl+w
14 dclick 54 30
16 windows
16.5 quit
EOS
PRE="mkdir -p \"\$T/desk/Projects/Reports/2025\" \"\$T/desk/Projects/Photos\"; echo a > \"\$T/desk/Projects/plan.txt\"
     echo q > \"\$T/desk/Projects/Reports/2025/q1.txt\"; echo r > \"\$T/desk/Projects/Reports/summary.md\"" \
DESKENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=19 tests/comp/run.sh "$S/script" "sleep 30" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
# dark pixels (text, icons) in an area
dark() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
print(sum(1 for x in range($2, $4, 2) for y in range($3, $5, 2) if sum(im.getpixel((x, y))) < 250))"; }
check "Ctrl+4: the Column view" "grep -q \"window 'Projects - Column View'\" '$S/out.txt'"
check "... one column, the path above it (Projects)" "[ \$(dark '$S/one.png' 330 138 400 156) -gt 5 ] && [ \$(dark '$S/one.png' 560 162 900 230) -lt 3 ]"
check "Reports chosen: its contents in the next column" "[ \$(dark '$S/two.png' 560 162 760 200) -gt 10 ]"
check "a file chosen there: its details beside it" "[ \$(dark '$S/file.png' 560 300 940 400) -gt 20 ]"
check "Left: back a column, the details gone" "[ \$(dark '$S/left.png' 560 300 940 400) -lt 3 ]"
check "Right: into the next column, its first object chosen (2025: its folder opened beside)" "[ \$(dark '$S/right.png' 560 162 940 200) -gt 10 ]"
check "closed and opened again: the Column view again" "grep \"window 'Projects\" '$S/out.txt' | tail -1 | grep -q 'Column View'"
check "no sanitizer reports" "! grep -q Sanitizer '$T/shell.log'"
echo "$fails failed (screenshots: $S, logs: $T)"
[ $fails = 0 ] && rm -rf "$S" "$T"
exit $fails
