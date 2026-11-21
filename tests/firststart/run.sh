#!/bin/sh
# The desktop's first start, step 7 (0.2): Templates in System. Part
# "new": a new desktop; part "old": a desktop at step 6 with Templates on
# it, which moves into System. Played by tests/firststart/scenario.py.
cd "$(dirname "$0")/../.."
REPO=$PWD
R=0
for part in new old; do
    T=$(mktemp -d "${TMPDIR:-/tmp}/wps-first-XXXX")
    RT=$(mktemp -d "$XDG_RUNTIME_DIR/wps-first-XXXX")
    mkdir -p "$T/desk" "$T/config" "$T/templates"
    [ $part = old ] && printf 'version 6\n' > "$T/config/desktop-made"
    export T WPS_CONFIG_DIR="$T/config" WPS_STATE_DIR="$T/state" WPS_RUNTIME_DIR="$RT" WPS_SAVE_DELAY_MS=100
    export XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" XDG_DATA_DIRS="$T/share" WPS_TEMPLATES_DIR="$T/templates"
    export WPS_UDISKS_BUS="unix:path=$RT/none"
    "$REPO/build/wpsd" 2> "$T/wpsd.log" &
    WP=$!
    for i in $(seq 50); do "$REPO/build/wpsctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
    python3 tests/firststart/scenario.py $part || R=1
    "$REPO/build/wpsctl" shutdown >/dev/null 2>&1; wait $WP
    [ $R = 0 ] && rm -rf "$T" "$RT" || { echo "kept: $T"; tail -5 "$T/wpsd.log"; rm -rf "$RT"; }
done
exit $R
