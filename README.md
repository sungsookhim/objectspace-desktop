# WPS for Linux

A desktop for Linux built on the object model of the OS/2 Workplace Shell. Every file, folder and abstract item is a persistent object belonging to a class that can be extended, with shadows, templates and settings notebooks. The object core is built first, with no GUI.

Status: M0 (kernel prototypes) done; M1 (object runtime) first version done.

## Documents

| Document | What it holds |
|---|---|
| [docs/object-core-design.md](docs/object-core-design.md) | Object core design v0.4: principles, responsiveness, runtime, classes, identity, persistence, concurrency, multiple logged-in users, links, change tracking, scale, API, tests, milestones |
| [docs/reviews/2026-11-18-gemini-review.md](docs/reviews/2026-11-18-gemini-review.md) | External review of v0.1, our assessment, and the changes folded into v0.2 |
| [docs/notes/2026-11-discussion-notes.md](docs/notes/2026-11-discussion-notes.md) | The discussion that led to the design: approach options, prior art, change tracking, links |

The repository copy is the source of truth. The Claude doc at https://claude.ai/artifact/1ucowX5KxZNaXLJaHGXZr4 holds v0.1 with a drawn architecture diagram.

## Plan

1. M0: kernel-behaviour and per-user filtering prototypes. Done; findings folded into design v0.4.
2. M1: object runtime, main loop, class generator, core classes. First version done.
3. M2–M5: persistence and identity, change tracking, shadows and jobs, API and plug-in host.
4. Then design the GUI on top of the object core.

Related project: [x64dos](https://github.com/sungsookhim/x64dos), a possible second target for the runtime.

## Layout

| Path | Contents |
|---|---|
| `runtime/` | `libwpo`: the object runtime (`wpo.h`, `wpo.c`: C11, C library only), the main loop (`wpo_loop.h`, POSIX implementation) and plug-in loading |
| `tools/wpoc.py` | Generates C headers and registration code from `.wpi` class files |
| `classes/` | The core classes: `WPObject`, `WPFileSystem`, `WPDataFile`, `WPFolder`, `WPAbstract`, `WPProgram`, `WPShadow` |
| `tests/` | Unit tests, test plug-ins, and one test class in four versions for compatibility checks |
| `proto/m0/` | M0 kernel and filtering prototypes |

## Building

```
make          # runtime, classes, plug-ins, tests (needs gcc and python3)
make test     # run the unit tests
```

The tests use AddressSanitizer and UBSan when `libasan` and `libubsan` are installed.
