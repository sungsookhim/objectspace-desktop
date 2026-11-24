#!/bin/sh
# The in-use hatching (0.4 step 3, the owner's), headless: an open folder
# shows its open icon unhatched; the System's "Hatch open icons too" adds
# the hatching behind it, live; an object's Properties notebook hatches
# its icon while it is open.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-hatch-XXXX")
W="$REPO/build/osdctl"
cat > "$S/script" <<EOS
4 dclick 54 30
5.5 move 1200 700
6 shot $S/open.png
8.5 shot $S/all.png
9 click 124 40 right
9.6 click 179 78
11 move 1200 700
11.5 shot $S/props.png
12 key alt+F4
13 shot $S/closed.png
13.5 quit
EOS
DESKENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=16 tests/comp/run.sh "$S/script" \
    "sleep 6.5; sys=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdSystem']['address'])\")
     '$W' settings_set \"\$sys\" hatch_open=true > \"\$T/set.log\" 2>&1" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# Hatching covers the desktop's colour round an icon: the pixels of that
# colour in its square (Projects' at 12,8, notes.txt's at 92,8: the free
# layout's cells at the Default size, 64) fewer with it than without.
bg() { python3 -W ignore -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
bg = im.getpixel((1100, 300))   # (the desktop: no window there)
print(sum(1 for x in range($2, $2 + 64) for y in range(8, 72) if sum(abs(a - b) for a, b in zip(im.getpixel((x, y)), bg)) < 30))"; }
check "the System's Hatch open icons too, set" "grep -q '\"ok\":true' '$T/set.log'"
check "an open folder: its open icon unhatched, hatched behind with the System's option (live)" "[ \$(( \$(bg '$S/open.png' 12) - \$(bg '$S/all.png' 12) )) -gt 20 ]"
check "a file's Properties open: its icon hatched; closed: not" "[ \$(( \$(bg '$S/closed.png' 92) - \$(bg '$S/props.png' 92) )) -gt 20 ]"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ]
