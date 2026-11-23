#!/bin/sh
# Compares two runs of tests/gui/golden.sh, mode by mode:
#   tests/gui/golden-compare.sh BASE NEW
# A mode marked unstable in golden-modes.txt is shown but never fails;
# skipped modes are left out. Exit status 1 if a stable mode differs or
# is missing.
cd "$(dirname "$0")/../.."
BASE=${1:?usage: tests/gui/golden-compare.sh BASE NEW}
NEW=${2:?usage: tests/gui/golden-compare.sh BASE NEW}
bad=0
same=0
for mode in $(grep -v '^#' tests/gui/golden-modes.txt | grep -v '^$' | awk -F'\t' '$5 != "skip" {print $1}'); do
    status=$(awk -F'\t' -v m="$mode" '$1 == m {print $5}' tests/gui/golden-modes.txt)
    if [ ! -f "$BASE/$mode.txt" ]; then echo "nobase  $mode"; continue; fi
    if [ ! -f "$NEW/$mode.txt" ]; then echo "MISSING $mode"; bad=$((bad + 1)); continue; fi
    if cmp -s "$BASE/$mode.txt" "$NEW/$mode.txt"; then
        same=$((same + 1))
        echo "same    $mode"
    elif [ "$status" = unstable ]; then
        echo "differ  $mode (unstable: not counted)"
        diff -u "$BASE/$mode.txt" "$NEW/$mode.txt" | sed -n '3,$p' | sed 's/^/        /'
    else
        echo "DIFFER  $mode"
        diff -u "$BASE/$mode.txt" "$NEW/$mode.txt" | sed -n '3,$p' | sed 's/^/        /'
        bad=$((bad + 1))
    fi
done
echo "$same same, $bad differ or missing"
[ $bad = 0 ]
