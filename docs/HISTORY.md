# History, and the old name

**Up to 0.4.1 this project was private and named WPS** (a Workplace Shell for Linux). **0.5, its first public version, took its own name, Objectspace,** so that it would not use the name of IBM's Workplace Shell, which inspired it. OS/2 and Workplace Shell are IBM's trademarks; Objectspace is not affiliated with IBM.

The documents in `docs/` from before 0.5 are kept as they were written: the designs, the plans, the release notes, the discussion notes and the review. They say WPS, and they use the old names for the commands, classes and files. This table gives today's names.

| In the old documents | Today |
|---|---|
| WPS, WPS for Linux | Objectspace |
| `wps`, `wpsctl`, `wps-shell`, `wps-terminal`, `wps-session` | `osd`, `osdctl`, `osd-shell`, `osd-terminal`, `osd-session` (each also as `objectspace`, `objectspace-ctl`, ...) |
| `wpsd`, `wps-watchd`, `wps-plughost`, `wps-compositor` | `osdd`, `osd-watchd`, `osd-plughost`, `osd-compositor` |
| Classes `WPObject`, `WPFolder`, ... and the `wps_`/`WPS_` prefixes in the code | `OsdObject`, `OsdFolder`, ... and `osd_`/`OSD_` |
| Class interface files `.wpi` | `.osdi` |
| `user.wps.*` attributes, `WPS_*` variables, `~/.config/wps` | `user.osd.*`, `OSD_*`, `~/.config/objectspace` |
| WarpCenter, LaunchPad (OS/2's names) | TheBar, Launcher |

The full list, and how the change was made, are in [v0.5-names.md](v0.5-names.md) and [v0.5-plan.md](v0.5-plan.md).

## The versions

| Version | Date | What it was |
|---|---|---|
| 0.1 | 2026-11-21 | The object core (classes, persistence, identity, change tracking, shadows, trash) and the first GUI |
| 0.2 | 2026-11-21 | The OS/2-style desktop: live folders, Trash Can, Drives, Programs, System, TheBar; an RPM for Fedora; its own session (osd-compositor, on wlroots) |
| 0.3 | 2026-11-22 | Daily use and OS/2 fidelity: the desktop's menu, backgrounds, sorting, search, icon layout, Open With, templates, fonts and sounds, the Launcher |
| 0.4, 0.4.1 | 2026-11-24 | Icon themes and sizes, free icon layout, Prevent delete, the Colour Palette, Work Area folders, Objectspace Terminal and the command line, the plug-in SDK; 0.4.1 its fixes (tagged `v0.4.1`) |
| 0.5 | | The first public version: the new name, nothing else new |

The release tags `v0.1` to `v0.4.1` are kept in this repository, with the whole history. The project was designed by its owner, and the code was written with Claude through Claude Code.
