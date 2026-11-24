#!/bin/sh
# Work Area folders, part 1 (0.4 step 9), headless (WPS session): Projects
# is a work area; two test programs opened from it are minimized and
# restored with its window, closed with it, and opened again when it
# opens again; what was open at the end (logout) is kept on the folder.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-workarea-XXXX")
W="$PWD/build/wpsctl"
C="$PWD/build/tests/ssd-client"
# Projects (its icon at 54,30) opens at 320,106 (640x480): two.desktop at
# 460,176, one.desktop at 372,176 (Two first: its window then covers only
# its own icon); its minimize button at 903,118, its close button at 943,118.
cat > "$S/one" <<EOS
3 dclick 54 30
4.5 dclick 460 176
6 dclick 372 176
7.5 windows
7.7 click 903 118
8.7 windows
9 dclick 54 30
10 windows
10.5 click 943 118
12 windows
13 dclick 54 30
15 windows
15.5 quit
EOS
PRE="printf '[Desktop Entry]\nType=Application\nName=One\nExec=$C One 3060a0\n' > \"\$T/desk/Projects/one.desktop\"
     printf '[Desktop Entry]\nType=Application\nName=Two\nExec=$C Two a03060\n' > \"\$T/desk/Projects/two.desktop\"
     '$W' settings_set \"\$T/desk/Projects\" 'work_area=1' > /dev/null" \
    env -u WPS_SESSION DESKENV="WPS_NO_GSETTINGS=1 WPS_NO_LAUNCHPAD=1" SECS=18 tests/comp/run.sh "$S/one" "sleep 60" > "$S/one.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/one.txt")
cp "$T/wpsd.log" "$S/wpsd1.log"   # (the second run writes its own)
# what the folder kept when wpsd stopped (as at logout)
KEEP_T=$T NODESK=1 PRE="'$W' get \"\$T/desk/Projects\" work_area_open > \"\$T/kept.txt\" 2>&1" SECS=4 \
    tests/comp/run.sh "$S/one" true > "$S/two.txt" 2>&1
# Each "windows" answer: its lines up to the desktop's.
python3 - "$S/one.txt" > "$S/blocks.txt" <<'EOP'
import sys
blocks, cur = [], []
for l in open(sys.argv[1]):
    if l.startswith("wps-compositor: window '"):
        cur.append(l.strip())
        if "app org.wps.Desktop" in l: blocks.append(cur); cur = []
for i, b in enumerate(blocks):
    for l in b:
        if "'One'" in l or "'Two'" in l: print(i, l.split("'")[1], "minimized" if l.endswith("minimized") else "shown")
EOP
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
check "two programs opened from the work area" "[ \$(grep -c '^0 ' '$S/blocks.txt') = 2 ]"
check "minimized with its window" "[ \$(grep -c '^1 .* minimized' '$S/blocks.txt') = 2 ]"
check "restored with it" "[ \$(grep -c '^2 .* shown' '$S/blocks.txt') = 2 ]"
check "closed with it" "! grep -q '^3 ' '$S/blocks.txt'"
check "opened again when it opens again" "[ \$(grep -c '^4 .* shown' '$S/blocks.txt') = 2 ]"
check "what was open at the end is kept on the folder" "grep -q one.desktop '$T/kept.txt' && grep -q two.desktop '$T/kept.txt'"
check "no sanitizer reports" "! grep -q 'Sanitizer' '$S/wpsd1.log' '$T/shell.log'"
[ $fails = 0 ] && rm -rf "$S" "$T" || echo "logs: $S $T"
exit $fails
