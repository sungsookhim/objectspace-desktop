#!/bin/bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# G0 folder-window benchmark: cold cache, warm, and 2x scale, each against a
# fresh osdd with scratch state. usage: proto/g0/bench.sh FOLDER [SECONDS]
set -u
FOLDER=$1; SECS=${2:-20}
S=${G0_STATE:-$(mktemp -d)}
# The socket goes in the session's runtime folder: socket paths are limited
# to 107 bytes, and the state folder may be deeper than that.
export OSD_CONFIG_DIR=$S/config OSD_STATE_DIR=$S/state OSD_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp}/osd-g0 OSD_WATCHD_SOCKET=none
mkdir -p "$OSD_RUNTIME_DIR"; chmod 700 "$OSD_RUNTIME_DIR"
start() { build/osdd 2>> "$S/osdd.log" & sleep 1; }
stop() { build/osdctl shutdown > /dev/null 2>&1; wait; }
for cfg in "cold 1" "warm 1" "warm 2"; do
    set -- $cfg
    if [ "$1" = cold ]; then sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null; fi
    start
    echo "== $1 cache, scale $2"
    FOLDERVIEW_SCALE=$2 timeout 120 build/g0/folderview "$FOLDER" --bench "$SECS"
    stop
done
grep -E "watchdog|ERROR" "$S/osdd.log" || echo "osdd: no watchdog or sanitizer reports"
