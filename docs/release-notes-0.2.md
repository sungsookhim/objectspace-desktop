# WPS for Linux 0.2: the OS/2 desktop, installable

Released 2026-11-21. A desktop for Linux built on the object model of the OS/2 Workplace Shell, now installable on Fedora 45 with `dnf`.

## Install

Download the two RPMs from this release, then:

```
sudo dnf install ./wps-0.2.0-1.fc45.x86_64.rpm ./wps-gnome-shell-extension-0.2.0-1.fc45.noarch.rpm
```

Reboot (the login screen reads its list of sessions at start), then either:

- choose **WPS** on the login screen: the WPS session, with its own compositor; or
- log in to GNOME and run `gnome-extensions enable wps@wps-linux`, then log out and in: the WPS desktop inside GNOME.

`wpsd`, the per-user object service, starts with every graphical session. To remove: `sudo dnf remove wps wps-gnome-shell-extension`; your objects in `~/.local/share/wps` and `~/.config/wps` are kept.

The WPS session needs a real seat (a console or a virtual machine's screen); it does not run over RDP.

## What's new since 0.1

- **The default desktop.** A new user gets System, Programs and the Trash Can on the desktop, made by wpsd at the first start in versioned steps; 0.1's desktops are brought along.
  - **Restore Desktop** (in System, or `wpsctl restore-defaults`) puts back default objects that were moved, deleted or trashed.
- **The System folder:** Templates, Startup, the Minimized Window Viewer, the settings objects, Restore Desktop.
- **Trash Can:** trash, restore, empty; shadows go to the trash too; empty and full icons.
- **Computer and Drives,** through udisks2: mount, unmount, eject; encrypted drives with a passphrase dialog; portable volumes let go of before unmounting.
- **Programs:** the installed programs, kept up to date as programs are installed and removed.
- **The WarpCenter** (on by default in the WPS session):
  - the WPS menu, with icons and cascades, Lockup, Log off and Shut down;
  - the window list, clock, keyboard indicator and meters;
  - **Messages:** WPS as the notification service, with a history;
  - **the tray:** other programs' status icons (StatusNotifierItem), with their menus.
- **Settings notebooks** for the WarpCenter and LaunchPad; both open from System, and never twice.
- **Startup:** programs and objects started at login, from the XDG autostart folder.
- **Open windows restored at login:** folder windows, Find windows and settings notebooks come back where they were.
- **Installation:** `make install` to `/usr`; versioned databases (0.2 reads 0.1's, refuses newer ones); one version in every program; the RPM.

## Known issues

- At a new user's first start the desktop can briefly show an extra icon before settling on the three defaults.
- After `dnf remove`, the login screen lists WPS until the next reboot.
- No manual pages yet for `wpsctl`, `wps-shell` and `wps-session`.
- Deferred: RDP, more than one monitor, window borders for GTK 4 programs, details-view sorting and columns. The full list is in [docs/roadmap.md](roadmap.md) and [docs/open-issues.md](open-issues.md).

## Authorship and license

Designed and built by Sungsoo Khim, with code written with the assistance of Claude (Anthropic) through Claude Code. The design and ideas are OS/2's Workplace Shell, from IBM; this project is not affiliated with IBM or the current owners of OS/2. MIT License.
