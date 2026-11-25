#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Thumbnails, Show pictures and the Gallery view (0.6 step 5), headless,
# with a thumbnailer of the test's own (it makes every picture solid red,
# whatever the file: what is drawn is the thumbnail, not the file):
# Projects with Show pictures: shot.png drawn as its thumbnail (red), kept
# in the shared cache (~/.cache/thumbnails/normal/MD5(URI).png, with its
# Thumb::URI and Thumb::MTime); notes.txt by the folder's .osd-thumbnails
# (a picture chosen for it: green). Ctrl+5: the Gallery view, the first
# object shown big (red), the others in the strip beneath.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-pics-XXXX")
W="$PWD/build/osdctl"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# Projects opens at 320,106 (640x480); its first icon's cell from 323,123.
cat > "$S/script" <<EOS
3 dclick 54 30
6 move 1000 700
6.5 shot $S/icons.png
7 key ctrl+5
8.5 move 1000 700
9 shot $S/gallery.png
9.3 windows
9.6 quit
EOS
PRE="python3 -c \"
from PIL import Image
Image.new('RGB', (64, 48), (20, 20, 220)).save('\$T/desk/Projects/shot.png')
Image.new('RGB', (40, 40), (20, 200, 20)).save('\$T/desk/Projects/green.png')\"
     echo note > \"\$T/desk/Projects/notes.txt\"
     printf 'notes.txt\tgreen.png\n' > \"\$T/desk/Projects/.osd-thumbnails\"
     mkdir -p \"\$T/xdg/data/thumbnailers\"
     printf '#!/bin/sh\nexec python3 -c \"from PIL import Image; Image.new(\\\\\"RGB\\\\\", (120, 90), (230, 20, 20)).save(\\\\\"\$2\\\\\")\"\n' > \"\$T/fakethumb.sh\"
     chmod +x \"\$T/fakethumb.sh\"
     printf '[Thumbnailer Entry]\nTryExec=%s\nExec=%s %%i %%o %%s\nMimeType=image/png;\n' \"\$T/fakethumb.sh\" \"\$T/fakethumb.sh\" > \"\$T/xdg/data/thumbnailers/test.thumbnailer\"
     '$W' set \"\$T/desk/Projects\" 'show_pictures=1' > /dev/null 2>&1" \
DESKENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=12 tests/comp/run.sh "$S/script" "sleep 20" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
# how many pixels of a colour (near) in an area
count() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
r, g, b = $6
print(sum(1 for x in range($2, $4, 2) for y in range($3, $5, 2) if all(abs(a - c) < 40 for a, c in zip(im.getpixel((x, y)), (r, g, b)))))"; }
check "Show pictures: shot.png drawn as its thumbnail (red, not its blue)" "[ \$(count '$S/icons.png' 323 123 960 300 '(230, 20, 20)') -gt 20 ] && [ \$(count '$S/icons.png' 323 123 960 300 '(20, 20, 220)') -lt 5 ]"
check "... notes.txt by the folder's .osd-thumbnails (green)" "[ \$(count '$S/icons.png' 323 123 960 300 '(20, 200, 20)') -gt 20 ]"
check "... kept in the shared cache, MD5 of its URI, with the standard's text" "python3 -c \"
import hashlib, sys, urllib.parse
uri = 'file://' + urllib.parse.quote('$T/desk/Projects/shot.png')
f = '$T/cache/thumbnails/normal/' + hashlib.md5(uri.encode()).hexdigest() + '.png'
d = open(f, 'rb').read()
sys.exit(0 if b'Thumb::URI' in d and uri.encode() in d and b'Thumb::MTime' in d else 1)\""
check "Ctrl+5: the Gallery view" "grep -q \"window 'Projects - Gallery View'\" '$S/out.txt'"
check "... one shown big (red: a thumbnail filling the upper part)" "[ \$(count '$S/gallery.png' 323 130 960 420 '(230, 20, 20)') -gt 400 ]"
check "no sanitizer reports" "! grep -q Sanitizer '$T/shell.log'"
echo "$fails failed (screenshots: $S, logs: $T)"
[ $fails = 0 ] && rm -rf "$S" "$T"
exit $fails
