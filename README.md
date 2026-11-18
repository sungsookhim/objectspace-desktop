# WPS for Linux

A desktop for Linux built on the object model of the OS/2 Workplace Shell. Every file, folder and abstract item is a persistent object belonging to a class that can be extended, with shadows, templates and settings notebooks. The object core is built first, with no GUI.

Status: design. No code yet.

## Documents

| Document | What it holds |
|---|---|
| [docs/object-core-design.md](docs/object-core-design.md) | Object core design v0.1: runtime, classes, identity, persistence, links, change tracking, API, tests, milestones |
| [docs/reviews/2026-11-18-gemini-review.md](docs/reviews/2026-11-18-gemini-review.md) | External review of v0.1, our assessment, and the changes accepted for v0.2 |
| [docs/notes/2026-11-discussion-notes.md](docs/notes/2026-11-discussion-notes.md) | The discussion that led to the design: approach options, prior art, change tracking, links |

The working copy of the design is also kept as a Claude doc: https://claude.ai/artifact/1ucowX5KxZNaXLJaHGXZr4

## Plan

1. Fold the accepted review changes into design v0.2.
2. M0: a fanotify tracking prototype that confirms the kernel behaviour the design relies on.
3. M1–M5: runtime, persistence and identity, change tracking, shadows, API.
4. Then design the GUI on top of the object core.

Related project: [x64dos](https://github.com/sungsookhim/x64dos), a possible second target for the runtime.
