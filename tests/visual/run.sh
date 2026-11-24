#!/bin/sh
# The looks, headless, for the refactor (docs/object-properties-design.md 4):
# one fixed scene (the desktop with the WarpCenter and the LaunchPad, the
# desktop's menu, a folder window, an object's menu, the desktop's notebook
# and its Background page) in each scheme, saved as PNGs in OUT.
#   tests/visual/run.sh OUT          (then tests/visual/compare.py BASE OUT)
# make visual-baseline / make visual-check do both with build/visual.
cd "$(dirname "$0")/../.."
REPO=$PWD
mkdir -p "${1:?usage: tests/visual/run.sh OUT}"
OUT=$(readlink -f "$1")
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-visual-XXXX")
trap 'rm -rf "$S"' EXIT
W="$REPO/build/wpsctl"
fails=0
for scheme in classic dark "high contrast"; do
    tag=$(echo "$scheme" | tr ' ' -)
    # First the desktop's notebook (a right click at 600,300 on the desktop,
    # 650,382 its Properties, 782,165 the Background tab), closed with
    # Alt+F4; then Projects (icon at 54,30), which opens at 320,96 (640x480).
    # The pointer is parked at 700,600 before each shot (no tooltips).
    cat > "$S/script" <<EOS
4 move 700 600
4.5 shot $OUT/$tag-desktop.png
4.7 click 600 300 right
5.3 shot $OUT/$tag-desktop-menu.png
5.5 click 650 382
7 move 700 600
7.5 shot $OUT/$tag-notebook.png
7.7 click 782 165
8.5 move 700 600
9 shot $OUT/$tag-notebook-background.png
9.2 key alt+F4
9.8 dclick 54 30
11.3 move 700 600
11.8 shot $OUT/$tag-folder.png
12 click 400 160 right
12.7 shot $OUT/$tag-object-menu.png
13 quit
EOS
    PRE="mkdir -p \"\$T/desk/Projects/Reports\"; echo a > \"\$T/desk/Projects/plan.txt\"; echo b > \"\$T/desk/Projects/notes.md\"
         printf '#!/bin/sh\n' > \"\$T/desk/Projects/build.sh\"; chmod +x \"\$T/desk/Projects/build.sh\"
         cp '$REPO/data/icons/'*.png \"\$T/desk/Projects/\" 2>/dev/null || true
         a=\$('$W' setup | python3 -c 'import json,sys; print(json.load(sys.stdin)[\"result\"][\"objects\"][\"WPSchemePalette\"][\"address\"])')
         '$W' settings_set \"\$a\" scheme='$scheme' > /dev/null" \
    DESKENV="WPS_WARPCENTER=1 WPS_LAUNCHPAD=1 WPS_NO_GSETTINGS=1 WPS_WALLPAPER= TZ=UTC" SECS=16 \
        tests/comp/run.sh "$S/script" "sleep 30" > "$S/$tag.txt" 2>&1
    for f in desktop desktop-menu folder object-menu notebook notebook-background; do
        [ -s "$OUT/$tag-$f.png" ] || { echo "missing: $tag-$f.png"; fails=$((fails + 1)); }
    done
    T=$(sed -n 's/^T=//p' "$S/$tag.txt")
    [ -n "$T" ] && rm -rf "$T"
done
echo "$fails missing (shots: $OUT)"
[ $fails = 0 ]
