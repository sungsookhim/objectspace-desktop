#!/bin/sh
# Positions from 0.2 (0.4 step 5), headless: two icons placed over each
# other (their long titles overlapping): the folder's status line offers
# Arrange (a link), nothing moved; the link arranges them; opened again,
# the offer is not made again (the folder's arrange_offered).
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-ao-XXXX")
W="$REPO/build/osdctl"
# Projects opens at 320,96 (640x480); its status line at y 562..584, the
# link after the offer's words (about x 611..660).
cat > "$S/script" <<EOS
3 dclick 54 30
5 shot $S/offer.png
5.5 click 635 572
7 shot $S/arranged.png
7.5 key alt+F4
8.5 dclick 54 30
10.5 shot $S/again.png
11 quit
EOS
P='$T/desk/Projects'
PRE="cd \"$P\" && touch a-rather-long-file-name-one.txt a-rather-long-file-name-two.txt &&
     '$W' place \"$P\" \"$P/a-rather-long-file-name-one.txt\" 0 0 \"$P/a-rather-long-file-name-two.txt\" 60 0 > /dev/null 2>&1" \
DESKENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=14 tests/comp/run.sh "$S/script" \
    "sleep 4.5; '$W' layout \"$P\" > \"\$T/before.log\" 2>&1; sleep 2.5; '$W' layout \"$P\" > \"\$T/after.log\" 2>&1;
     '$W' props \"$P\" > \"\$T/props.log\" 2>&1; sleep 10" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# the link: the selection colour (a dark blue) in the status line
link() { python3 -W ignore -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
print(sum(1 for x in range(330, 950) for y in range(562, 584) if (lambda p: p[2] > 100 and p[0] < 60)(im.getpixel((x, y)))))"; }
# the two icons' x, as stored
xs() { python3 -c "
import json
d = json.load(open('$1'))['result']['items']
print(*sorted(int(i.get('x', -1)) for i in d if i['name'].startswith('a-rather')))"; }
check "the offer: an Arrange link in the status line" "[ \$(link '$S/offer.png') -gt 20 ]"
b=$(xs "$T/before.log"); a=$(xs "$T/after.log")
check "nothing moved unasked ($b)" "[ '$b' = '0 60' ]"
check "the link: arranged, far enough apart ($a)" "[ \$(( \${a#* } - \${a% *} )) -ge 100 ]"
check "the offer gone" "[ \$(link '$S/arranged.png') -eq 0 ]"
check "kept on the folder: offered once" "python3 -c \"import json,sys; exit(0 if any(p['name'] == 'arrange_offered' and p['value'] in ('1', 'true') for p in json.load(open('$T/props.log'))['result']) else 1)\""
check "opened again: not offered again" "[ \$(link '$S/again.png') -eq 0 ]"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ]
