#!/bin/sh
# Schemes as objects (docs/object-properties-design.md 2.3), headless: the
# Classic scheme object's desktop colour changed while the desktop runs; a
# scheme of the user's made in the Schemes folder and chosen in the Scheme
# Palette; its title bar colour on a folder window's frame.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-schemes-XXXX")
W="$REPO/build/osdctl"
# Projects (54,30) opens at 320,96: its title bar at y 105.
cat > "$S/script" <<EOS
3 dclick 54 30
5.5 move 700 700
6 shot $S/one.png
9.5 shot $S/two.png
10 quit
EOS
DESKENV="OSD_THEBAR=1 OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=13 tests/comp/run.sh "$S/script" \
    "sleep 4
     classic=\$('$W' setup | python3 -c \"import json,sys; print([s['address'] for s in json.load(sys.stdin)['result']['schemes'] if s['title']=='Classic'][0])\")
     '$W' set \"\$classic\" 'desktop=#800000' > \"\$T/set1.log\" 2>&1
     sleep 3
     '$W' create \"\$T/setup/Schemes\" OsdScheme Mine > \"\$T/create.log\" 2>&1
     mine=\$('$W' setup | python3 -c \"import json,sys; print([s['address'] for s in json.load(sys.stdin)['result']['schemes'] if s['title']=='Mine'][0])\")
     '$W' set \"\$mine\" 'desktop=#000080;title_active=#008000' > \"\$T/set2.log\" 2>&1
     pal=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdSchemePalette']['address'])\")
     '$W' set \"\$pal\" 'scheme=Mine' > \"\$T/set3.log\" 2>&1
     '$W' props \"\$pal\" > \"\$T/pal.log\" 2>&1" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
px() { python3 -c "from PIL import Image; print('%d %d %d' % Image.open('$1').convert('RGB').getpixel(($2, $3)))"; }
check "Classic's desktop colour set" "grep -q '\"ok\":true' '$T/set1.log'"
check "... the desktop shows it at once" "[ \"\$(px '$S/one.png' 1000 400)\" = '128 0 0' ]"
check "a scheme of the user's made in Schemes" "grep -q '\"ok\":true' '$T/create.log'"
check "... chosen in the Scheme Palette (it lists it)" "grep -q '\"ok\":true' '$T/set3.log' && grep -q 'Classic,Dark,High contrast,Mine' '$T/pal.log'"
check "... its desktop colour" "[ \"\$(px '$S/two.png' 1000 400)\" = '0 0 128' ]"
check "... its active title bar on the folder window" "[ \"\$(px '$S/two.png' 600 105)\" = '0 128 0' ]"
echo "$fails failed (screenshots: $S, logs: $T)"
[ $fails = 0 ]
