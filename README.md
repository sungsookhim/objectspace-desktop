# WPS for Linux

A desktop for Linux built on the object model of the OS/2 Workplace Shell. Every file, folder and abstract item is a persistent object belonging to a class that can be extended, with shadows, templates and settings notebooks. The object core is built first, with no GUI.

Status: object core done (M0–M5): kernel prototypes, object runtime, persistence and the service, change tracking, scale (shadows, object files, streaming listings, jobs), the service API (events, menus and settings as data, programs, templates) and sandboxed plug-in hosts. Next: the GUI design.

## Documents

| Document | What it holds |
|---|---|
| [docs/object-core-design.md](docs/object-core-design.md) | Object core design v0.17: principles, responsiveness, runtime, classes, identity, persistence, concurrency, multiple logged-in users, links, change tracking, scale, API, tests, milestones |
| [docs/gui-design.md](docs/gui-design.md) | GUI design v0.6: OS/2 Warp 4–4.52 base, modern features, configurable appearance, action events; coexists with GNOME; own drawing; proposed architecture and milestones |
| [docs/open-issues.md](docs/open-issues.md) | Known problems and gaps found while building and trying G2, and what is planned for each |
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
7. GUI (docs/gui-design.md): G0 prototypes done (proto/g0/RESULTS.md); G1 done: the widget set's frames, icon and details views and menus, and wps-shell folder windows (gui/RESULTS-G1.md). Next G2: desktop, tree view, notebooks, drag and drop, live updates.

Related project: [x64dos](https://github.com/sungsookhim/x64dos), a possible second target for the runtime.

## Layout

| Path | Contents |
|---|---|
| `runtime/` | `libwpo`: the object runtime (`wpo.h`, `wpo.c`: C11, C library only), the main loop (`wpo_loop.h`, POSIX implementation) and plug-in loading |
| `tools/wpoc.py` | Generates C headers and registration code from `.wpi` class files |
| `service/` | `wpsd` (the per-user object service), `wpsctl` (its command-line client), `wps-watchd` (the optional root helper), `wps-plughost` (runs one plug-in package, sandboxed), change tracker, attribute store, database and volume sync, CBOR protocol |
| `classes/` | The core classes: `WPObject`, `WPFileSystem`, `WPDataFile`, `WPFolder`, `WPAbstract`, `WPProgram`, `WPShadow` |
| `tests/` | Unit tests, test plug-ins, and one test class in four versions for compatibility checks |
| `proto/m0/` | M0 kernel and filtering prototypes |
| `gui/` | `libwpsui` (the widget set: windows, frames, containers, menus, scroll layers) and `gui/shell/wps-shell` |
| `proto/g0/` | G0 GUI prototypes: a folder window drawn with Cairo and HarfBuzz on Wayland, sound latency through PipeWire |

## Building

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
tests/gnome/run.sh  # the desktop and its GNOME Shell extension in a private headless GNOME Shell
tests/gnome/with-gnome.sh -- tests/gui/selftest.sh MODE [SETUP]   # one GUI self-test, in a private headless GNOME
```

The WPS desktop in your GNOME session: `tools/install-extension.sh` installs the `wps@wps-linux` GNOME Shell extension (which starts `wps-shell --desktop`) and a systemd user unit for `wpsd`; then log out and back in. `tools/install-extension.sh --uninstall` removes both. To try it first without logging out: `tools/try-desktop.sh` runs a nested GNOME Shell in a window, with the extension and private settings (needs `mutter-devkit`). Without the extension, `wps-shell` with no folder shows the desktop as an ordinary full-screen window.

The tests use AddressSanitizer and UBSan when `libasan` and `libubsan` are installed.

Try the service by hand:

```
build/wpsd &                                  # socket in $XDG_RUNTIME_DIR/wps.sock
build/wpsctl call ~/notes.txt wpSetTitle "My notes"
build/wpsctl resolve ~/notes.txt
getfattr -d -m user.wps ~/notes.txt           # the title travels with the file
build/wps-shell ~/Documents                   # a folder window (Ctrl+1 icons, Ctrl+2 tree, Ctrl+3 details)
build/wpsctl shutdown
```
