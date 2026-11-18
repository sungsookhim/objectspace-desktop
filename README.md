# WPS for Linux

A desktop for Linux built on the object model of the OS/2 Workplace Shell. Every file, folder and abstract item is a persistent object belonging to a class that can be extended, with shadows, templates and settings notebooks. The object core is built first, with no GUI.

Status: object core done (M0–M5): kernel prototypes, object runtime, persistence and the service, change tracking, scale (shadows, object files, streaming listings, jobs), the service API (events, menus and settings as data, programs, templates) and sandboxed plug-in hosts. Next: the GUI design.

## Documents

| Document | What it holds |
|---|---|
| [docs/object-core-design.md](docs/object-core-design.md) | Object core design v0.13: principles, responsiveness, runtime, classes, identity, persistence, concurrency, multiple logged-in users, links, change tracking, scale, API, tests, milestones |
| [docs/gui-design.md](docs/gui-design.md) | GUI design v0.3: OS/2 Warp 4–4.52 base, modern features, configurable appearance, action events; coexists with GNOME; own drawing; proposed architecture and milestones |
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
7. GUI (docs/gui-design.md): G0 prototypes done (proto/g0/RESULTS.md); next G1, the widget set's frames, containers and menus.

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
```

The tests use AddressSanitizer and UBSan when `libasan` and `libubsan` are installed.

Try the service by hand:

```
build/wpsd &                                  # socket in $XDG_RUNTIME_DIR/wps.sock
build/wpsctl call ~/notes.txt wpSetTitle "My notes"
build/wpsctl resolve ~/notes.txt
getfattr -d -m user.wps ~/notes.txt           # the title travels with the file
build/wpsctl shutdown
```
