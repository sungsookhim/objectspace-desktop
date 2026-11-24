#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Icon themes (0.4 step 2), headless: the desktop in Objectspace (flat); all folders
# switched to Objectspace (outline) from the System object, live; one folder with
# a theme of its own (Classic) while the desktop keeps outline.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-themes-XXXX")
W="$REPO/build/osdctl"
cat > "$S/script" <<EOS
4 move 700 600
4.5 shot $S/flat.png
7.5 shot $S/outline.png
8 dclick 54 30
11 move 1200 700
11.5 shot $S/own.png
12 quit
EOS
DESKENV="OSD_THEBAR=1 OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=15 tests/comp/run.sh "$S/script" \
    "sleep 5; sys=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdSystem']['address'])\")
     '$W' set \"\$sys\" 'icon_theme=Objectspace (outline)' > \"\$T/set1.log\" 2>&1
     sleep 2; mkdir -p \"\$T/desk/Projects/Inner\"; '$W' set \"\$T/desk/Projects\" 'icon_theme=Classic' > \"\$T/set2.log\" 2>&1
     '$W' props \"\$T/desk/Projects\" > \"\$T/props.log\" 2>&1" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
same() { python3 -W ignore -c "
from PIL import Image, ImageChops
a, b = (Image.open(p).convert('RGB').crop(($3)) for p in ('$1', '$2'))
exit(0 if ImageChops.difference(a, b).getbbox() is None else 1)"; }
check "the System's Icon theme: all folders'" "grep -q '\"ok\":true' '$T/set1.log'"
check "... the desktop's icons change, live (Projects' icon)" "! same '$S/flat.png' '$S/outline.png' 30,10,80,45"
check "a folder's own theme (Classic): its origin, its own" "grep -q '\"name\":\"icon_theme\",\"value\":\"Classic\"[^}]*\"origin\":\"object\"' '$T/props.log'"
# (outline draws dark edges; Classic's folder has none: the desktop's
# Projects at 38,8 and the window's Inner at 361,129, 34 pixels square)
dark() { python3 -W ignore -c "
from PIL import Image
im = Image.open('$1').convert('RGB').crop(($2, $3, $2 + 34, $3 + 34))
print(sum(1 for p in im.getdata() if sum(p) < 200))"; }
check "... its window's icons in Classic, the desktop's in outline" "[ \$(dark '$S/own.png' 38 8) -gt 15 ] && [ \$(dark '$S/own.png' 361 129) -lt 5 ]"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ]
