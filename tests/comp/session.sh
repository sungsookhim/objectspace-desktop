#!/bin/sh
# Lockup, Shut down and Log off from the WarpCenter's menu, headless
# (gui/shell/session.c; v0.3-plan step 1). Shut down's dialog is
# cancelled (never confirmed: nothing may power off); Log off is confirmed
# and reaches the compositor (headless sessions with a desktop do not
# exit by themselves: the time limit ends them); Lockup, in a second run, shows the lock screen.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-sess-XXXX")
ENV="WPS_WARPCENTER=1 WPS_NO_GSETTINGS=1 WPS_NO_LAUNCHPAD=1 WPS_DEBUG_EVENTS=1"
# The menu (1280x800): Lockup now at y 715, Log off... 737, Shut down... 759.
cat > "$S/end" <<EOS
2.5 click 30 785
3.2 click 80 759
4 shot $S/shutdown.png
4.2 key Escape
5 click 30 785
5.7 click 80 737
6.5 shot $S/logoff.png
6.7 key Return
12 quit
EOS
cat > "$S/lock" <<EOS
2.5 click 30 785
3.2 click 80 715
4.5 shot $S/lock.png
5 quit
EOS
env -u WPS_SESSION WPS_COMP_DEBUG=1 DESKENV="$ENV" SECS=14 tests/comp/run.sh "$S/end" true > "$S/end.txt" 2>&1
T1=$(sed -n 's/^T=//p' "$S/end.txt")
env -u WPS_SESSION DESKENV="$ENV" SECS=8 tests/comp/run.sh "$S/lock" true > "$S/lock.txt" 2>&1
T2=$(sed -n 's/^T=//p' "$S/lock.txt")
# The buttons after the window list (the owner's, as OS/2's): Lockup at
# x 152, Find 180, Log off 212, Shut down 242 (y 785). Dialogs cancelled.
cat > "$S/buttons" <<EOS
2.5 click 242 785
3.3 shot $S/b-shutdown.png
3.5 key Escape
4 click 180 785
5 windows
5.5 click 212 785
6.2 shot $S/b-logoff.png
6.4 key Escape
7 click 152 785
8 shot $S/b-lock.png
8.5 quit
EOS
env -u WPS_SESSION DESKENV="$ENV" SECS=11 tests/comp/run.sh "$S/buttons" true > "$S/buttons.txt" 2>&1
T3=$(sed -n 's/^T=//p' "$S/buttons.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# a dialog's title bar (the active window's colour) at the screen's middle
titlebar() { python3 -c "from PIL import Image; r,g,b = Image.open('$1').convert('RGB').getpixel((500, 225)); exit(0 if b > 100 and r < 60 else 1)"; }
check "Shut down... asks (its dialog shown)" "titlebar '$S/shutdown.png'"
check "Log off... asks (its dialog shown)" "titlebar '$S/logoff.png'"
check "Escape cancels it: no session.end poweroff" "! grep -q 'session.end.*poweroff' '$T1/shell.log'"
check "Log off... confirmed: session.end logout" "grep -q 'session.end.*logout' '$T1/shell.log'"
check "the compositor is told to log off" "grep -q 'desktop command shutdown logout' '$T1/comp.log'"
check "Lockup now: session.lock" "grep -q 'session.lock' '$T2/shell.log'"
check "the lock screen is up" "grep -qi 'lock' '$T2/comp.log'"
check "button Shut down: its dialog" "titlebar '$S/b-shutdown.png'"
check "button Log off: its dialog" "titlebar '$S/b-logoff.png'"
check "both cancelled: no session.end" "! grep -q 'session.end' '$T3/shell.log'"
check "button Find: a Find window" "grep -q \"window 'Find\" '$T3/comp.log'"
check "button Lockup: session.lock" "grep -q 'session.lock' '$T3/shell.log'"
echo "$fails failed (screenshots: $S, logs: $T1 $T2)"
[ $fails = 0 ]
