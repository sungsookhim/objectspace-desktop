#!/bin/sh
# Every osd-shell self-test, its output kept as a "golden" file to compare
# across a refactor (docs/object-properties-design.md 4):
#   tests/gui/golden.sh OUT [MODE...]      (default: every mode in golden-modes.txt)
#   tests/gui/golden-compare.sh BASE OUT
# Each mode runs through tests/gui/selftest.sh in a fresh folder, inside a
# headless compositor (never the user's session): osd-compositor with a
# private D-Bus and runtime folder, or a private headless GNOME Shell for
# the modes that need GNOME. OUT/MODE.txt gets the "selftest:" lines and
# any sanitizer or runtime errors, with temporary paths, PIDs and times
# replaced by placeholders.
cd "$(dirname "$0")/../.."
REPO=$PWD
OUT=$(mkdir -p "${1:?usage: tests/gui/golden.sh OUT [MODE...]}" && cd "$1" && pwd); shift
WANT=" $* "
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/osd-golden-XXXX")
trap 'rm -rf "$SCRATCH"' EXIT
start=$(date +%s)

# Placeholders for what changes from run to run.
normalize() {
    sed -E \
        -e "s#$REPO#<REPO>#g" \
        -e 's#/tmp/osd-selftest-[A-Za-z0-9_-]+#<W>#g' \
        -e 's#/tmp/osd-[a-z]+-[A-Za-z0-9]{4}#<T>#g' \
        -e 's#/run/user/[0-9]+/osd-[A-Za-z0-9-]+#<RT>#g' \
        -e 's#vobj:[0-9a-f-]{36}#vobj:<ID>#g' \
        -e 's#[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}#<ID>#g' \
        -e 's#==[0-9]+==#==<PID>==#g' \
        -e 's#\b(pid|PID) [0-9]+#\1 <N>#g' \
        -e 's#[0-9]+(\.[0-9]+)? ?ms\b#<MS>#g' \
        -e 's#\b[0-9]+\.[0-9]+ s\b#<S> s#g' \
        -e 's#-[0-9]{9,}\.png#-<N>.png#g' \
        -e 's#-[0-9]{9,}\.osdicon#-<N>.osdicon#g' \
        -e 's#cube-[A-Za-z0-9-]+#<HOST>#g' \
        -e "s#$(hostname)#<HOST>#g"
}

# One self-test in the headless osd-compositor: its own runtime folder
# (so no Wayland, PipeWire or portal of the user's session is reached) and
# its own D-Bus; gsettings are off (nothing of the user's GNOME changes).
run_comp() {   # MODE ENV SETUP
    W=$(mktemp -d "$SCRATCH/c-XXXX")
    RT=$(mktemp -d "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/osd-golden-XXXX"); chmod 700 "$RT"
    cat > "$W/cmd" <<EOC
cd '$REPO'
env OSD_NO_GSETTINGS=1 $2 tests/gui/selftest.sh '$1' "\$(cat '$W/setup')" > '$W/out.txt' 2>&1
kill \$(cat '$W/pid') 2>/dev/null
EOC
    printf '%s' "$3" > "$W/setup"
    XDG_RUNTIME_DIR=$RT OSD_COMP_HEADLESS=1280x800 timeout 120 dbus-run-session -- sh -c \
        "echo \$\$ > '$W/pid'; exec '$REPO/build/osd-compositor' -s 'sh $W/cmd'" > "$W/comp.log" 2>&1
    cat "$W/out.txt" 2>/dev/null
    rm -rf "$RT"
}

run_gnome() {   # MODE ENV SETUP
    W=$(mktemp -d "$SCRATCH/g-XXXX")
    printf '%s' "$3" > "$W/setup"
    tests/gnome/with-gnome.sh -- sh -c "cd '$REPO' && env $2 tests/gui/selftest.sh '$1' \"\$(cat '$W/setup')\"" 2>&1
}

# tests/gnome/run.sh (the extension starts the desktop): the self-test's
# lines from the desktop's log, then its folder removed.
run_gnomerun() {   # ENV
    t=$(env $1 tests/gnome/run.sh 2>/dev/null | sed -n 's/^T=//p')
    [ -n "$t" ] && [ -f "$t/shell.log" ] && grep -E "selftest:|ERROR: AddressSanitizer|runtime error" "$t/shell.log"
    case "$t" in /tmp/osd-gnome-*) rm -rf "$t" ;; esac
}

grep -v '^#' tests/gui/golden-modes.txt | grep -v '^$' | while IFS='	' read -r mode runner envs setup status; do
    case "$WANT" in "  ") ;; *" $mode "*) ;; *) continue ;; esac
    [ "$status" = skip ] && { echo "skip    $mode"; continue; }
    [ "$envs" = - ] && envs=
    [ "$setup" = - ] && setup=
    envs=$(echo "$envs" | sed "s#@REPO@#$REPO#g")
    t0=$(date +%s)
    case "$runner" in
        comp) run_comp "$mode" "$envs" "$setup" ;;
        gnome) run_gnome "$mode" "$envs" "$setup" ;;
        a11y) tests/gnome/with-gnome.sh -- tests/gui/a11y-run.sh 2>&1 | grep -E "^(a11y:|selftest)|ERROR: AddressSanitizer|runtime error" ;;
        gnomerun) run_gnomerun "$envs" ;;
    esac < /dev/null | grep -E "selftest|ERROR|Sanitizer|runtime error|a11y" | normalize > "$OUT/$mode.tmp"
    # sorted: lines printed by concurrent callbacks, in any order
    if [ "$status" = sorted ]; then sort "$OUT/$mode.tmp" > "$OUT/$mode.txt"; rm -f "$OUT/$mode.tmp"
    else mv "$OUT/$mode.tmp" "$OUT/$mode.txt"; fi
    echo "ran     $mode ($(($(date +%s) - t0)) s, $(wc -l < "$OUT/$mode.txt") lines)"
done
echo "total $(($(date +%s) - start)) s (output: $OUT)"
