#!/bin/sh
# Icon sizes and the open icon (0.4 step 3), headless: the desktop at the
# Default set (64), switched live to Large (96) and Small (32) from the
# System object; Projects opened: its icon the theme's folder-open while
# its window is open. SCALE=2: the same at a scale of 2.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-sizes-XXXX")
W="$REPO/build/wpsctl"
K=${SCALE:-1}
cat > "$S/script" <<EOS
4 move 1200 700
4.5 shot $S/default.png
7.5 shot $S/large.png
10.5 shot $S/small.png
11 dclick 37 24
13 move 1200 700
13.5 shot $S/open.png
14 quit
EOS
DESKENV="WPS_UI_SCALE=$K WPS_NO_LAUNCHPAD=1 WPS_NO_GSETTINGS=1 WPS_WALLPAPER=" SECS=17 tests/comp/run.sh "$S/script" \
    "sleep 5; sys=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['WPSystem']['address'])\")
     '$W' set \"\$sys\" 'icon_set=Large' > \"\$T/set1.log\" 2>&1
     sleep 3; '$W' set \"\$sys\" 'icon_set=Small' > \"\$T/set2.log\" 2>&1
     '$W' props \"\$T/desk/Projects\" > \"\$T/props.log\" 2>&1" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# The first icon's size (Projects, the top left cell of CELL by ICON):
# the box of the pixels, in its icon's square, not the desktop's colour.
size() { python3 -W ignore -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
k = im.width / 1280
bg = im.getpixel((int(600 * k), int(400 * k)))
box = [int(v * k) for v in (4, 6, 4 + $2, 8 + $3)]
pts = [(x, y) for x in range(box[0], box[2]) for y in range(box[1], box[3])
       if sum(abs(a - b) for a, b in zip(im.getpixel((x, y)), bg)) > 60]
xs, ys = [p[0] for p in pts], [p[1] for p in pts]
print(0 if not pts else round(max(max(xs) - min(xs), max(ys) - min(ys)) / k + 1))"; }
same() { python3 -W ignore -c "
from PIL import Image, ImageChops
a, b = (Image.open(p).convert('RGB') for p in ('$1', '$2'))
k = a.width / 1280
box = tuple(int(v * k) for v in ($3,))
exit(0 if ImageChops.difference(a.crop(box), b.crop(box)).getbbox() is None else 1)"; }
wd=$(size "$S/default.png" 80 64); wl=$(size "$S/large.png" 112 96); ws=$(size "$S/small.png" 56 32)   # (its cell at least: the free layout's)
echo "sizes: small $ws, default $wd, large $wl"
check "the System's Icon size: all folders' (Large, then Small)" "grep -q '\"ok\":true' '$T/set1.log' && grep -q '\"ok\":true' '$T/set2.log'"
check "... a folder reads it as its own set's (inherited)" "grep -q '\"name\":\"icon_set\",\"value\":\"Small\"' '$T/props.log'"
check "Default: the icon about 64" "[ $wd -ge 54 ] && [ $wd -le 66 ]"
check "Large, live: about 96" "[ $wl -ge 82 ] && [ $wl -le 98 ]"
check "Small, live: about 32" "[ $ws -ge 26 ] && [ $ws -le 34 ]"
tx=$(python3 -W ignore -c "
from PIL import Image
im = Image.open('$S/large.png').convert('RGB'); k = im.width / 1280
bg = im.getpixel((int(600 * k), int(400 * k)))
xs = [x for x in range(int(900 * k), im.width) for y in range(int(500 * k), im.height) if sum(abs(a - b) for a, b in zip(im.getpixel((x, y)), bg)) > 60]
print(1 if xs else 0)")
check "Large: the Trash Can's corner kept on the desktop" "[ $tx = 1 ]"
check "open: Projects' icon changes while its window is open" "! same '$S/small.png' '$S/open.png' 18,6,58,42"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ]
