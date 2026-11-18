#!/bin/bash
# G0 folder-window benchmark: cold cache, warm, and 2x scale, each against a
# fresh wpsd with scratch state. usage: proto/g0/bench.sh FOLDER [SECONDS]
set -u
FOLDER=$1; SECS=${2:-20}
S=${G0_STATE:-$(mktemp -d)}
# The socket goes in the session's runtime folder: socket paths are limited
# to 107 bytes, and the state folder may be deeper than that.
export WPS_CONFIG_DIR=$S/config WPS_STATE_DIR=$S/state WPS_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp}/wps-g0 WPS_WATCHD_SOCKET=none
mkdir -p "$WPS_RUNTIME_DIR"; chmod 700 "$WPS_RUNTIME_DIR"
start() { build/wpsd 2>> "$S/wpsd.log" & sleep 1; }
stop() { build/wpsctl shutdown > /dev/null 2>&1; wait; }
for cfg in "cold 1" "warm 1" "warm 2"; do
    set -- $cfg
    if [ "$1" = cold ]; then sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null; fi
    start
    echo "== $1 cache, scale $2"
    FOLDERVIEW_SCALE=$2 timeout 120 build/g0/folderview "$FOLDER" --bench "$SECS"
    stop
done
grep -E "watchdog|ERROR" "$S/wpsd.log" || echo "wpsd: no watchdog or sanitizer reports"
