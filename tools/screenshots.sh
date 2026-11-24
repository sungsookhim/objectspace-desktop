#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# The README's screenshots (0.5 step 6), taken headless with the compositor
# and the desktop as the session runs them: a new desktop with TheBar, a
# folder window, a settings notebook. Written to docs/screenshots (or OUT).
#   tools/screenshots.sh [OUT]        (after make session)
# My System is titled plainly (its first-start title has the computer's name).
cd "$(dirname "$0")/.."
REPO=$PWD
OUT=$(readlink -f "${1:-docs/screenshots}")
mkdir -p "$OUT"
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-shots-XXXX")
trap 'rm -rf "$S"' EXIT
W="$REPO/build/osdctl"
# Projects (its icon at 54,30) opens at 320,106; its own icon in it at
# 372,176 (Photos): its menu, then its Properties (420,214); the pointer
# parked away for each shot.
cat > "$S/script" <<EOS
5 dclick 54 30
7 move 1000 700
7.5 shot $OUT/desktop.png
8 click 372 176 right
8.7 shot $OUT/menu.png
9 click 420 214
10.5 move 1000 700
11 shot $OUT/notebook.png
11.5 quit
EOS
PRE="mkdir -p \"\$T/desk/Projects/Reports\" \"\$T/desk/Projects/Photos\"
     echo a > \"\$T/desk/Projects/plan.txt\"; echo b > \"\$T/desk/Projects/notes.md\"; echo c > \"\$T/desk/Projects/budget.ods\"
     printf '#!/bin/sh\n' > \"\$T/desk/Projects/build.sh\"; chmod +x \"\$T/desk/Projects/build.sh\"
     cp '$REPO/data/icons/computer.png' \"\$T/desk/Projects/screen.png\"" \
DESKENV="OSD_THEBAR=1 OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER= TZ=UTC" SECS=15 \
    tests/comp/run.sh "$S/script" "sleep 2.5; '$W' set \"\$T/xdg/data/objectspace/System\" 'title=My System' > \"\$T/title.log\" 2>&1; sleep 30" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
cat "$T/title.log" 2>/dev/null
[ -n "$T" ] && rm -rf "$T"
ls -l "$OUT"/desktop.png "$OUT"/menu.png "$OUT"/notebook.png
