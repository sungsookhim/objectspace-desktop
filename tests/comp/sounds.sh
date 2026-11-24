#!/bin/sh
# sounds.conf (before the Sound object, 0.1) moved into the Sound object at
# the desktop's first start with it, and kept aside as sounds.conf.imported
# (docs/object-properties-design.md 2.4).
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-sounds-XXXX")
W="$REPO/build/osdctl"
printf '6 quit\n' > "$S/script"
PRE="printf '# old settings\nvolume = 40\nwindow.open = bell\nmenu.open = none\n' > \"\$T/config/sounds.conf\"" \
DESKENV="OSD_THEBAR=1 OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1" SECS=9 tests/comp/run.sh "$S/script" \
    "sleep 5; '$W' setup > \"\$T/setup.json\" 2>&1; ls \"\$T/config\" > \"\$T/files.txt\"" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
snd() { python3 -c "import json; print(json.load(open('$T/setup.json'))['result']['objects']['OsdSound']['settings'].get('$1'))"; }
check "the Sound object has sounds.conf's volume" "[ \"\$(snd volume)\" = 40 ]"
check "... and its sounds (none: none)" "[ \"\$(snd window.open)\" = bell ] && [ \"\$(snd menu.open)\" = '' ]"
check "sounds.conf kept aside" "grep -qx sounds.conf.imported '$T/files.txt' && ! grep -qx sounds.conf '$T/files.txt'"
echo "$fails failed (logs: $T)"
[ $fails = 0 ]
