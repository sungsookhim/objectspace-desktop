#!/bin/sh
# The Launcher's Close (its bar's right-click menu), headless: the
# Launcher (above TheBar: 520..870 x 695..768) gone after it.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-lpc-XXXX")
cat > "$S/script" <<EOS
4 move 700 300
4.3 shot $S/before.png
4.5 click 525 760 right
5.2 click 565 747
6 move 700 300
6.5 shot $S/after.png
7 quit
EOS
DESKENV="OSD_THEBAR=1 OSD_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=10 tests/comp/run.sh "$S/script" "sleep 20" > "$S/out.txt" 2>&1
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
px() { python3 -c "from PIL import Image; print('%d %d %d' % Image.open('$1').convert('RGB').getpixel(($2, $3)))"; }
check "the Launcher up" "[ \"\$(px '$S/before.png' 600 702)\" = '204 204 204' ]"
check "its menu's Close: gone" "[ \"\$(px '$S/after.png' 600 702)\" != '204 204 204' ]"
echo "$fails failed ($S)"
[ $fails = 0 ]
