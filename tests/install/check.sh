#!/bin/bash
# make install into a scratch root: the files expected, then a headless
# session from the installed programs (the desktop up, its windows listed).
#   tests/install/check.sh
set -e
cd "$(dirname "$0")/../.."
R=$(mktemp -d)
trap 'rm -rf "$R"' EXIT
make -s install DESTDIR="$R" PREFIX=/usr >/dev/null
# (the generated sets - icon themes, the SDK's headers and files - are
# checked by what must be in them, below; the rest is listed exactly)
got=$(cd "$R" && find . -type f | grep -v -e '^./usr/share/objectspace/icon-themes/' -e '^./usr/include/objectspace/' -e '^./usr/share/objectspace/sdk/' \
      -e '^./usr/share/man/' | LC_ALL=C sort)
want='./etc/pam.d/osd-lock
./usr/bin/osd
./usr/bin/osdctl
./usr/bin/osd-session
./usr/bin/osd-shell
./usr/bin/osd-terminal
./usr/share/applications/io.github.sungsookhim.Objectspace.Terminal.desktop
./usr/lib/systemd/system/osd-watchd.service
./usr/lib/systemd/user/osdd.service
./usr/libexec/objectspace/osd-compositor
./usr/libexec/objectspace/osdd
./usr/libexec/objectspace/osd-plughost
./usr/libexec/objectspace/osd-watchd
./usr/share/gnome-shell/extensions/objectspace@sungsookhim.github.io/extension.js
./usr/share/gnome-shell/extensions/objectspace@sungsookhim.github.io/metadata.json
./usr/share/gnome-shell/extensions/objectspace@sungsookhim.github.io/schemas/gschemas.compiled
./usr/share/gnome-shell/extensions/objectspace@sungsookhim.github.io/schemas/org.gnome.shell.extensions.objectspace.gschema.xml
./usr/share/wayland-sessions/objectspace.desktop
./usr/share/objectspace/icons/computer.png
./usr/share/objectspace/icons/drive-disk-off.png
./usr/share/objectspace/icons/drive-disk.png
./usr/share/objectspace/icons/drive-optical-off.png
./usr/share/objectspace/icons/drive-optical.png
./usr/share/objectspace/icons/drive-root-off.png
./usr/share/objectspace/icons/drive-root.png
./usr/share/objectspace/icons/drive-sd-off.png
./usr/share/objectspace/icons/drive-sd.png
./usr/share/objectspace/icons/drive-usb-off.png
./usr/share/objectspace/icons/drive-usb.png
./usr/share/objectspace/icons/programs.png
./usr/share/objectspace/icons/trash-empty.png
./usr/share/objectspace/icons/trash-full.png'
want=$(echo "$want" | LC_ALL=C sort)
[ "$got" = "$want" ] || { echo "FAIL: installed files differ:"; diff <(echo "$want") <(echo "$got") || true; exit 1; }
echo "ok   the installed files"
# the commands' long names: links to the short ones
for p in osd=objectspace osdctl=objectspace-ctl osd-shell=objectspace-shell osd-terminal=objectspace-terminal osd-session=objectspace-session; do
    [ "$(readlink "$R/usr/bin/${p#*=}")" = "${p%%=*}" ] || { echo "FAIL: ${p#*=} is not a link to ${p%%=*}"; exit 1; }
done
echo "ok   the long names (objectspace, objectspace-ctl, ...) link to the short ones"
for t in objectspace-flat objectspace-outline classic; do
    [ -f "$R/usr/share/objectspace/icon-themes/$t/theme.ini" ] && ls "$R/usr/share/objectspace/icon-themes/$t/"*.osdicon >/dev/null 2>&1 ||
        { echo "FAIL: icon theme $t"; exit 1; }
done
echo "ok   the icon themes (objectspace-flat, objectspace-outline, classic)"
for f in include/objectspace/osdo.h include/objectspace/osd_classes.h include/objectspace/OsdObject.h include/objectspace/OsdDataFile.h \
         share/objectspace/sdk/osdc.py share/objectspace/sdk/README.md share/objectspace/sdk/classes/OsdObject.osdi share/objectspace/sdk/sample/Makefile; do
    [ -f "$R/usr/$f" ] || { echo "FAIL: the SDK: $f"; exit 1; }
done
echo "ok   the plug-in SDK (headers, interfaces, osdc, sample)"
ls "$R/usr/share/man/man1/"osdctl.1* >/dev/null 2>&1 || { echo "FAIL: man pages"; exit 1; }
grep -q "^LIBEXEC=\${OSD_LIBEXEC:-/usr/libexec/objectspace}" "$R/usr/bin/osd-session" || { echo "FAIL: osd-session's paths"; exit 1; }
grep -q "^ExecStart=/usr/libexec/objectspace/osdd$" "$R/usr/lib/systemd/user/osdd.service" || { echo "FAIL: osdd.service"; exit 1; }
echo "ok   paths in osd-session and osdd.service"
# the installed programs, headless, with a private service
T=$(mktemp -d); RT=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/osd-inst-XXXX")
mkdir -p "$T/desk" "$T/xdg/config" "$T/xdg/data"
export OSD_CONFIG_DIR="$T/config" OSD_STATE_DIR="$T/state" OSD_RUNTIME_DIR="$RT" XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data" \
       OSD_TEMPLATES_DIR="$T/templates" OSD_SETUP_DIR="$T/setup" OSD_LAUNCHER_DIR="$T/launcher"
"$R/usr/libexec/objectspace/osdd" 2>"$T/osdd.log" & WP=$!
for i in $(seq 50); do "$R/usr/bin/osdctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
v=$("$R/usr/bin/osdctl" ping)
case $v in *'"version":"'$(cat VERSION)'"'*) echo "ok   the installed osdd answers, version $(cat VERSION)";; *) echo "FAIL: ping: $v"; kill $WP; exit 1;; esac
printf '3 windows\n3.5 quit\n' > "$T/s"
out=$(OSD_COMP_HEADLESS=800x600 OSD_COMP_SCRIPT="$T/s" timeout 20 "$R/usr/libexec/objectspace/osd-compositor" -d "'$R/usr/bin/osd-shell' --desktop '$T/desk'" 2>&1 || true)
"$R/usr/bin/osdctl" shutdown >/dev/null 2>&1; wait $WP 2>/dev/null || true
rm -rf "$T" "$RT"
echo "$out" | grep -q "window 'Desktop' app io.github.sungsookhim.Objectspace.Desktop" || { echo "FAIL: no desktop from the installed programs"; echo "$out" | tail -5; exit 1; }
echo "ok   a session from the installed programs (the desktop up)"
