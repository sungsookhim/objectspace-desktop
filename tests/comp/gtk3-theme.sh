#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# A GTK 3 theme from the scheme (0.7 step 7), headless, GNOME's settings in
# a private keyfile (GNOME's own GTK 3 theme "MyTheme"): in the Objectspace
# session at login Objectspace-classic written (the scheme's colours in
# it) and set as GNOME's gtk-theme, MyTheme kept; a GTK 3 window drawn in
# Classic's face and selection colours; the scheme Dark: Objectspace-dark
# set, the same window (running) in Dark's face; High contrast its theme
# too. Started again as inside GNOME: MyTheme back. (The window's checks
# need GTK 3's Python bindings; without them, skipped.)
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-gtk3-XXXX")
W="$PWD/build/osdctl"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
export GSETTINGS_BACKEND=keyfile
gtk3=1
python3 -c "import gi; gi.require_version('Gtk', '3.0'); from gi.repository import Gtk" 2>/dev/null || gtk3=
cat > "$S/script" <<EOS
9 move 1270 790
9.5 shot $S/classic.png
13 move 1260 780
13.5 shot $S/dark.png
16 quit
EOS
G='gsettings get org.gnome.desktop.interface gtk-theme'
ENV="OSD_NO_LAUNCHER=1 OSD_WALLPAPER= GSETTINGS_BACKEND=keyfile"
PRE="gsettings set org.gnome.desktop.interface gtk-theme MyTheme; gsettings set org.gnome.desktop.interface cursor-theme MyCursors" DESKENV="$ENV OSD_SESSION=1" SECS=18 tests/comp/run.sh "$S/script" \
    "sleep 4; $G > \"\$T/theme.login\"; gsettings get org.gnome.desktop.interface cursor-theme > \"\$T/cursor.login\"
     ${gtk3:+GDK_BACKEND=wayland python3 '$PWD/tests/comp/gtk3-client.py' 20 > \"\$T/client.log\" 2>&1 &}
     pal=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdSchemePalette']['address'])\")
     sleep 6; '$W' set \"\$pal\" 'scheme=dark' > /dev/null; sleep 2; $G > \"\$T/theme.dark\"
     sleep 2; '$W' set \"\$pal\" 'scheme=high contrast' > /dev/null; sleep 1.5; $G > \"\$T/theme.contrast\"" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
printf '5 quit\n' > "$S/script2"
KEEP_T="$T" DESKENV="$ENV" SECS=7 tests/comp/run.sh "$S/script2" "sleep 4; $G > \"\$T/theme.gnome\"; gsettings get org.gnome.desktop.interface cursor-theme > \"\$T/cursor.gnome\"" > "$S/out2.txt" 2>&1
TH="$T/xdg/data/themes"
echo "  gtk-theme: login $(cat "$T/theme.login") dark $(cat "$T/theme.dark") contrast $(cat "$T/theme.contrast") inside GNOME $(cat "$T/theme.gnome"); themes: $(ls "$TH" | tr '\n' ' ')"
# the theme's colour for a name (@define-color NAME #rrggbb)
col() { sed -n "s/^@define-color $2 \(#[0-9a-f]*\);/\1/p" "$TH/$1/gtk-3.0/gtk.css"; }
# how many pixels of a colour (exactly, give or take 6) in the window's area
count() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
c = tuple(int('$2'[i:i + 2], 16) for i in (1, 3, 5))
print(sum(1 for x in range(400, 880, 2) for y in range(160, 480, 2) if all(abs(a - b) <= 6 for a, b in zip(im.getpixel((x, y)), c))))"; }
check "at login: Objectspace-classic written (the scheme's colours in it)" "[ -n \"\$(col Objectspace-classic theme_bg_color)\" ] && grep -q 'Adwaita/gtk-contained.css' '$TH/Objectspace-classic/gtk-3.0/gtk.css'"
check "... and set as GNOME's gtk-theme for GTK 3" "grep -q \"'Objectspace-classic'\" '$T/theme.login'"
check "... GNOME's own kept (MyTheme)" "grep -qx MyTheme '$TH/.objectspace-gnome-gtk-theme'"
check "the scheme Dark: Objectspace-dark (Adwaita's dark underneath)" "grep -q \"'Objectspace-dark'\" '$T/theme.dark' && grep -q 'gtk-contained-dark.css' '$TH/Objectspace-dark/gtk-3.0/gtk.css'"
check "High contrast: its theme too" "grep -q \"'Objectspace-high-contrast'\" '$T/theme.contrast' && [ -s '$TH/Objectspace-high-contrast/gtk-3.0/gtk.css' ]"
check "started inside GNOME: GNOME's own back (MyTheme)" "grep -q \"'MyTheme'\" '$T/theme.gnome'"
check "GTK 3's pointers: Objectspace's in the session, GNOME's own back inside GNOME (MyCursors)" "grep -q \"'Objectspace'\" '$T/cursor.login' && grep -q \"'MyCursors'\" '$T/cursor.gnome'"
if [ -n "$gtk3" ]; then
    face=$(col Objectspace-classic theme_bg_color); sel=$(col Objectspace-classic theme_selected_bg_color); dface=$(col Objectspace-dark theme_bg_color)
    cf=$(count "$S/classic.png" "$face"); cs=$(count "$S/classic.png" "$sel"); df=$(count "$S/dark.png" "$dface"); dc=$(count "$S/dark.png" "$face")
    echo "  window: Classic's face $face $cf px, selection $sel $cs px; then Dark's face $dface $df px (Classic's $dc)"
    check "a GTK 3 window: Classic's face and selection" "grep -q 'theme Objectspace-classic' '$T/client.log' && [ $cf -gt 5000 ] && [ $cs -gt 300 ]"
    check "... the scheme Dark: the running window in Dark's face" "[ $df -gt 5000 ] && [ $dc -lt 1000 ]"
else
    echo "skip a GTK 3 window (no GTK 3 Python bindings)"
fi
check "no sanitizer reports" "! grep -q Sanitizer '$T/shell.log'"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ] && rm -rf "$S"
[ $fails = 0 ] && rm -rf "$T"
exit $fails
