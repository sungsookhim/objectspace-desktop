# Contributing to Objectspace

Thank you for your interest. Issues (problems, questions, ideas) and pull requests are welcome. Everyone taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md).

## Before you start

- **A problem:** open an issue with what you did, what you expected and what happened, your Fedora version, and whether you were in the Objectspace session or in GNOME. The logs help: `~/.local/state/objectspace/session.log` (the session) and `journalctl --user -u osdd` (the object service).
- **A small fix** (a crash, a wrong word, a drawing glitch): a pull request is fine straight away.
- **Something larger** (a new feature, a new class, a change to how objects or settings behave): open an issue first, so the design can be agreed before the code is written. Objectspace follows OS/2's Workplace Shell where it can, and the design documents in `docs/` record what was decided and why.

## Building and running

See the README's *Building from source* for the packages (Fedora 45). Then:

```
make && make gui && make session
```

`tools/try-compositor.sh` runs the session's compositor in a window of your desktop, which is the quickest way to see a change. Never run `tools/osd-session` inside a running graphical session: it changes systemd's user environment.

## Tests

Before a pull request, these should pass:

```
make check          # unit tests, the service's scenarios, the licence lines
make gui-test       # the widget set's unit tests
make comp-test      # the compositor and the desktop, headless
```

For a change to how things look, also `make visual-check` (screenshots in each scheme against a baseline you took before the change with `make visual-baseline`), and say in the pull request which differences are intended. `tests/gui/golden.sh` and `golden-compare.sh` run the GUI self-tests and compare them the same way.

New behaviour comes with a test: a scenario in `tests/` for the service, a headless script in `tests/comp/` for the desktop and the compositor (see `tests/comp/run.sh`), or a self-test mode in `gui/shell/selftest.c`. Continuous integration runs the suites on every push and pull request.

## The code

- **C:** `gnu11` (the object runtime in `runtime/` is strict C11 and uses only the C library: `make check` checks it). Four spaces, no tabs; braces on the same line; names in `lower_case`, the project's prefix `osd_`/`OSD_` for anything shared between files.
- **Classes** are declared in `.osdi` files in `classes/`, with their properties, and `tools/osdc.py` generates their C bindings; see `docs/object-properties-design.md`.
- **Python** (tools and tests): the standard library, and Pillow where pictures are made or compared.
- **Comments** say what a part is for and why it is so, in plain sentences, with the design document's section where there is one.
- **Every file** starts with the licence lines (`SPDX-License-Identifier: MIT` and the copyright); `python3 tools/spdx.py --add` adds them to new files.
- **What the user reads** (menus, notebooks, messages) follows OS/2's words where OS/2 had the thing, and plain words otherwise.

## Pull requests and commits

- One change per pull request, with a description of what it changes and how it was tested.
- Commit messages: a short first line saying what changed, then why, wrapped at about 72 columns.
- By contributing, you agree that your contribution is released under the project's [MIT License](LICENSE).

## The documents

`docs/` holds the designs (`object-core-design.md`, `gui-design.md`, `object-properties-design.md`, `cli-design.md`), each version's design and plan, the release notes, and the roadmap. A change that alters a design updates its document in the same pull request. The documents from before 0.5 use the project's old name, WPS; they are kept as written (see `docs/HISTORY.md`).
