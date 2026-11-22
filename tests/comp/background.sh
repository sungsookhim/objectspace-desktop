#!/bin/sh
# Backgrounds, headless (v0.3-plan step 3): the desktop's own picture
# (Use GNOME's background off, stretched), a folder window's colour, then
# its picture centred, then GNOME's background on the desktop again; set
# as their notebooks do (settings_set) while the desktop runs.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-bg-XXXX")
mkdir -p "$S/다운로드"   # (a Korean folder name: the desktop's picture's path is escaped for glycin)
python3 -c "
from PIL import Image
Image.new('RGB', (200, 100), (20, 200, 20)).save('$S/다운로드/green.webp', lossless=True)
Image.new('RGB', (50, 50), (9, 9, 9)).save('$S/x.png')
Image.new('RGB', (100, 100), (20, 20, 220)).save('$S/blue.png')"
# Projects opens at 320,96 (640x480); its list is about x 323..939, y 123..549.
cat > "$S/script" <<EOS
2.5 dclick 54 30
7 shot $S/one.png
10 shot $S/two.png
13 shot $S/three.png
13.5 quit
EOS
W="$REPO/build/wpsctl"
DESKENV="WPS_WARPCENTER=1 WPS_NO_GSETTINGS=1 WPS_NO_LAUNCHPAD=1 WPS_WALLPAPER=" SECS=16 tests/comp/run.sh "$S/script" \
    "sleep 4; '$W' settings_set \"\$T/desk\" bg_gnome=false bg_image='$S/다운로드/green.webp' bg_mode=4 > \"\$T/set.log\" 2>&1;
     '$W' settings_set \"\$T/desk/Projects\" bg_color='#ff00ff' >> \"\$T/set.log\" 2>&1;
     sleep 4; '$W' settings_set \"\$T/desk/Projects\" bg_image='$S/blue.png' bg_mode=0 >> \"\$T/set.log\" 2>&1;
     sleep 3; '$W' settings_set \"\$T/desk\" bg_gnome=true >> \"\$T/set.log\" 2>&1;
     '$W' settings \"\$T/desk/Projects\" > \"\$T/page.log\" 2>&1" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
# The page itself: None takes the picture away; the palette (the colour
# swatch at 590,331) gives a colour: green at 616,381.
cat > "$S/page" <<EOS
3.5 click 600 300 right
4.2 click 650 382
5.5 click 818 204
6.2 click 843 271
7 click 590 331
7.8 shot $S/palette.png
8 click 616 381
11 quit
EOS
DESKENV="WPS_WARPCENTER=1 WPS_NO_GSETTINGS=1 WPS_NO_LAUNCHPAD=1" SECS=14 tests/comp/run.sh "$S/page" \
    "sleep 2.5; '$W' settings_set \"\$T/desk\" bg_image='$S/x.png' > /dev/null; sleep 6.5; '$W' settings \"\$T/desk\" > \"\$T/after.log\"" > "$S/page.txt" 2>&1
T2=$(sed -n 's/^T=//p' "$S/page.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
px() { python3 -c "from PIL import Image; print('%d %d %d' % Image.open('$1').convert('RGB').getpixel(($2, $3)))"; }
check "every setting taken" "[ \$(grep -c '\"ok\":true' '$T/set.log') -eq 4 ]"
check "the desktop: its own picture, stretched" "[ \"\$(px '$S/one.png' 100 650)\" = '20 200 20' ] && [ \"\$(px '$S/one.png' 1270 10)\" = '20 200 20' ]"
check "a folder window: its colour" "[ \"\$(px '$S/one.png' 600 400)\" = '255 0 255' ]"
check "its picture, centred in the list" "[ \"\$(px '$S/two.png' 631 336)\" = '20 20 220' ] && [ \"\$(px '$S/two.png' 340 140)\" = '255 0 255' ]"
check "GNOME's background again (WPS_WALLPAPER empty: a colour, OS/2's teal)" "[ \"\$(px '$S/three.png' 100 650)\" = '0 128 128' ]"
check "None: no picture" "grep -q '\"key\":\"bg_image\"[^}]*\"value\":\"\"' '$T2/after.log'"
check "the palette: a colour picked" "grep -q '\"key\":\"bg_color\"[^}]*\"value\":\"#008000\"' '$T2/after.log'"
check "a folder's page has no Use GNOME's background" "! grep -q bg_gnome '$T/page.log' && grep -q bg_image '$T/page.log'"
echo "$fails failed (screenshots: $S, logs: $T)"
[ $fails = 0 ]
