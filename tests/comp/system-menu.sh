#!/bin/sh
# Another program's system menu, drawn by the compositor as the toolkit's
# menus are (0.4 step 8: one menu implementation), in the menu font:
# Minimize from it; with the System's menu font 18.Sans, a taller menu.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-sysmenu-XXXX")
W="$PWD/build/osdctl"
ENV="OSD_NO_GSETTINGS=1 OSD_NO_LAUNCHPAD=1"
CLIENT="'$PWD/build/tests/ssd-client' Framed 3060a0"
# The test client's frame at 436,156: its system menu button at 450,171;
# the menu opens under it (Maximize, Minimize at y 205, -, Close).
cat > "$S/one" <<EOS
3.2 click 450 171
4 shot $S/menu.png
4.2 click 470 205
5 windows
5.5 quit
EOS
env -u OSD_SESSION DESKENV="$ENV" SECS=8 tests/comp/run.sh "$S/one" "$CLIENT" > "$S/one.txt" 2>&1
T1=$(sed -n 's/^T=//p' "$S/one.txt")
cat > "$S/two" <<EOS
3.2 click 450 171
4 shot $S/big.png
4.2 key Escape
4.5 quit
EOS
PRE="a=\$('$W' setup | python3 -c 'import json,sys; print(json.load(sys.stdin)[\"result\"][\"objects\"][\"OsdSystem\"][\"address\"])')
     '$W' settings_set \"\$a\" 'font_menus=18.Sans' > /dev/null" \
    env -u OSD_SESSION DESKENV="$ENV" SECS=8 tests/comp/run.sh "$S/two" "$CLIENT" > "$S/two.txt" 2>&1
T2=$(sed -n 's/^T=//p' "$S/two.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# the menu's face (grey) at a point: x 570 (past its labels), y
face() { python3 -c "from PIL import Image; r,g,b = Image.open('$1').convert('RGB').getpixel((570, $2)); exit(0 if abs(r - g) < 8 and abs(g - b) < 8 and r > 150 else 1)"; }
check "the system menu opens under its button" "face '$S/menu.png' 240"
check "Minimize from it minimizes the program" "grep -q \"window 'Framed' .* minimized\" '$S/one.txt'"
check "the interface font: the menu ends above y 290" "! face '$S/menu.png' 290"
check "the menu font (18.Sans): a taller menu" "face '$S/big.png' 290"
[ $fails = 0 ] && rm -rf "$S" "$T1" "$T2"
exit $fails
