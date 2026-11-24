# WPS for Linux

A desktop for Linux built on the object model of the OS/2 Workplace Shell. Every file, folder and abstract item is a persistent object belonging to a class that can be extended, with shadows, templates and settings notebooks. The object core is built first, with no GUI.

Status: **version 0.4.1** (2026-11-24): icons, folders and objects. Every setting is a property of an object, set for one object, a file type or a whole class (OS/2's Class... and Default), with actions on events and schemes as objects; icon themes and sizes with open icons; OS/2's free icon layout; folder windows that remember where they were; Prevent delete; OS/2's Colour Palette and fonts in every window; Work Area folders; WPS Terminal, WPS's own terminal for text-mode programs and the command line, and a `wps` command for scripts; a plug-in SDK (`wps-devel`). Built on 0.3 (daily use and OS/2 fidelity), 0.2 (the OS/2 desktop, installable as an RPM for Fedora 45) and 0.1's object core and GUI, inside GNOME or as its own session from the login screen (`wps-compositor` on wlroots). See the [release notes](docs/release-notes-0.4.1.md); what is open, deferred or not yet designed is gathered in [docs/roadmap.md](docs/roadmap.md).

## Installing

On Fedora 45, download the two RPMs from the [latest release](https://github.com/sungsookhim/wps-linux/releases/latest) and run `sudo dnf install ./wps-*.rpm`. Reboot, then choose **WPS** on the login screen, or in GNOME run `gnome-extensions enable wps@wps-linux` and log in again. `make rpm` builds the RPMs from a checkout (needs `rpm-build` and the build dependencies below).

## Documents

| Document | What it holds |
|---|---|
| [docs/object-core-design.md](docs/object-core-design.md) | Object core design v0.17: principles, responsiveness, runtime, classes, identity, persistence, concurrency, multiple logged-in users, links, change tracking, scale, API, tests, milestones |
| [docs/gui-design.md](docs/gui-design.md) | GUI design v0.6: OS/2 Warp 4–4.52 base, modern features, configurable appearance, action events; coexists with GNOME; own drawing; proposed architecture and milestones |
| [docs/roadmap.md](docs/roadmap.md) | Version 0.1's baseline, every open and deferred item, and a proposed order for what follows |
| [docs/v0.2-design.md](docs/v0.2-design.md) | Version 0.2's design: the default desktop, live folders, Trash Can, Computer and Drives, Programs, the System folder and Startup, WarpCenter, Messages, settings objects, installation and versions |
| [docs/v0.2-plan.md](docs/v0.2-plan.md) | Version 0.2's implementation plan: nine steps, their checks, order and risks |
| [docs/release-notes-0.2.md](docs/release-notes-0.2.md) | Version 0.2's release notes: installing, what is new, known issues |
| [docs/v0.3-design.md](docs/v0.3-design.md) | Version 0.3's design: the desktop's menu, backgrounds, sorting and columns, search, icon layout and locks, Open With, program settings, templates, fonts and sounds, the LaunchPad (Work Area folders designed for 0.4) |
| [docs/v0.3-plan.md](docs/v0.3-plan.md) | Version 0.3's implementation plan: eleven steps, their checks, order and risks |
| [docs/release-notes-0.3.md](docs/release-notes-0.3.md) | Version 0.3's release notes: installing and upgrading, what is new, known issues |
| [docs/v0.4-design.md](docs/v0.4-design.md) | Version 0.4's design: icon packages and themes, icon sizes and the open icon, OS/2's free layout, folder windows remembered, Prevent delete, the Colour Palette, fonts everywhere, Work Area folders |
| [docs/cli-design.md](docs/cli-design.md) | The command line in WPS (0.4.1): WPS Terminal, Command prompt here, the `wps` command |
| [docs/v0.4-plan.md](docs/v0.4-plan.md) | Version 0.4's implementation plan: eleven steps, their checks, order and risks |
| [docs/release-notes-0.4.1.md](docs/release-notes-0.4.1.md) | Version 0.4.1's release notes: installing and upgrading, what is new, known issues |
| [docs/open-issues.md](docs/open-issues.md) | Known problems and gaps found while building and trying the GUI, item by item |
| [docs/reviews/2026-11-18-gemini-review.md](docs/reviews/2026-11-18-gemini-review.md) | External review of v0.1, our assessment, and the changes folded into v0.2 |
| [docs/notes/2026-11-discussion-notes.md](docs/notes/2026-11-discussion-notes.md) | The discussion that led to the design: approach options, prior art, change tracking, links |

The repository copy is the source of truth. The Claude doc at https://claude.ai/artifact/1ucowX5KxZNaXLJaHGXZr4 holds v0.1 with a drawn architecture diagram.

## Plan

1. M0: kernel-behaviour and per-user filtering prototypes. Done; findings folded into design v0.4.
2. M1: object runtime, main loop, class generator, core classes. Done.
3. M2: persistence, identity, concurrency, `wpsd` and `wpsctl`, state that travels with removable volumes. Done.
4. M3: change tracking with `wps-watchd` or inotify; saves, moves, copies, hard links; catch-up scan. Done.
5. M4: shadows (managed symlinks), trash, programs and objects as files, streaming listings, selections and jobs. Done.
6. M5: subscriptions and events, menus and settings as data, programs, templates, file types, root sessions, sandboxed plug-in hosts. Done.
7. GUI (docs/gui-design.md): G0 prototypes (proto/g0/RESULTS.md); G1 the widget set and folder windows (gui/RESULTS-G1.md); G2 the desktop, views, notebooks, drag and drop, templates, shadows, trash; G3 action events, sounds, schemes, GNOME settings sync; G4 Window List, LaunchPad, Find, accessibility, input methods; G5 `wps-compositor` and the WPS session. Done: version 0.1.
8. Version 0.2 ([design](docs/v0.2-design.md), [plan](docs/v0.2-plan.md), [release notes](docs/release-notes-0.2.md)): the OS/2 desktop, installable. Done.
9. Version 0.3 ([design](docs/v0.3-design.md), [plan](docs/v0.3-plan.md), [release notes](docs/release-notes-0.3.md)): daily use and OS/2 fidelity. Done.
10. Version 0.4.1 ([design](docs/v0.4-design.md), [the command line](docs/cli-design.md), [plan](docs/v0.4-plan.md), [release notes](docs/release-notes-0.4.1.md)): icons, folders and objects, on the object-property base; WPS Terminal and the `wps` command. Done.
11. Next: 0.5, ready to be public: a name of its own, the repository's licenses and documents, a welcome for contributors (see the [roadmap](docs/roadmap.md)).

Related project: [x64dos](https://github.com/sungsookhim/x64dos), a possible second target for the runtime.

## Layout

| Path | Contents |
|---|---|
| `runtime/` | `libwpo`: the object runtime (`osdo.h`, `osdo.c`: C11, C library only), the main loop (`osdo_loop.h`, POSIX implementation) and plug-in loading |
| `tools/osdc.py` | Generates C headers and registration code from `.osdi` class files |
| `service/` | `wpsd` (the per-user object service), `wpsctl` (its command-line client), `wps-watchd` (the optional root helper), `wps-plughost` (runs one plug-in package, sandboxed), change tracker, attribute store, database and volume sync, CBOR protocol |
| `classes/` | The core classes: `OsdObject`, `OsdFileSystem`, `OsdDataFile`, `OsdFolder`, `OsdAbstract`, `OsdProgram`, `OsdShadow` |
| `tests/` | Unit tests, test plug-ins, and one test class in four versions for compatibility checks |
| `proto/m0/` | M0 kernel and filtering prototypes |
| `gui/` | `libwpsui` (the widget set: windows, frames, containers, menus, scroll layers) and `gui/shell/wps-shell` |
| `proto/g0/` | G0 GUI prototypes: a folder window drawn with Cairo and HarfBuzz on Wayland, sound latency through PipeWire |

## Building

On a new Fedora machine (45; GNOME), the packages for everything here:

```
sudo dnf install gcc make python3 sqlite-devel cairo-devel harfbuzz-devel freetype-devel fontconfig-devel \
    wayland-devel wayland-protocols-devel libxkbcommon-devel pipewire-devel systemd-devel \
    wlroots-devel libinput-devel libseat-devel pixman-devel libdrm-devel libxcb-devel xorg-x11-server-Xwayland \
    pam-devel glycin-thumbnailer xdg-user-dirs libasan libubsan gdb
make && make gui && make compositor
tools/install-extension.sh             # the desktop in GNOME (and the wpsd user service)
sudo tools/install-session.sh          # "WPS" on the login screen; then reboot (GDM reads its list at start)
```

The WPS session needs a real seat (a console, or a virtual machine's screen): over RDP, GDM starts sessions headless, which only GNOME's own compositor can serve (docs/open-issues.md).


```
make          # runtime, classes, service, plug-ins, tests (needs gcc, python3, sqlite-devel)
make test     # unit tests
make check    # unit tests + strict C11 core check + ThreadSanitizer run of the loop
make m2       # M2 scenarios: real services on loopback ext4/btrfs/FAT, several users (uses sudo)
make m3       # M3 change-tracking scenarios, with inotify and with wps-watchd (uses sudo)
tests/m4/m4.py  # M4 scenarios (needs the test bed up; scale ones create a 6 GB sparse volume)
tests/m5/m5.py  # M5 scenarios: events, menus, programs, templates, root, plug-in hosts
make g0       # G0 GUI prototypes (needs cairo, harfbuzz, freetype, fontconfig, wayland, xkbcommon, pipewire -devel)
make gui      # wps-shell (optimised) and wps-shell-asan
make compositor     # wps-compositor (G5, wlroots) and its test clients
make session        # everything the WPS session runs: after a pull, this
make install DESTDIR=... PREFIX=/usr   # into /usr (or a staging root): programs, user service, session, PAM file, GNOME extension
make install-check  # install into a scratch root and run a headless session from it
tests/comp/run.sh SCRIPT  # the compositor headless, with WPS's desktop, played by a script
tools/try-compositor.sh   # the compositor nested in a window of your session
tests/gnome/run.sh  # the desktop and its GNOME Shell extension in a private headless GNOME Shell
tests/gnome/with-gnome.sh -- tests/gui/selftest.sh MODE [SETUP]   # one GUI self-test, in a private headless GNOME
```

The WPS desktop in your GNOME session: `tools/install-extension.sh` installs the `wps@wps-linux` GNOME Shell extension (which starts `wps-shell --desktop`) and a systemd user unit for `wpsd`; then log out and back in. `tools/install-extension.sh --uninstall` removes both. To try it first without logging out: `tools/try-desktop.sh` runs a nested GNOME Shell in a window, with the extension and private settings (needs `mutter-devkit`). Without the extension, `wps-shell` with no folder shows the desktop as an ordinary full-screen window.

The tests use AddressSanitizer and UBSan when `libasan` and `libubsan` are installed.

Try the service by hand:

```
build/wpsd &                                  # socket in $XDG_RUNTIME_DIR/wps.sock
build/wpsctl call ~/notes.txt osdSetTitle "My notes"
build/wpsctl resolve ~/notes.txt
getfattr -d -m user.wps ~/notes.txt           # the title travels with the file
build/wps-shell ~/Documents                   # a folder window (Ctrl+1 icons, Ctrl+2 tree, Ctrl+3 details)
build/wpsctl shutdown
```

## Authorship and license

This project started from fond memories of using OS/2, from Warp 3.0 through WSeB to eComStation, and of missing its Workplace Shell. The original design and ideas are OS/2's, from IBM; this is a new implementation of them for Linux, not affiliated with IBM or with the current owners of OS/2.

WPS for Linux is designed and built by Sungsoo Khim. Its code was written with the assistance of Claude (Anthropic's AI model) through Claude Code; the design, decisions and testing are the author's.

Released under the MIT License: see [LICENSE](LICENSE).
