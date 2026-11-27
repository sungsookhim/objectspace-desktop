#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Objectspace's pointers (0.7 step 8): the theme's files (every name
# programs ask for, at 24, 32, 48 and 64); headless, the compositor's
# pointer from it (drawn into the screenshot: Objectspace's arrow, white
# with a dark edge); a GTK 4 program asking for a pointer by name
# (cursor-shape-v1) drawn by the compositor; the System's Pointers set to
# the system's: the compositor's from the default theme (Adwaita's arrow,
# dark).
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-cursors-XXXX")
W="$PWD/build/osdctl"
TH=build/cursor-themes/Objectspace
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# every name: each an Xcursor file (or a link to one) with every size
names="default left_ptr arrow top_left_arrow wait watch progress left_ptr_watch pointer hand hand1 hand2 text xterm ibeam
move fleur all-scroll grabbing ns-resize n-resize s-resize row-resize sb_v_double_arrow top_side bottom_side
ew-resize e-resize w-resize col-resize sb_h_double_arrow left_side right_side nwse-resize nw-resize se-resize
top_left_corner bottom_right_corner nesw-resize ne-resize sw-resize top_right_corner bottom_left_corner
not-allowed no-drop crossed_circle crosshair cross"
bad=$(python3 - "$TH/cursors" $names <<'EOF'
import os, struct, sys
d = sys.argv[1]
for n in sys.argv[2:]:
    p = os.path.join(d, n)
    try:
        b = open(p, "rb").read()
    except OSError:
        print(n, "missing"); continue
    if b[:4] != b"Xcur":
        print(n, "not an Xcursor file"); continue
    nt = struct.unpack_from("<I", b, 12)[0]
    sizes = {struct.unpack_from("<III", b, 16 + 12 * i)[1] for i in range(nt)}
    if not {24, 32, 48, 64} <= sizes:
        print(n, "sizes", sorted(sizes))
EOF
)
check "the theme: every name programs ask for, at 24, 32, 48 and 64" "[ -z \"$bad\" ] && grep -q 'Inherits=Adwaita' '$TH/index.theme'" || echo "  $bad"
check "... the busy clock turns (frames)" "python3 -c \"
import struct; b = open('$TH/cursors/wait', 'rb').read(); n = struct.unpack_from('<I', b, 12)[0]
exit(0 if n >= 4 * 8 else 1)\""
cat > "$S/script" <<EOS
3.5 move 640 390
3.8 move 642 392
4.2 move 900 500
4.5 shot $S/ours.png
5 quit
EOS
ENV="OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER="
OSD_COMP_SOFTWARE_CURSOR=1 DESKENV="$ENV" SECS=7 tests/comp/run.sh "$S/script" \
    "sleep 1; GDK_BACKEND=wayland GSK_RENDERER=cairo zenity --info --text Shapes 2>/dev/null" > "$S/out.txt" 2>&1
# the System's Pointers: the system's (the compositor told by the desktop)
cat > "$S/script2" <<EOS
6 move 900 500
6.5 shot $S/system.png
7 quit
EOS
OSD_COMP_SOFTWARE_CURSOR=1 XCURSOR_THEME=Adwaita DESKENV="$ENV" SECS=9 tests/comp/run.sh "$S/script2" \
    "sleep 2; sys=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdSystem']['address'])\"); '$W' set \"\$sys\" 'cursors=system' > \"\$T/set.log\" 2>&1" > "$S/out2.txt" 2>&1
# the pointer's pixels at 900,500: light (white) and dark ones
px() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
bg = im.getpixel((960, 560))
w = d = 0
for x in range(900, 916):
    for y in range(500, 524):
        p = im.getpixel((x, y))
        if sum(abs(a - b) for a, b in zip(p, bg)) < 40: continue
        if sum(p) > 600: w += 1
        elif sum(p) < 150: d += 1
print(w, d)"; }
o=$(px "$S/ours.png"); s=$(px "$S/system.png")
echo "  the pointer's light and dark pixels: Objectspace's $o, the system's $s"
check "the compositor's pointer: Objectspace's (white, a dark edge)" "grep -q 'pointers: Objectspace' '$S/out.txt' && (set -- $o; [ \$1 -gt 60 ] && [ \$1 -gt \$2 ])"
check "a GTK 4 program asked for a pointer by name: drawn by the compositor" "grep -q 'client cursor shape: ' '$S/out.txt'"
check "the System's Pointers: the system's (the default theme's: dark)" "grep -q 'pointers: Adwaita' '$S/out2.txt' && (set -- $s; [ \$2 -gt \$1 ])"
T1=$(sed -n 's/^T=//p' "$S/out.txt"); T2=$(sed -n 's/^T=//p' "$S/out2.txt")
check "no sanitizer reports" "! grep -q Sanitizer '$T1/shell.log' '$T2/shell.log' '$T1/comp.log' '$T2/comp.log'"
echo "$fails failed ($S)"
if [ $fails = 0 ]; then rm -rf "$S" "$T1"; rm -rf "$T2"; fi
exit $fails
