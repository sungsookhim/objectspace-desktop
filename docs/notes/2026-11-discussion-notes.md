# Discussion notes: WPS for Linux (November 2026)

The conversation that led to the object core design, in order. Effort figures are estimates for one experienced developer.

## 1. Building a WPS-like desktop on Linux

**Route 1: on top of GTK/GLib.** GObject plays SOM's role, with GObject-Introspection for name-based calls and other languages. Persistence uses extended attributes plus SQLite. Integration uses wlr-layer-shell, foreign-toplevel and xdg-shell. This is the faster route to something usable.

**Route 2: own toolkit, with Wayland only as a pixel and input manager.** Three ways to read "Wayland as a bitmap manager":

| Option | How | Linux apps? | Folder positions remembered? |
|---|---|---|---|
| A. Own toolkit as a normal Wayland client | Fill your own windows with pixels | Yes | No: the compositor decides placement |
| B. Own compositor on wlroots (recommended) | wlroots provides outputs, input and protocol; you draw the desktop and frames | Yes | Yes |
| C. Bare DRM/KMS + libinput | Draw straight to the screen | No | Yes |

Use FreeType, HarfBuzz and xkbcommon rather than writing text or keymap handling; pixman is optional for drawing.

| Piece | Effort |
|---|---|
| Drawing layer | 2–4 weeks (days with pixman) |
| Text rendering | 2–4 weeks |
| Input: focus, capture, keyboard navigation | 2–3 weeks |
| Controls, including icon container and settings notebook | 3–5 months |
| Text entry and input methods | 1–2 months |
| High-DPI and theming | about 1 month |
| Compositor on wlroots | 2–4 months |

Totals: prototype about 2–3 months, usable daily about 12–15 months, polished about 2–2.5 years. Route 2 gives up accessibility (AT-SPI), much internationalisation and mature text editing for free. It gains an exact PM/WPS look, a small stack, and **portability**: a toolkit behind a ~10-function pixel and input interface can also run on an X64-DOS framebuffer.

## 2. Prior art

No one has replicated the Workplace Shell on Linux.

- **ArcaOS** (Arca Noae): commercial OS/2 continuation shipping IBM's real WPS and SOM. Best reference for behaviour, in a VM.
- **XWorkplace** (netlabs, GPL): a WPS enhancer, not a replacement; its source shows how real WPS classes are written.
- **Voyager** (netlabs): planned WPS reimplementation on a modern base; never produced a usable result.
- **osFree**: open-source OS/2 clone effort; revived several times, WPS work never got far.
- Partial ideas on Linux: GNOME 2's spatial Nautilus (later dropped), Haiku's Tracker, ROX Desktop, Étoilé, and Warp 4 themes that copy only the look.

Earlier attempts failed by aiming at full OS/2 compatibility with volunteer effort. This project copies the WPS model, not OS/2.

## 3. Evaluation and starting point

Worth building; the space is empty. The hard part is the object model, not graphics, so start with the object core and no GUI:

- C with its own small SOM-like runtime (shareable with X64-DOS), not GObject.
- A minimal class tree: `WPObject`, `WPFileSystem`, `WPDataFile`, `WPFolder`, `WPAbstract`, `WPProgram`, `WPShadow`.
- Extended attributes for file state, SQLite for abstract objects.
- Object handles that survive moves: filesystem UUID + inode identity + cached path.
- A command-line client (`wpsctl`) and automated tests before any GUI.

## 4. Tracking changes made outside WPS

OS/2's extended attributes were themselves layered on top of an ordinary filesystem, so storing state in Linux extended attributes with a SQLite fallback is the same compromise. The question was how to notice changes made from the command line.

- Access time is unsuitable: `relatime`/`noatime` make it update at most daily, and it records reads, not moves. mtime and ctime are useful for catching up.
- Most command-line activity is already handled: `mv` within a filesystem keeps the inode, and the attributes move with it.
- Problem cases: shadows pointing to moved files, atomic saves that replace the inode, moves across filesystems.
- **inotify**: no root needed, but per-directory, about 1 KB of kernel memory per watch, renames as paired events, overflow forces a rescan. Good for open folders.
- **fanotify**: one mark per filesystem, `FAN_RENAME` (5.17+) as one event, file handles instead of paths. Needs `CAP_SYS_ADMIN` for filesystem-wide marks, hence a small privileged helper.
- Use `name_to_handle_at()` handles as identity: they include the inode generation, so reused inode numbers do not collide.
- Catch up at mount: btrfs generations (`find-new`), a ctime-pruned walk on ext4, a path-based scan on FAT.

Decision: use both, mostly fanotify.

## 5. Symlinks, hard links and shadows

| | Symlink | Hard link | WPS shadow |
|---|---|---|---|
| What it is | A small file holding a path | Another name for the same inode | An object pointing to another object |
| Target moves | Breaks | Unaffected | Follows |
| Across filesystems | Yes | No | Yes |
| Own extended attributes | Not allowed for `user.*` | Shared with the other names | Yes |

Decisions:
- Shadows of files and folders are **managed symlinks**, kept correct by the service and visible to every Linux program.
- Shadows of abstract objects live only in the database.
- User-made symlinks are shown as links and never rewritten unless the user chooses "Track target".
- Hard links are one object in several folders; per-folder state is stored on the folder, as OS/2's `.ICONPOS` did.
- Never follow symlinks in tree walks.

## 6. Outcome

The design doc [../object-core-design.md](../object-core-design.md) captured these decisions (v0.1, 2026-11-17). An external review with our response is in [../reviews/2026-11-18-gemini-review.md](../reviews/2026-11-18-gemini-review.md). The GUI is designed on top of the object core afterwards.
