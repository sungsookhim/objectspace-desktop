#!/bin/sh
# The WPS session's notification service, headless (v0.2-plan step 7): a
# program (notify_client.py) on a private session bus sends two
# notifications; the pop-up goes by the bar, its Reply button reaches the
# program (ActionInvoked, then NotificationClosed 2), and the envelope
# opens the Messages list. Prints ok/FAIL lines; T= the test folder.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-notify-XXXX")
cat > "$S/script" <<EOS
3.0 windows
3.1 shot $S/popup.png
3.5 click 1015 742
4.5 windows
5.0 click 1160 785
6.5 windows
6.6 shot $S/messages.png
8 quit
EOS
DESKENV="OSD_NOTIFY=1 OSD_WARPCENTER=1 OSD_NO_GSETTINGS=1" SECS=12 dbus-run-session -- tests/comp/run.sh "$S/script" \
    "python3 '$REPO/tests/comp/notify_client.py' \$T/notify.log 8" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
check "the service answers (GetServerInformation)" "grep -q '^server Workplace Shell' '$T/notify.log'"
check "capabilities: actions, persistence" "grep -q '^caps.*actions.*persistence' '$T/notify.log'"
check "the pop-up by the bar, bottom right" "grep -q \"app io.github.sungsookhim.Objectspace.Notice 360x[0-9]*+912+\" '$S/out.txt'"
check "the Reply button reaches the program" "grep -q '^action 1 reply' '$T/notify.log'"
check "then the notification is closed (dismissed)" "grep -q '^closed 1 2' '$T/notify.log'"
check "the envelope opens Messages" "grep -q \"app io.github.sungsookhim.Objectspace.Messages\" '$S/out.txt'"
check "screenshots made" "[ -s '$S/popup.png' ] && [ -s '$S/messages.png' ]"
echo "$fails failed (screenshots: $S, logs: $T)"
[ $fails = 0 ]
