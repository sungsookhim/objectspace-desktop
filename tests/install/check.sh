#!/bin/bash
# make install into a scratch root: the files expected, then a headless
# session from the installed programs (the desktop up, its windows listed).
#   tests/install/check.sh
set -e
cd "$(dirname "$0")/../.."
R=$(mktemp -d)
trap 'rm -rf "$R"' EXIT
make -s install DESTDIR="$R" PREFIX=/usr >/dev/null
got=$(cd "$R" && find . -type f | LC_ALL=C sort)
want='./etc/pam.d/wps-lock
./usr/bin/wpsctl
./usr/bin/wps-session
./usr/bin/wps-shell
./usr/lib/systemd/system/wps-watchd.service
./usr/lib/systemd/user/wpsd.service
./usr/libexec/wps/wps-compositor
./usr/libexec/wps/wpsd
./usr/libexec/wps/wps-plughost
./usr/libexec/wps/wps-watchd
./usr/share/gnome-shell/extensions/wps@wps-linux/extension.js
./usr/share/gnome-shell/extensions/wps@wps-linux/metadata.json
./usr/share/gnome-shell/extensions/wps@wps-linux/schemas/gschemas.compiled
./usr/share/gnome-shell/extensions/wps@wps-linux/schemas/org.gnome.shell.extensions.wps.gschema.xml
./usr/share/wayland-sessions/wps.desktop
./usr/share/wps/icons/computer.png
./usr/share/wps/icons/drive-disk-off.png
./usr/share/wps/icons/drive-disk.png
./usr/share/wps/icons/drive-optical-off.png
./usr/share/wps/icons/drive-optical.png
./usr/share/wps/icons/drive-root-off.png
./usr/share/wps/icons/drive-root.png
./usr/share/wps/icons/drive-sd-off.png
./usr/share/wps/icons/drive-sd.png
./usr/share/wps/icons/drive-usb-off.png
./usr/share/wps/icons/drive-usb.png
./usr/share/wps/icons/trash-empty.png
./usr/share/wps/icons/trash-full.png'
want=$(echo "$want" | LC_ALL=C sort)
[ "$got" = "$want" ] || { echo "FAIL: installed files differ:"; diff <(echo "$want") <(echo "$got") || true; exit 1; }
echo "ok   the installed files"
grep -q "^LIBEXEC=\${WPS_LIBEXEC:-/usr/libexec/wps}" "$R/usr/bin/wps-session" || { echo "FAIL: wps-session's paths"; exit 1; }
grep -q "^ExecStart=/usr/libexec/wps/wpsd$" "$R/usr/lib/systemd/user/wpsd.service" || { echo "FAIL: wpsd.service"; exit 1; }
echo "ok   paths in wps-session and wpsd.service"
# the installed programs, headless, with a private service
T=$(mktemp -d); RT=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wps-inst-XXXX")
mkdir -p "$T/desk" "$T/xdg/config" "$T/xdg/data"
export WPS_CONFIG_DIR="$T/config" WPS_STATE_DIR="$T/state" WPS_RUNTIME_DIR="$RT" XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" \
       WPS_TEMPLATES_DIR="$T/templates" WPS_SETUP_DIR="$T/setup" WPS_LAUNCHPAD_DIR="$T/launchpad"
"$R/usr/libexec/wps/wpsd" 2>"$T/wpsd.log" & WP=$!
for i in $(seq 50); do "$R/usr/bin/wpsctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
v=$("$R/usr/bin/wpsctl" ping)
case $v in *'"version":"'$(cat VERSION)'"'*) echo "ok   the installed wpsd answers, version $(cat VERSION)";; *) echo "FAIL: ping: $v"; kill $WP; exit 1;; esac
printf '3 windows\n3.5 quit\n' > "$T/s"
out=$(WPS_COMP_HEADLESS=800x600 WPS_COMP_SCRIPT="$T/s" timeout 20 "$R/usr/libexec/wps/wps-compositor" -d "'$R/usr/bin/wps-shell' --desktop '$T/desk'" 2>&1 || true)
"$R/usr/bin/wpsctl" shutdown >/dev/null 2>&1; wait $WP 2>/dev/null || true
rm -rf "$T" "$RT"
echo "$out" | grep -q "window 'Desktop' app org.wps.Desktop" || { echo "FAIL: no desktop from the installed programs"; echo "$out" | tail -5; exit 1; }
echo "ok   a session from the installed programs (the desktop up)"
