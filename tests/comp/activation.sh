#!/bin/sh
# Programs come to the front (0.4 step 6), headless: an executable opened
# from the desktop gets the click's xdg-activation token, as
# XDG_ACTIVATION_TOKEN and DESKTOP_STARTUP_ID (the window asks the
# compositor for it and sends it with open; osdd sets them).
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-act-XXXX")
W="$REPO/build/osdctl"
printf '#!/bin/sh\necho "$XDG_ACTIVATION_TOKEN|$DESKTOP_STARTUP_ID" > "%s/tok.txt"\n' "$S" > "$S/tool"
cat > "$S/script" <<EOS
3 dclick 54 30
5 quit
EOS
# (a-tool.sh where Projects was; its Program page's type: window, no terminal)
PRE="rm -rf \"\$T/desk/Projects\"; cp '$S/tool' \"\$T/desk/a-tool.sh\"; chmod +x \"\$T/desk/a-tool.sh\";
     '$W' settings_set \"\$T/desk/a-tool.sh\" prog_type=1 > /dev/null 2>&1" \
DESKENV="OSD_NO_LAUNCHPAD=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER=" SECS=8 tests/comp/run.sh "$S/script" true > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
tok=$(cat "$S/tok.txt" 2>/dev/null)
check "opened, with the click's token, both names ($tok)" "[ -n \"\${tok%%|*}\" ] && [ \"\${tok%%|*}\" = \"\${tok#*|}\" ]"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ]
