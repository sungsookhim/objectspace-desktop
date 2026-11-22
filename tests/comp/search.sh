#!/bin/sh
# A folder window's search field, headless (v0.3-plan step 5): Ctrl+F shows
# it, typing filters (the matches flow from the top left, in name order),
# Escape takes it away and every icon is back in its place; Ctrl+Shift+F
# is the Find window.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-search-XXXX")
cat > "$S/script" <<EOS
2.5 dclick 54 30
3.5 shot $S/before.png
4 key ctrl+f
4.5 key r
4.7 key e
4.9 key p
6.5 shot $S/typed.png
7 key Escape
8.5 shot $S/after.png
9 key ctrl+shift+f
10 windows
10.5 quit
EOS
PRE='cd "$T/desk/Projects" && touch report-2025.ods report-2026.ods budget.ods notes.txt photo.jpg readme.md letter.odt todo.txt music.ogg && mkdir reports' \
DESKENV="WPS_WARPCENTER=1 WPS_NO_GSETTINGS=1 WPS_NO_LAUNCHPAD=1" SECS=13 tests/comp/run.sh "$S/script" true > "$S/out.txt" 2>&1
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# Projects opens at 320,96 (640x480); the field's row under its title bar
py() { python3 -W ignore -c "from PIL import Image
$1"; }
check "Ctrl+F: the field shows (its row: the face colour under the title bar)" \
    "py \"im = Image.open('$S/typed.png').convert('RGB'); exit(0 if im.getpixel((345, 137)) == (204, 204, 204) else 1)\""
check "typed: the three matches, in the first row only (none below)" \
    "py \"im = Image.open('$S/typed.png').convert('RGB')
top = sum(1 for x in range(330, 940, 3) for y in range(160, 240, 3) if im.getpixel((x, y))[0] < 100)
low = sum(1 for x in range(330, 940, 3) for y in range(250, 520, 3) if im.getpixel((x, y))[0] < 100)
exit(0 if top > 20 and low == 0 else 1)\""
check "Escape: every icon back in its place" \
    "py \"a = Image.open('$S/before.png').crop((330, 150, 950, 560)); b = Image.open('$S/after.png').crop((330, 150, 950, 560)); exit(0 if a.tobytes() == b.tobytes() else 1)\""
check "Ctrl+Shift+F: the Find window" "grep -q \"window 'Find\" '$S/out.txt'"
echo "$fails failed (screenshots: $S)"
[ $fails = 0 ]
