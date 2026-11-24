#!/bin/sh
# Work Area folders (0.4 steps 9 and 10), headless (WPS session): Projects
# is a work area; two test programs, a link to a folder and a shadow of a
# folder opened from it are minimized and restored with its window, closed
# with it, and opened again when it opens again; what was open at the end
# (logout) is kept on the folder, and at the next login it opens again with
# them, its folders still its group's.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-workarea-XXXX")
W="$PWD/build/osdctl"
C="$PWD/build/tests/ssd-client"
# Projects (its icon at 54,30) opens at 320,106 (640x480): away (a link to
# a folder elsewhere) at 372,176, one.desktop at 460,176, two.desktop at
# 548,176, zfolder (a shadow of another folder, made once the desktop
# runs) at 636,176. Its title bar at y 118 stays in view (the programs open
# at 436,156, the folders lower right): a click there raises it again
# before the next icon. Its minimize button at 903,118, close at 943,118.
cat > "$S/one" <<EOS
3 dclick 54 30
4.5 dclick 460 176
6 click 700 118
6.5 dclick 548 176
8 click 700 118
8.5 dclick 372 176
9.5 click 700 118
10 dclick 636 176
11.5 windows
11.7 click 700 118
12 click 903 118
13 windows
13.5 dclick 54 30
14.5 windows
15 click 700 118
15.3 click 943 118
17 windows
18 dclick 54 30
21 windows
21.5 quit
EOS
PRE="mkdir -p \"\$T/away\" \"\$T/zfolder\"; echo z > \"\$T/away/z.txt\"; ln -s \"\$T/away\" \"\$T/desk/Projects/away\"
     printf '[Desktop Entry]\nType=Application\nName=One\nExec=$C One 3060a0\n' > \"\$T/desk/Projects/one.desktop\"
     printf '[Desktop Entry]\nType=Application\nName=Two\nExec=$C Two a03060\n' > \"\$T/desk/Projects/two.desktop\"
     '$W' settings_set \"\$T/desk/Projects\" 'work_area=1' > /dev/null" \
    env -u OSD_SESSION DESKENV="OSD_NO_GSETTINGS=1 OSD_NO_LAUNCHPAD=1" SECS=24 tests/comp/run.sh "$S/one" \
    "sleep 2; '$W' shadow \"\$T/zfolder\" \"\$T/desk/Projects\" > /dev/null; sleep 60" > "$S/one.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/one.txt")
cp "$T/osdd.log" "$S/wpsd1.log"   # (the second run writes its own)
# what the folder kept when osdd stopped (as at logout)
KEEP_T=$T NODESK=1 PRE="'$W' get \"\$T/desk/Projects\" work_area_open > \"\$T/kept.txt\" 2>&1" SECS=4 \
    tests/comp/run.sh "$S/one" true > "$S/two.txt" 2>&1
# The next login, with the System's Restore open windows at login off (a
# work area opens again all the same): Projects and its folders open again
# (the open windows' list), its programs with it (what it kept);
# minimized, they all go.
cat > "$S/three" <<EOS
7 windows
7.3 click 700 118
7.6 click 903 118
8.6 windows
9 quit
EOS
KEEP_T=$T PRE="a=\$('$W' setup | python3 -c 'import json,sys; print(json.load(sys.stdin)[\"result\"][\"objects\"][\"OsdSystem\"][\"address\"])')
     '$W' settings_set \"\$a\" 'restore_windows=0' > /dev/null" \
    env -u OSD_SESSION DESKENV="OSD_NO_GSETTINGS=1 OSD_NO_LAUNCHPAD=1" SECS=12 tests/comp/run.sh "$S/three" "sleep 60" > "$S/three.txt" 2>&1
# Each "windows" answer: its lines after its "windows now:".
cat > "$S/blocks.py" <<'EOP'
import sys
blocks = []
for l in open(sys.argv[1]):
    if l.startswith("osd-compositor: windows now:"): blocks.append([])
    elif l.startswith("osd-compositor: window '") and blocks: blocks[-1].append(l.strip())
for i, b in enumerate(blocks):
    for l in b:
        if "'One'" in l or "'Two'" in l or "'away - " in l or "'zfolder - " in l:
            print(i, l.split("'")[1], "minimized" if l.endswith("minimized") else "shown")
EOP
python3 "$S/blocks.py" "$S/one.txt" > "$S/blocks.txt"
python3 "$S/blocks.py" "$S/three.txt" > "$S/login.txt"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
check "two programs, a linked folder and a shadow of a folder opened from it" "[ \$(grep -c '^0 ' '$S/blocks.txt') = 4 ]"
check "minimized with its window (the folders too)" "[ \$(grep -c '^1 .* minimized' '$S/blocks.txt') = 4 ]"
check "restored with it" "[ \$(grep -c '^2 .* shown' '$S/blocks.txt') = 4 ]"
check "closed with it" "! grep -q '^3 ' '$S/blocks.txt'"
check "opened again when it opens again (the folders too)" "[ \$(grep -c '^4 .* shown' '$S/blocks.txt') = 4 ]"
check "what was open at the end is kept on the folder" "grep -q one.desktop '$T/kept.txt' && grep -q two.desktop '$T/kept.txt'"
check "at the next login: open again with its programs and folders" "[ \$(grep -c '^0 .* shown' '$S/login.txt') = 4 ]"
check "... and they are still its group" "[ \$(grep -c '^1 .* minimized' '$S/login.txt') = 4 ]"
check "no sanitizer reports" "! grep -q 'Sanitizer' '$S/wpsd1.log' '$T/shell.log'"
[ $fails = 0 ] && rm -rf "$S" "$T" || echo "logs: $S $T"
exit $fails
