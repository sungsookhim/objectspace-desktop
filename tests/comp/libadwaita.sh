#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# libadwaita programs follow the scheme (0.7 step 6), headless, GNOME's
# settings in a private keyfile (GSETTINGS_BACKEND=keyfile): in the
# Objectspace session at login GNOME is told the scheme (GNOME said dark
# and red: classic's default and blue); the scheme Dark: prefer-dark and
# Dark's accent; High contrast: GNOME's high contrast; GNOME's dark from
# GNOME: the scheme Dark. As inside GNOME (no OSD_SESSION): at start
# GNOME's values win, its accent untouched.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-adw-XXXX")
W="$PWD/build/osdctl"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
export GSETTINGS_BACKEND=keyfile
printf '14 quit\n' > "$S/script"
G='gsettings get org.gnome.desktop.interface'
# the hook: what GNOME and the Scheme Palette say at each point
STEP="pal=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdSchemePalette']['address'])\")
     say() { echo \"\$1 \$($G color-scheme) \$($G accent-color) \$(gsettings get org.gnome.desktop.a11y.interface high-contrast) \$('$W' get \"\$pal\" scheme | python3 -c 'import json,sys; print(json.load(sys.stdin)[\"result\"])' 2>/dev/null)\" >> \"\$T/states\"; }"
PRE="gsettings set org.gnome.desktop.interface color-scheme prefer-dark; gsettings set org.gnome.desktop.interface accent-color red"
ENV="OSD_NO_LAUNCHER=1 OSD_WALLPAPER= GSETTINGS_BACKEND=keyfile"
PRE="$PRE" DESKENV="$ENV OSD_SESSION=1" SECS=16 tests/comp/run.sh "$S/script" "sleep 4; $STEP
     say login
     '$W' set \"\$pal\" 'scheme=dark' > /dev/null; sleep 2; say dark
     '$W' set \"\$pal\" 'scheme=high contrast' > /dev/null; sleep 2; say contrast
     '$W' set \"\$pal\" 'scheme=classic' > /dev/null; sleep 2; say classic
     gsettings set org.gnome.desktop.interface color-scheme prefer-dark; sleep 2; say gnome-dark" > "$S/session.txt" 2>&1
T1=$(sed -n 's/^T=//p' "$S/session.txt")
PRE="$PRE" DESKENV="$ENV" SECS=10 tests/comp/run.sh "$S/script" "sleep 4; $STEP; say start" > "$S/gnome.txt" 2>&1
T2=$(sed -n 's/^T=//p' "$S/gnome.txt")
st() { grep "^$2 " "$1/states" | head -1; }
echo "  session: $(cat "$T1/states" | tr '\n' '|')"
echo "  inside GNOME: $(cat "$T2/states" | tr '\n' '|')"
check "the Objectspace session at login: GNOME told the scheme (classic: default, blue)" "st '$T1' login | grep -q \"^login 'default' 'blue' false classic\""
check "the scheme Dark: prefer-dark, Dark's accent" "st '$T1' dark | grep -q \"^dark 'prefer-dark' '[a-z]*' false dark\""
check "High contrast: GNOME's high contrast" "st '$T1' contrast | grep -q ' true high contrast'"
check "Classic again: default, blue, no high contrast" "st '$T1' classic | grep -q \"^classic 'default' 'blue' false classic\""
check "GNOME's dark from GNOME: the scheme Dark" "st '$T1' gnome-dark | grep -q \"'prefer-dark' .* dark\$\""
check "inside GNOME at start: GNOME's values win (dark), its accent untouched (red)" "st '$T2' start | grep -q \"^start 'prefer-dark' 'red' false dark\""
check "no sanitizer reports" "! grep -q Sanitizer '$T1/shell.log' && ! grep -q Sanitizer '$T2/shell.log'"
echo "$fails failed ($S)"
if [ $fails = 0 ]; then rm -rf "$S" "$T1"; rm -rf "$T2"; fi
exit $fails
