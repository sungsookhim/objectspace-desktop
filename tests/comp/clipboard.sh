#!/bin/sh
# The clipboard between WPS's text fields and other programs (wl-copy and
# wl-paste, from wl-clipboard), headless: text copied in a folder's search
# field is pasted elsewhere; text copied elsewhere is pasted in the field.
cd "$(dirname "$0")/../.."
command -v wl-copy >/dev/null && command -v wl-paste >/dev/null || { echo "skipped: wl-clipboard is not installed"; exit 0; }
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-clip-XXXX")
# Projects (icon at 54,30) opens with its window focused: Ctrl+F, its search field.
cat > "$S/script" <<EOS
3 dclick 54 30
5 key ctrl+f
5.3 key a
5.4 key b
5.5 key c
5.8 key ctrl+a
6 key ctrl+c
9 key ctrl+a
9.2 key ctrl+v
9.5 key ctrl+a
9.7 key ctrl+c
12 quit
EOS
DESKENV="WPS_WARPCENTER=1 WPS_NO_LAUNCHPAD=1 WPS_NO_GSETTINGS=1" SECS=15 tests/comp/run.sh "$S/script" \
    "sleep 7; wl-paste -n > '$S/out1.txt' 2>&1; wl-copy xyz; sleep 4; wl-paste -n > '$S/out2.txt' 2>&1" > "$S/out.txt" 2>&1
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
check "copied in WPS, pasted by another program" "[ \"\$(cat '$S/out1.txt')\" = abc ]"
check "copied by another program, pasted in WPS (and copied out again)" "[ \"\$(cat '$S/out2.txt')\" = xyz ]"
echo "$fails failed ($S)"
[ $fails = 0 ]
