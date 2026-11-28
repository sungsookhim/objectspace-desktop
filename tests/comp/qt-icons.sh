#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Qt programs and the icon theme (0.7 step 9), headless, GNOME's settings
# in a private keyfile (GNOME's own icons "MyIcons"; the user's own qt5ct
# settings with a colour scheme of theirs): in the Objectspace session
# qt6ct's and qt5ct's colour schemes from the scheme (the window's colour
# the GTK 3 theme's too; 22 roles for Qt 6, 21 for Qt 5), qt6ct's settings
# pointed at Classic's, then Dark's; the user's qt5ct settings untouched;
# the icon theme "Objectspace" (every name it maps, at 16 and 48) set as
# GNOME's icon-theme, a GTK 3 file dialog's folders Objectspace's
# (yellow); started again as inside GNOME: MyIcons back.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-qticons-XXXX")
W="$PWD/build/osdctl"
TH=build/share/icons/Objectspace
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
export GSETTINGS_BACKEND=keyfile
gtk3=1
python3 -c "import gi; gi.require_version('Gtk', '3.0'); from gi.repository import Gtk" 2>/dev/null || gtk3=
# the icon theme: every name it maps, at 16 and 48
missing=$(python3 - "$TH" <<'EOF'
import os, sys
sys.path.insert(0, "tools/icons")
from make_xdg_icons import MAP
for ctx, names in MAP.items():
    for n in names:
        for s in (16, 48):
            if not os.path.isfile(os.path.join(sys.argv[1], f"{s}x{s}", ctx, n + ".png")):
                print(f"{s}/{ctx}/{n}")
EOF
)
check "the icon theme: every name it maps, at 16 and 48; Adwaita's for the rest" "[ -z '$missing' ] && grep -q 'Inherits=Adwaita,hicolor' '$TH/index.theme'" || echo "  missing: $missing"
cat > "$S/script" <<EOS
9 move 1270 790
9.5 shot $S/files.png
13 quit
EOS
ENV="OSD_NO_LAUNCHER=1 OSD_WALLPAPER= GSETTINGS_BACKEND=keyfile"
PRE="gsettings set org.gnome.desktop.interface icon-theme MyIcons
     mkdir -p \"\$XDG_CONFIG_HOME/qt5ct\" \"\$T/desk/Projects/Reports\" \"\$T/desk/Projects/Photos\"; echo a > \"\$T/desk/Projects/plan.txt\"
     printf '[Appearance]\ncolor_scheme_path=/my/own.conf\ncustom_palette=true\n' > \"\$XDG_CONFIG_HOME/qt5ct/qt5ct.conf\"" \
DESKENV="$ENV OSD_SESSION=1" SECS=15 tests/comp/run.sh "$S/script" \
    "sleep 4; gsettings get org.gnome.desktop.interface icon-theme > \"\$T/icons.login\"; cp \"\$XDG_CONFIG_HOME/qt6ct/qt6ct.conf\" \"\$T/qt6ct.login\"
     ${gtk3:+GDK_BACKEND=wayland python3 '$PWD/tests/comp/gtk3-client.py' files 20 \"\$T/desk/Projects\" > \"\$T/client.log\" 2>&1 &}
     pal=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdSchemePalette']['address'])\")
     sleep 6; '$W' set \"\$pal\" 'scheme=dark' > /dev/null; sleep 2; cp \"\$XDG_CONFIG_HOME/qt6ct/qt6ct.conf\" \"\$T/qt6ct.dark\"" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
printf '5 quit\n' > "$S/script2"
KEEP_T="$T" DESKENV="$ENV" SECS=7 tests/comp/run.sh "$S/script2" "sleep 4; gsettings get org.gnome.desktop.interface icon-theme > \"\$T/icons.gnome\"" > "$S/out2.txt" 2>&1
C="$T/xdg/config"
roles() { sed -n 's/^active_colors=//p' "$1" | tr ',' '\n' | wc -l; }
role() { sed -n 's/^active_colors=//p' "$1" | tr -d ' ' | cut -d, -f"$2" | sed 's/^#ff/#/'; }
face=$(sed -n 's/^@define-color theme_bg_color \(#[0-9a-f]*\);/\1/p' "$T/xdg/data/themes/Objectspace-classic/gtk-3.0/gtk.css")
echo "  qt6ct Classic: $(roles "$C/qt6ct/colors/Objectspace-classic.conf") roles, Window $(role "$C/qt6ct/colors/Objectspace-classic.conf" 11) (the GTK 3 theme's $face); icon-theme: login $(cat "$T/icons.login") inside GNOME $(cat "$T/icons.gnome")"
check "qt6ct's colour scheme from Classic: 22 roles, the window's colour the scheme's" "[ \$(roles '$C/qt6ct/colors/Objectspace-classic.conf') = 22 ] && [ \"\$(role '$C/qt6ct/colors/Objectspace-classic.conf' 11)\" = '$face' ]"
check "qt5ct's: 21 roles" "[ \$(roles '$C/qt5ct/colors/Objectspace-classic.conf') = 21 ]"
check "qt6ct's settings: Classic's scheme (Fusion), then Dark's" "grep -q 'color_scheme_path=.*/Objectspace-classic.conf' '$T/qt6ct.login' && grep -q 'style=Fusion' '$T/qt6ct.login' && grep -q 'color_scheme_path=.*/Objectspace-dark.conf' '$T/qt6ct.dark'"
check "the user's own qt5ct settings untouched" "grep -q 'color_scheme_path=/my/own.conf' '$C/qt5ct/qt5ct.conf'"
check "the icon theme Objectspace set as GNOME's icon-theme" "grep -q \"'Objectspace'\" '$T/icons.login'"
check "... a copy in the user's icons (run from the build tree: for GNOME's file dialogs, Flatpaks)" "[ -f '$T/xdg/data/icons/Objectspace/index.theme' ] && [ -f '$T/xdg/data/icons/Objectspace/.objectspace-build-copy' ] && [ -f '$T/xdg/data/icons/Objectspace/cursors/default' ]"
check "started inside GNOME: GNOME's own icons back (MyIcons)" "grep -q \"'MyIcons'\" '$T/icons.gnome'"
if [ -n "$gtk3" ]; then
    y=$(python3 -c "
from PIL import Image
im = Image.open('$S/files.png').convert('RGB')
print(sum(1 for x in range(0, 640) for y in range(60, 400) if all(abs(a - b) < 50 for a, b in zip(im.getpixel((x, y)), (242, 196, 44)))))")
    echo "  the file dialog: $y yellow pixels (Objectspace's folders)"
    check "a GTK 3 file dialog: Objectspace's folders (yellow)" "grep -q 'icons Objectspace' '$T/client.log' && [ $y -gt 40 ]"
else
    echo "skip a GTK 3 file dialog (no GTK 3 Python bindings)"
fi
check "no sanitizer reports" "! grep -q Sanitizer '$T/shell.log'"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ] && rm -rf "$S"
[ $fails = 0 ] && rm -rf "$T"
exit $fails
