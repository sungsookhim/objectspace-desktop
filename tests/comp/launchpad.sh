#!/bin/sh
# The LaunchPad and the WarpCenter in 0.3 (v0.3-plan step 10), headless:
# tooltips after resting on a LaunchPad button and on the clock; Super
# pressed alone opens the WPS menu, again closes it, Super with a click in
# between does nothing; a vertical LaunchPad's drawers open out to the
# right, or the left (its Drawers setting).
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-lp-XXXX")
# (the LaunchPad: 351x74+520+718; its first object button at 700..754 x 733..789)
cat > "$S/script" <<EOS
3 move 720 758
3.2 move 721 758
4.5 shot $S/tip-lp.png
5 move 1250 785
5.2 move 1251 785
6.5 shot $S/tip-clock.png
7 move 640 300
7.2 key Super_L
8 shot $S/super.png
8.5 click 640 300
9 shot $S/closed.png
9.1 press Super_L
9.2 click 640 300
9.4 release Super_L
10 shot $S/super-click.png
10.3 key Super_L
10.8 key Super_L
11.3 shot $S/super-twice.png
13 windows
13.5 click 680 660
14 shot $S/right.png
14.2 click 300 300
16 click 529 660
16.5 shot $S/left.png
16.8 windows
17.2 click 512 657
18 move 640 120
18.5 shot $S/cascade.png
19 quit
EOS
REPO=$PWD
W="$REPO/build/wpsctl"
# (at 12 s: the LaunchPad made vertical, its drawers on the right; at 15 s on the left)
DESKENV="WPS_WARPCENTER=1 WPS_NO_GSETTINGS=1 WPS_LAUNCHPAD=1" SECS=21 tests/comp/run.sh "$S/script" \
    "sleep 11.5; A=\$('$W' setup | python3 -c 'import json,sys; print(json.load(sys.stdin)[\"result\"][\"objects\"][\"WPLaunchPad\"][\"address\"])');
     F=\$('$W' layout \"\$T/launchpad\" | python3 -c 'import json,sys; print([k for k, v in json.load(sys.stdin)[\"result\"][\"positions\"].items() if v.startswith(\"0,\")][0])');
     mkdir -p \"\$T/launchpad Drawers/\$F/Sub\" && echo x > \"\$T/launchpad Drawers/\$F/Sub/a.txt\";
     '$W' settings_set \"\$A\" orientation=1 drawer_side=0 > /dev/null; sleep 3; '$W' settings_set \"\$A\" drawer_side=1 > /dev/null" \
    > "$S/out.txt" 2>&1
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
px() { python3 -c "from PIL import Image; print('%d %d %d' % Image.open('$1').convert('RGB').getpixel(($2, $3)))"; }
yellow() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB')
ok = lambda p: p[0] >= 250 and p[1] >= 250 and 215 <= p[2] <= 232   # (the tooltip's pale yellow)
exit(0 if any(ok(im.getpixel((x, y))) for x in range($2, $3, 2) for y in range($4, $5, 2)) else 1)"; }
check "a LaunchPad button's tooltip (above it)" "yellow '$S/tip-lp.png' 690 780 690 735"
check "the clock's tooltip: the date" "yellow '$S/tip-clock.png' 1080 1280 740 770"
check "Super alone: the WPS menu (above the Desktop button)" "[ \"\$(px '$S/super.png' 150 700)\" = '204 204 204' ]"
check "the menu closed by a click elsewhere" "[ \"\$(px '$S/closed.png' 150 700)\" != '204 204 204' ]"
check "Super again: the menu closed" "[ \"\$(px '$S/super-twice.png' 150 700)\" != '204 204 204' ]"
# (vertical, kept on the screen: 168x309+520+460; its first drawer arrow at 674..686 x 640..682 on the right, 523..535 on the left)
check "vertical: placed on the screen" "grep -q \"LaunchPad' app org.wps.LaunchPad 168x309+520+460\" '$S/out.txt'"
check "drawers on the right: one opens out to the right" "[ \"\$(px '$S/right.png' 760 668)\" = '204 204 204' ] && [ \"\$(px '$S/right.png' 450 668)\" != '204 204 204' ]"
check "drawers on the left: out to the left" "[ \"\$(px '$S/left.png' 450 668)\" = '204 204 204' ] && [ \"\$(px '$S/left.png' 760 668)\" != '204 204 204' ]"
# (the drawer, on the left, spans x 323..520; a cascade from its folder opens left of it)
check "a cascade from a drawer on the left: out to the left too" "[ \"\$(px '$S/cascade.png' 280 668)\" = '204 204 204' ]"
check "Super with a click: no menu" "[ \"\$(px '$S/super-click.png' 150 700)\" != '204 204 204' ]"
echo "$fails failed (screenshots: $S)"
[ $fails = 0 ]
