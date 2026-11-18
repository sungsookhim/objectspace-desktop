# WPS for Linux

A desktop for Linux built on the object model of the OS/2 Workplace Shell. Every file, folder and abstract item is a persistent object belonging to a class that can be extended, with shadows, templates and settings notebooks. The object core is built first, with no GUI.

Status: design. No code yet.

## Documents

| Document | What it holds |
|---|---|
| [docs/object-core-design.md](docs/object-core-design.md) | Object core design v0.2: principles, responsiveness, runtime, classes, identity, persistence, concurrency and multiple users, links, change tracking, scale, API, tests, milestones |
| [docs/reviews/2026-11-18-gemini-review.md](docs/reviews/2026-11-18-gemini-review.md) | External review of v0.1, our assessment, and the changes folded into v0.2 |
| [docs/notes/2026-11-discussion-notes.md](docs/notes/2026-11-discussion-notes.md) | The discussion that led to the design: approach options, prior art, change tracking, links |

The repository copy is the source of truth. The Claude doc at https://claude.ai/artifact/1ucowX5KxZNaXLJaHGXZr4 holds v0.1 with the architecture diagram.

## Plan

1. M0: a fanotify tracking prototype that confirms the kernel behaviour the design relies on.
2. M1–M5: runtime, persistence and identity, change tracking, shadows, API.
3. Then design the GUI on top of the object core.

Related project: [x64dos](https://github.com/sungsookhim/x64dos), a possible second target for the runtime.
