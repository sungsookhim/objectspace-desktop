#!/bin/sh
# Icons, headless: System's own icon, set while the desktop runs (as its
# notebook does), shows on the desktop's shadow of it at once (it showed
# only after wpsd started again).
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-icons-XXXX")
python3 -c "from PIL import Image; Image.new('RGB', (64, 64), (220, 20, 20)).save('$S/red.png')"
cat > "$S/script" <<EOS
3 shot $S/before.png
7 shot $S/after.png
7.5 quit
EOS
DESKENV="OSD_WARPCENTER=1 OSD_NO_GSETTINGS=1 OSD_NO_LAUNCHPAD=1" SECS=10 tests/comp/run.sh "$S/script" \
    "sleep 4; '$REPO/build/wpsctl' settings_set \"\$T/xdg/data/wps/System\" icon='$S/red.png' > \"\$T/set.log\" 2>&1" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# red pixels in the desktop's top rows (where its icons are)
reds() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
print(sum(1 for x in range(0, 1280, 2) for y in range(0, 120, 2) if im.getpixel((x, y)) == (220, 20, 20)))"; }
check "the icon was set" "grep -q '\"ok\":true' '$T/set.log'"
check "before: no red icon" "[ \$(reds '$S/before.png') -eq 0 ]"
check "after: the shadow on the desktop shows it" "[ \$(reds '$S/after.png') -gt 50 ]"
echo "$fails failed (screenshots: $S, logs: $T)"
[ $fails = 0 ]
