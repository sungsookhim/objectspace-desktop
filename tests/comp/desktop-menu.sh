#!/bin/sh
# The desktop's own menu, headless (v0.3-plan step 2): System setup's
# cascade (System Setup's objects; one opens its notebook), Shut down...
# (its dialog; cancelled, never confirmed), and Lockup now (the lock
# screen, in a second run).
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-dmenu-XXXX")
ENV="WPS_WARPCENTER=1 WPS_NO_GSETTINGS=1 WPS_NO_LAUNCHPAD=1 WPS_DEBUG_EVENTS=1"
# A right click at (600, 300) opens the menu there (1280x800): System
# setup at y 499, Lockup now 521, Shut down... 543; the cascade beside it
# (Mouse, Scheme Palette, Sound, System: System at (815, 568)).
cat > "$S/one" <<EOS
2.5 click 600 300 right
3.2 click 660 499
3.8 shot $S/cascade.png
4 click 815 568
5.5 windows
6 click 300 300 right
6.7 shot $S/menu2.png
7 quit
EOS
cat > "$S/two" <<EOS
2.5 click 600 300 right
3.2 click 660 543
4 shot $S/shutdown.png
4.2 key Escape
5 click 600 300 right
5.7 click 660 521
6.7 shot $S/lock.png
7.5 quit
EOS
env -u WPS_SESSION DESKENV="$ENV" SECS=10 tests/comp/run.sh "$S/one" true > "$S/one.txt" 2>&1
T1=$(sed -n 's/^T=//p' "$S/one.txt")
env -u WPS_SESSION DESKENV="$ENV" SECS=10 tests/comp/run.sh "$S/two" true > "$S/two.txt" 2>&1
T2=$(sed -n 's/^T=//p' "$S/two.txt")
# A window open beside the click (the test client at 436,146): the menu
# is above it, not below (the desktop's menus were under every window).
cat > "$S/three" <<EOS
3 click 380 200 right
4 shot $S/over.png
4.5 quit
EOS
env -u WPS_SESSION DESKENV="$ENV" SECS=8 tests/comp/run.sh "$S/three" "'$PWD/build/tests/ssd-client' Framed 3060a0" > "$S/three.txt" 2>&1
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# a dialog's title bar (the active window's colour) at the screen's middle
titlebar() { python3 -c "from PIL import Image; r,g,b = Image.open('$1').convert('RGB').getpixel((500, 225)); exit(0 if b > 100 and r < 60 else 1)"; }
# the cascade's highlighted parent (System setup) at the menu
highlighted() { python3 -c "from PIL import Image; r,g,b = Image.open('$1').convert('RGB').getpixel((640, 499)); exit(0 if b > 100 and r < 60 else 1)"; }
check "the menu is above other windows" "python3 -c \"from PIL import Image; r,g,b = Image.open('$S/over.png').convert('RGB').getpixel((500, 300)); exit(0 if abs(r - g) < 8 and abs(g - b) < 8 and r > 150 else 1)\""
check "System setup opens its cascade" "highlighted '$S/cascade.png'"
check "a System setup object opens (object.open System)" "grep -q 'object.open.*System' '$T1/shell.log'"
check "its notebook is a window" "grep -q \"window 'System\" '$T1/comp.log'"
check "Shut down... asks (its dialog shown)" "titlebar '$S/shutdown.png'"
check "Escape cancels it" "! grep -q 'session.end' '$T2/shell.log'"
check "Lockup now: session.lock from the desktop" "grep -q 'session.lock' '$T2/shell.log'"
check "the lock screen is up" "grep -qi 'lock' '$T2/comp.log'"
echo "$fails failed (screenshots: $S, logs: $T1 $T2)"
[ $fails = 0 ]
