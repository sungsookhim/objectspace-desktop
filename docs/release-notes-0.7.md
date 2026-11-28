# Objectspace 0.7: more screens, and other programs in its look

Released 2026-11-28. The desktop now spans every screen, with a Screens object to arrange them, and X11 programs are sharp on scaled screens. Other programs follow the scheme: GTK 3 programs get a theme made from it, libadwaita programs its light or dark, accent and high contrast, Qt programs its colours. Objectspace's own pointers and icons are offered to every program. The steps of [v0.7-plan.md](v0.7-plan.md), each checked by the owner on a VM; also 0.6.1's Column view details.

## Install

On Fedora 45, download the RPMs from this release, then:

```
sudo dnf install ./objectspace-0.7.0-1.fc45.x86_64.rpm ./objectspace-gnome-shell-extension-0.7.0-1.fc45.noarch.rpm
```

Add `./objectspace-devel-0.7.0-1.fc45.x86_64.rpm` to write plug-ins. For Qt programs in the scheme's colours, also `sudo dnf install qt6ct`; for X11 programs' fonts at the screen's scale, `xrdb` (`sudo dnf install xrdb`).

**64-bit ARM (aarch64), experimental:** the `aarch64` RPMs here are built on GitHub's ARM machines in Fedora 45 and pass the package's own tests there, but no session has been tried on ARM hardware yet. Install them the same way; reports welcome.

Over 0.6.0 it is an upgrade: log out and in again afterwards. Your objects and settings are kept; at the first login System Setup gets the Screens object. On a new install, reboot first (the login screen reads its list of sessions at start), then choose **Objectspace** on the login screen.

To remove: `sudo dnf remove objectspace objectspace-gnome-shell-extension objectspace-devel`; your objects' state in `~/.local/share/objectspace` and `~/.config/objectspace` is kept. Objectspace's GTK 3 themes are left in `~/.local/share/themes`, its colour schemes in `~/.config/qt6ct` and `~/.config/qt5ct`; GNOME's own theme, pointers and icons come back when the desktop next starts inside GNOME (or set them in GNOME Tweaks).

## What's new since 0.6

- **More than one screen.** The screens are arranged as GNOME's Settings or the Screens object left them (GNOME's `monitors.xml`), and followed when that changes. TheBar, notifications and the Launcher are on the primary screen; new windows open on the screen the pointer is on; windows come back at login on the screen they were on. **Super+Shift+Left** and **Right** move the front window to the next screen; **Alt+F10** maximizes and restores, as on OS/2. A screen unplugged: its windows move to the primary.
- **One desktop across all screens.** Each screen shows its own part of the wallpaper (GNOME's "spanned" stretches one picture over all). Icons can go on any screen; Arrange and new icons fill the primary first; the Trash Can sits in the primary's corner. An icon on a screen that is gone shows on the nearest one and goes back when the screen does.
- **The Screens object** (System Setup): a picture of the screens to drag them into place, the primary screen, and a page per screen with its resolution, refresh rate, scale, and on or off. Changes take effect at once and are saved to GNOME's `monitors.xml`, so GNOME and Objectspace keep one setup. A change of resolution, refresh, scale or a screen on or off asks to be kept: unless **Keep** is chosen, it goes back in 15 seconds. Inside GNOME it opens GNOME's Settings, Displays.
- **Sharp X11 programs on scaled screens.** On a screen at 200 %, X11 programs draw at twice the pixels instead of being stretched; their fonts follow (`Xft.dpi`), and so does xterm's. **Scale X11 programs themselves** on the System's Window page (on by default).
- **libadwaita programs follow the scheme** (GNOME's own programs keep their own look and title bars): a dark scheme makes them dark, the accent is GNOME's nearest to the scheme's selection colour, a high-contrast scheme turns on GNOME's high contrast. Fonts and text size were already shared.
- **A GTK 3 theme from the scheme:** GTK 3 programs (GIMP, Thunderbird, and others) drawn in the scheme's colours with OS/2's square, bevelled buttons and sunken fields, Adwaita underneath. Made again when the scheme changes; running programs change at once.
- **Objectspace's pointers:** the arrow, a busy clock that turns, a hand, the text beam, move, resize and more, drawn in the icons' manner (24 to 64 pixels). Programs that ask for a pointer by name (GTK 4, Qt 6, Firefox) get these too. **Pointers** on the System's Theme page: Objectspace's or the system's.
- **Objectspace's icons for other programs:** its folders, places, drives and file types as an icon theme, so other programs' file dialogs show the same pictures (the rest stay Adwaita's). **Other programs' icons** on the System's Theme page.
- **Qt programs in the scheme's colours,** through qt6ct (or qt5ct) when installed: a colour scheme made from the scheme, used for the programs the desktop starts, unless you chose one of your own in qt6ct.
- In the Objectspace session, the theme, pointers and icons above are set as GNOME's for the session; GNOME's own are kept and come back when the desktop starts inside GNOME, so a GNOME session keeps its look.
- **The Column view's details** (0.6.1): a file's picture or icon as large as the column allows, and more details to choose on the folder's **Column view** page (owner, group, permissions, created, last access, last change, MIME type, location, link target).
- **Icon spacing:** at least half an icon between icons, titles included (Programs' long names stood too close), at every icon size.

## Fixed

- Restoring a window that opened maximized (LibreOffice Writer) ended the session.
- TheBar's buttons were blurry on scaled screens.
- An X11 program's menu (xterm's) took the keyboard with it when it closed.
- xterm's text was tiny on scaled screens, and in a Korean (Chinese, Japanese) locale every letter stood two cells wide.
- The file picker (a Background's picture, a sound) showed an old folder picture instead of the icon theme's.
- In GTK 3 lists with the keyboard, the selection was Adwaita's blue.

## Known issues

- X11 programs draw at one scale, the largest screen's: on a screen with a smaller scale they look small.
- Tiling (Super+Left and Right) is not there yet.
- GTK 4 programs take the icon theme through the desktop portal; where it does not run, they keep Adwaita's icons.
- Qt programs already running keep the colours they started with.
- The aarch64 RPMs are experimental (above).
- See [roadmap.md](roadmap.md) and [open-issues.md](open-issues.md).

## Next

0.8: the Network object and network file systems, and jobs. Also waiting: the owner's design updates (window borders, title-bar buttons, the selection rectangle, TheBar's layout) and Objectspace Terminal's own settings. See [roadmap.md](roadmap.md).
