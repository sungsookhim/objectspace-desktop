#!/bin/sh
# A rename that changes the extension (0.4 step 6), headless: notes.txt
# renamed memo.md asks (Use .md / Keep .txt / Cancel): Keep gives memo.txt;
# renamed memo.md again, Use gives memo.md; renamed memo (the extension
# taken away), Cancel keeps memo.md.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-rext-XXXX")
typed() { echo "$1" | sed 's/./& /g' | awk -v t="$2" '{ for (i = 1; i <= NF; i++) { k = $i == "." ? "period" : $i; printf "%.1f key %s\n", t + i * 0.1, k } }'; }
{
    echo "3 click 124 40"
    echo "3.5 key F2"
    echo "3.8 key ctrl+a"
    typed memo.md 3.8
    echo "5 key Return"
    echo "5.8 windows"
    echo "6 key Return"
    echo "7 click 124 40"
    echo "7.5 key F2"
    echo "7.8 key ctrl+a"
    typed memo.md 7.8
    echo "9 key Return"
    echo "9.8 key Left"
    echo "10 key Return"
    echo "11 click 124 40"
    echo "11.5 key F2"
    echo "11.8 key ctrl+a"
    typed memo 11.8
    echo "13 key Return"
    echo "13.8 key Escape"
    echo "15 quit"
} > "$S/script"
DESKENV="WPS_NO_LAUNCHPAD=1 WPS_NO_GSETTINGS=1 WPS_WALLPAPER=" SECS=18 tests/comp/run.sh "$S/script" \
    "sleep 6.8; ls \"\$T/desk\" > \"\$T/after-keep.txt\"; sleep 4; ls \"\$T/desk\" > \"\$T/after-use.txt\"; sleep 4; ls \"\$T/desk\" > \"\$T/after-cancel.txt\"; sleep 5" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
check "the question: a Rename dialog" "grep -q \"window 'Rename'\" '$S/out.txt'"
check "Keep .txt: memo.txt" "grep -qx memo.txt '$T/after-keep.txt' && ! grep -qx notes.txt '$T/after-keep.txt'"
check "Use .md: memo.md" "grep -qx memo.md '$T/after-use.txt'"
check "the extension taken away, Cancel: still memo.md" "grep -qx memo.md '$T/after-cancel.txt' && ! grep -qx memo '$T/after-cancel.txt'"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ]
