#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Put "Objectspace" on the login screen (G5 step 4). Run with sudo, from your
# checkout (it stays there: the session runs tools/osd-session from it):
#   sudo tools/install-session.sh            install
#   sudo tools/install-session.sh remove     take it away again
# It writes three files and nothing else:
#   /usr/share/wayland-sessions/objectspace.desktop   the login screen's entry
#   /usr/local/bin/osd-session                runs tools/osd-session here
#   /etc/pam.d/osd-lock                       the lock screen's password check
# (the same as the system's: auth include system-auth)
# GDM 51 lists only sessions that say X-GDM-CanRunHeadless=true; the login
# screen reads the list when it starts: after installing, restart GDM or
# reboot.
set -e
[ "$(id -u)" = 0 ] || { echo "run it with sudo: sudo $0 $*" >&2; exit 1; }
REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
if [ "$1" = remove ]; then
    rm -f /usr/share/wayland-sessions/objectspace.desktop /usr/local/bin/osd-session /etc/pam.d/osd-lock
    echo "removed: Objectspace is no longer on the login screen"
    exit 0
fi
for f in build/osd-compositor build/osd-shell build/osdd tools/osd-session; do
    [ -x "$REPO/$f" ] || { echo "$REPO/$f is missing: build first (make && make gui && make compositor)" >&2; exit 1; }
done
cat > /usr/local/bin/osd-session <<EOF
#!/bin/sh
exec '$REPO/tools/osd-session' "\$@"
EOF
chmod 755 /usr/local/bin/osd-session
cat > /usr/share/wayland-sessions/objectspace.desktop <<'EOF'
[Desktop Entry]
Name=Objectspace
Comment=The Objectspace desktop (objectspace-desktop)
Exec=/usr/local/bin/osd-session
TryExec=/usr/local/bin/osd-session
Type=Application
DesktopNames=Objectspace
X-GDM-CanRunHeadless=true
EOF
cat > /etc/pam.d/osd-lock <<'EOF'
#%PAM-1.0
# osd-compositor's lock screen: the user's own password, as at login
auth       include      system-auth
account    include      system-auth
EOF
chmod 644 /usr/share/wayland-sessions/objectspace.desktop /etc/pam.d/osd-lock
echo "installed. The login screen reads its list when it starts: reboot, or log out and from a text console (Ctrl+Alt+F3) run: sudo systemctl restart gdm. Then choose Objectspace with the gear button, before the password."
