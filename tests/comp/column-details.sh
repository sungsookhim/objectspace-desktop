#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# The Column view's details of a file (0.6.1), headless, with a
# thumbnailer of the test's own (every picture solid red, whatever the
# file): shot.png chosen in the Column view: its picture beside it, large
# (red, not its icon); plan.txt: its icon, large; the folder's Column view
# page with Owner, Permissions, MIME type and Location chosen: four lines
# more beneath (the text goes further down).
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-cdet-XXXX")
W="$PWD/build/osdctl"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# Projects opens at 320,106 (640x480); the first column 325..545 from
# y 160, a row every 20: Photos 170, Reports 190, plan.txt 210, shot.png 230;
# the details from x 545 to the window's edge.
cat > "$S/script" <<EOS
3 dclick 54 30
5 key ctrl+4
6 click 420 210
6.5 move 1000 700
7 shot $S/icon.png
7.5 click 420 230
8.5 move 1000 700
9.5 shot $S/pic.png
10 quit
EOS
# one run: NAME, then the Column view page's settings chosen (KEY=1 ...)
run() {
    n=$1; shift
    sets=""; for kv in "$@"; do sets="${sets:+$sets;}$kv"; done
    PRE="python3 -c \"
from PIL import Image
Image.new('RGB', (64, 48), (20, 20, 220)).save('\$T/desk/Projects/shot.png')\"
     mkdir -p \"\$T/desk/Projects/Reports\" \"\$T/desk/Projects/Photos\"; echo a > \"\$T/desk/Projects/plan.txt\"
     mkdir -p \"\$T/xdg/data/thumbnailers\"
     printf '#!/bin/sh\nexec python3 -c \"from PIL import Image; Image.new(\\\\\"RGB\\\\\", (120, 90), (230, 20, 20)).save(\\\\\"\$2\\\\\")\"\n' > \"\$T/fakethumb.sh\"
     chmod +x \"\$T/fakethumb.sh\"
     printf '[Thumbnailer Entry]\nTryExec=%s\nExec=%s %%i %%o %%s\nMimeType=image/png;\n' \"\$T/fakethumb.sh\" \"\$T/fakethumb.sh\" > \"\$T/xdg/data/thumbnailers/test.thumbnailer\"
     ${sets:+'$W' set \"\$T/desk/Projects\" '$sets' > /dev/null 2>&1}" \
    DESKENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=13 tests/comp/run.sh "$S/script" "sleep 20" > "$S/$n.txt" 2>&1
    mkdir -p "$S/$n"; mv "$S/icon.png" "$S/pic.png" "$S/$n/" 2>/dev/null
}
run plain
run more cd_owner=1 cd_perms=1 cd_mime=1 cd_location=1
# how many pixels of a colour (near) in an area
count() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
r, g, b = $6
print(sum(1 for x in range($2, $4, 2) for y in range($3, $5, 2) if all(abs(a - c) < 40 for a, c in zip(im.getpixel((x, y)), (r, g, b)))))"; }
# pixels not near white (an icon's lines and edges) in an area
dark() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
print(sum(1 for x in range($2, $4, 2) for y in range($3, $5, 2) if sum(im.getpixel((x, y))) < 600))"; }
# the lines of text in the details' labels (bands of dark rows)
bands() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
n, prev = 0, False
for y in range(300, 560):
    d = any(sum(im.getpixel((x, y))) < 450 for x in range(552, 640))
    n, prev = n + (d and not prev), d
print(n)"; }
check "a picture chosen: its thumbnail beside it, large (red)" "[ \$(count '$S/plain/pic.png' 550 165 950 440 '(230, 20, 20)') -gt 4000 ]"
check "a text file chosen: its icon, large (no red)" "[ \$(count '$S/plain/icon.png' 550 165 950 440 '(230, 20, 20)') -lt 5 ] && [ \$(dark '$S/plain/icon.png' 600 180 900 430) -gt 800 ]"
check "four more details chosen: four lines more" "a=\$(bands '$S/plain/pic.png'); b=\$(bands '$S/more/pic.png'); echo \"  (lines \$a, then \$b)\"; [ \$((b - a)) -ge 4 ]"
for n in plain more; do
    T=$(sed -n 's/^T=//p' "$S/$n.txt")
    check "no sanitizer reports ($n)" "! grep -q Sanitizer '$T/shell.log'"
    [ $fails = 0 ] && [ -n "$T" ] && rm -rf "$T"
done
echo "$fails failed (screenshots: $S)"
[ $fails = 0 ] && rm -rf "$S"
exit $fails
