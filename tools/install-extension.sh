#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Install the Objectspace desktop for this user's GNOME session:
#   - the GNOME Shell extension objectspace@sungsookhim.github.io, pointed at this build's osd-shell;
#   - a systemd user unit that starts this build's osdd at login.
# Then log out and back in (GNOME on Wayland loads new extensions only at login).
#
# usage: tools/install-extension.sh [--uninstall]
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
uuid=objectspace@sungsookhim.github.io
ext="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$uuid"
unit="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/osdd.service"

enabled() { gsettings get org.gnome.shell enabled-extensions; }

if [ "$1" = "--uninstall" ]; then
    gnome-extensions disable "$uuid" 2>/dev/null || true
    rm -rf "$ext"
    systemctl --user disable --now osdd.service 2>/dev/null || true
    rm -f "$unit"
    systemctl --user daemon-reload
    echo "Removed. Log out and back in to finish."
    exit 0
fi

for f in "$here/build/osd-shell" "$here/build/osdd"; do
    [ -x "$f" ] || { echo "$f is missing: run 'make && make gui' first" >&2; exit 1; }
done

mkdir -p "$ext"
cp "$here/gnome-extension/$uuid/metadata.json" "$here/gnome-extension/$uuid/extension.js" "$ext/"
rm -rf "$ext/schemas"
cp -r "$here/gnome-extension/$uuid/schemas" "$ext/"
glib-compile-schemas "$ext/schemas"   # its settings (Ctrl+Esc for the Window List)
printf '{"command": ["%s", "--desktop"]}\n' "$here/build/osd-shell" > "$ext/config.json"

# Enabled from the next login (gnome-extensions enable only knows extensions
# the running Shell has already seen).
cur=$(enabled)
case "$cur" in
    *"'$uuid'"*) ;;
    "@as []"|"[]") gsettings set org.gnome.shell enabled-extensions "['$uuid']" ;;
    *) gsettings set org.gnome.shell enabled-extensions "$(echo "$cur" | sed "s/]\$/, '$uuid']/")" ;;
esac
gsettings set org.gnome.shell disable-user-extensions false

mkdir -p "$(dirname "$unit")"
cat > "$unit" <<UNIT
[Unit]
Description=Objectspace object service (osdd)
PartOf=graphical-session.target
After=graphical-session-pre.target

[Service]
ExecStart=$here/build/osdd
Restart=on-failure

[Install]
WantedBy=graphical-session.target
UNIT
systemctl --user daemon-reload
systemctl --user enable osdd.service >/dev/null
echo "Installed the extension ($ext) and osdd.service."
echo "If a osdd is running by hand, stop it ('build/osdctl shutdown'); then log out and back in."
