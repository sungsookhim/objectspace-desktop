#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Suspend after idle (0.6 step 6), headless, with a fake logind on a private
# bus (tests/comp/fake_logind.py): sleep set to 3 s on mains: idle, logind
# is asked to suspend; the pointer moved every second: not; on battery
# with sleep set for mains only: not; hibernate chosen: logind is asked to
# hibernate.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-sleep-XXXX")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
dbus-daemon --session --nofork --print-address=3 3> "$S/addr" > /dev/null 2>&1 &
DB=$!
for i in $(seq 50); do [ -s "$S/addr" ] && break; sleep 0.1; done
ADDR=$(head -1 "$S/addr")
# one run: NAME, its script, then the compositor's settings (VAR=VALUE ...)
run() {
    n=$1; sc=$2; shift 2
    : > "$S/$n.log"
    python3 tests/comp/fake_logind.py "$ADDR" "$S/$n.log" > "$S/$n.fake" 2>&1 &
    FL=$!
    for i in $(seq 50); do grep -q ready "$S/$n.log" 2>/dev/null && break; sleep 0.1; done
    env DBUS_SYSTEM_BUS_ADDRESS="$ADDR" OSD_COMP_LOGIND=1 "$@" NODESK=1 SECS=10 tests/comp/run.sh "$S/$n" "sleep 20" > "$S/$n.txt" 2>&1
    kill $FL 2>/dev/null; wait $FL 2>/dev/null
    t=$(sed -n 's/^T=//p' "$S/$n.txt"); [ -n "$t" ] && rm -rf "$t"
}
printf '8 quit\n' > "$S/idle"
run idle "$S/idle" OSD_SLEEP_AC=3 OSD_SLEEP_BATTERY=3 OSD_POWER=mains
for k in 1 2 3 4 5 6 7; do echo "$k move $((300 + k * 10)) 300"; done > "$S/busy"
echo "8 quit" >> "$S/busy"
run busy "$S/busy" OSD_SLEEP_AC=3 OSD_SLEEP_BATTERY=3 OSD_POWER=mains
run battery "$S/idle" OSD_SLEEP_AC=3 OSD_SLEEP_BATTERY=0 OSD_POWER=battery
run hib "$S/idle" OSD_SLEEP_AC=3 OSD_SLEEP_AC_ACTION=hibernate OSD_POWER=mains
kill $DB 2>/dev/null; wait $DB 2>/dev/null
check "idle 3 s on mains: logind asked to suspend" "grep -qx suspend '$S/idle.log' && grep -q 'sleeps after 3 s idle' '$S/idle.txt'"
check "the pointer moved every second: no suspend" "! grep -q suspend '$S/busy.log'"
check "on battery, sleep set for mains only: no suspend" "! grep -q suspend '$S/battery.log'"
check "hibernate chosen: logind asked to hibernate" "grep -qx hibernate '$S/hib.log' && ! grep -q suspend '$S/hib.log'"
echo "$fails failed ($S)"
[ $fails = 0 ] && rm -rf "$S"
exit $fails
