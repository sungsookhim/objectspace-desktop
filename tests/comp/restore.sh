#!/bin/sh
# Open windows restored at login (v0.2-plan step 7), headless: a first
# login opens Projects and drags it; the desktop records it. Then the list
# gets a details view of a given size, a Find window and a notebook, and a
# second login (the same test folder) opens them all, where they were.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-restore-XXXX")
cat > "$S/one" <<EOS
3 dclick 54 30
5 down 500 117
5.2 move 400 150
5.4 move 300 217
5.6 up 300 217
9 quit
EOS
printf '5 windows\n6 quit\n' > "$S/two"
export DESKENV="WPS_NO_GSETTINGS=1 WPS_NO_LAUNCHPAD=1"
SECS=12 tests/comp/run.sh "$S/one" "sleep 1" > "$S/out1" 2>&1
T=$(sed -n 's/^T=//p' "$S/out1")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
check "the moved folder window is recorded, with its place" "grep -q \"^F	$T/desk/Projects	0	640	480	120	206	0\$\" '$T/state/open-windows'"
cp "$T/state/open-windows" "$S/recorded"
{
    printf 'F\t%s\t1\t500\t300\t40\t60\t0\n' "$T/desk"
    printf 'S\t%s\tnotes\t700\t90\n' "$T/desk"
    printf 'N\t%s\t1\t300\t400\n' "$T/desk/notes.txt"
} >> "$T/state/open-windows"
KEEP_T=$T SECS=9 tests/comp/run.sh "$S/two" "sleep 1" > "$S/out2" 2>&1
check "Projects reopens where it was" "grep -q \"window 'Projects - Icon View' app org.wps.shell 640x480+120+206\" '$S/out2'"
check "a details view at its size and place" "grep -q \"window '.* - Details View' app org.wps.shell 500x300+40+60\" '$S/out2'"
check "the Find window, its place" "grep -q \"window 'Find' app org.wps.Find [0-9x]*+700+90\" '$S/out2'"
check "the notebook, its place" "grep -q \"window 'notes.txt - Properties' app org.wps.notebook [0-9x]*+300+400\" '$S/out2'"
echo "$fails failed (logs: $S, $T)"
[ $fails = 0 ]
