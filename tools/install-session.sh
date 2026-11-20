#!/bin/sh
# Put "WPS" on the login screen (G5 step 4). Run with sudo, from your
# checkout (it stays there: the session runs tools/wps-session from it):
#   sudo tools/install-session.sh            install
#   sudo tools/install-session.sh remove     take it away again
# It writes three files and nothing else:
#   /usr/share/wayland-sessions/wps.desktop   the login screen's entry
#   /usr/local/bin/wps-session                runs tools/wps-session here
#   /etc/pam.d/wps-lock                       the lock screen's password check
# (the same as the system's: auth include system-auth)
set -e
[ "$(id -u)" = 0 ] || { echo "run it with sudo: sudo $0 $*" >&2; exit 1; }
REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
if [ "$1" = remove ]; then
    rm -f /usr/share/wayland-sessions/wps.desktop /usr/local/bin/wps-session /etc/pam.d/wps-lock
    echo "removed: WPS is no longer on the login screen"
    exit 0
fi
for f in build/wps-compositor build/wps-shell build/wpsd tools/wps-session; do
    [ -x "$REPO/$f" ] || { echo "$REPO/$f is missing: build first (make && make gui && make compositor)" >&2; exit 1; }
done
cat > /usr/local/bin/wps-session <<EOF
#!/bin/sh
exec '$REPO/tools/wps-session' "\$@"
EOF
chmod 755 /usr/local/bin/wps-session
cat > /usr/share/wayland-sessions/wps.desktop <<'EOF'
[Desktop Entry]
Name=WPS
Comment=The Workplace Shell desktop (wps-linux)
Exec=/usr/local/bin/wps-session
TryExec=/usr/local/bin/wps-session
Type=Application
DesktopNames=WPS
EOF
cat > /etc/pam.d/wps-lock <<'EOF'
#%PAM-1.0
# wps-compositor's lock screen: the user's own password, as at login
auth       include      system-auth
account    include      system-auth
EOF
chmod 644 /usr/share/wayland-sessions/wps.desktop /etc/pam.d/wps-lock
echo "installed: choose WPS on the login screen (the gear button, before the password)"
