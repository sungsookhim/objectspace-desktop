# Objectspace 0.5: a name of its own, and public

Released 2026-11-24. Up to 0.4.1 this project was private and named WPS (a Workplace Shell for Linux). 0.5 is its first public version, under a name of its own, **Objectspace**, so as not to use the name of IBM's Workplace Shell, which inspired it. Besides the names, it brings a handful of fixes and small additions; nothing else changes in how it works. See [HISTORY.md](HISTORY.md).

OS/2 and Workplace Shell are trademarks of IBM. Objectspace is not affiliated with or endorsed by IBM.

## Install

On Fedora 45, download the RPMs from this release, then:

```
sudo dnf install ./objectspace-0.5.0-1.fc45.x86_64.rpm ./objectspace-gnome-shell-extension-0.5.0-1.fc45.noarch.rpm
```

Add `./objectspace-devel-0.5.0-1.fc45.x86_64.rpm` to write plug-ins.

Reboot (the login screen reads its list of sessions at start), then either choose **Objectspace** on the login screen, or log in to GNOME and run `gnome-extensions enable objectspace@sungsookhim.github.io`, then log out and in.

To remove: `sudo dnf remove objectspace objectspace-gnome-shell-extension objectspace-devel`; your objects' state in `~/.local/share/objectspace` and `~/.config/objectspace` is kept.

## Coming from WPS 0.4.1

0.5 is a fresh start: it does not take over 0.4.1's settings, positions or objects, and the two are separate packages. Remove 0.4.1 first, then install 0.5:

```
sudo dnf remove wps wps-gnome-shell-extension wps-devel
```

The old state can then be deleted by hand if you like: `~/.config/wps`, `~/.local/share/wps`, `~/.local/state/wps`. The `user.wps.*` attributes 0.4.1 left on files are harmless and ignored.

What is called what now:

| WPS 0.4.1 | Objectspace 0.5 |
|---|---|
| WPS (the session on the login screen, the product) | Objectspace |
| `wps`, `wpsctl`, `wps-shell`, `wps-terminal`, `wps-session` | `osd`, `osdctl`, `osd-shell`, `osd-terminal`, `osd-session` (also as `objectspace`, `objectspace-ctl`, `objectspace-shell`, `objectspace-terminal`, `objectspace-session`) |
| `wpsd`, `wps-watchd`, `wps-plughost`, `wps-compositor` | `osdd`, `osd-watchd`, `osd-plughost`, `osd-compositor` |
| WPS Terminal | Objectspace Terminal |
| WarpCenter, LaunchPad | TheBar, Launcher |
| Icon themes WPS (flat), WPS (outline) | Objectspace (flat), Objectspace (outline) |
| Packages `wps`, `wps-devel`, `wps-gnome-shell-extension` | `objectspace`, `objectspace-devel`, `objectspace-gnome-shell-extension` |
| GNOME extension `wps@wps-linux` | `objectspace@sungsookhim.github.io` |
| App IDs `org.wps.*` | `io.github.sungsookhim.Objectspace.*` |
| `~/.config/wps`, `~/.local/share/wps`, `~/.local/state/wps` | `~/.config/objectspace`, `~/.local/share/objectspace`, `~/.local/state/objectspace` |
| Attributes `user.wps.*` | `user.osd.*` |
| Variables for scripts `WPS_FOLDER`, `WPS_OBJECT`, `WPS_PATH`, ... | `OSD_FOLDER`, `OSD_OBJECT`, `OSD_PATH`, ... |
| `.wpsicon` icon packages, `.wpobj` object files | `.osdicon`, `.osdobj` |

**For plug-in authors:** the SDK's names changed with the rest: classes `WPObject`, `WPFolder`, ... are `OsdObject`, `OsdFolder`, ...; methods `wpQueryTitle`, `wpclsQueryTitle`, ... are `osdQueryTitle`, `osdclsQueryTitle`, ...; class interface files `.wpi` are `.osdi`, compiled by `osdc`; the runtime's `wpo_` functions are `osdo_`. A 0.4.1 plug-in is renamed the same way and rebuilt against `objectspace-devel`.

## What's new

- **Command Prompt in My System:** your shell in Objectspace Terminal, in your home folder (or the folder its Program page names).
- **Properties notebooks show every label in full:** the label column is as wide as the window's longest label, long labels wrap onto more lines, choice lists are as wide as their longest choice, and each window opens large enough for all its pages.
- **TheBar's tray** is called **Status icons** in TheBar's notebook (it shows other programs' status icons); OS/2's WarpCenter trays of objects are planned for 0.6.

## Fixed

- **In a container** (toolbox, distrobox) whose root filesystem is not a disk, no objects could be made: the root filesystem is now always a volume.
- **A new desktop's icons:** on the first login, My System and Programs came in no fixed order, sometimes after an empty spot the Trash Can had left when it went to its corner. Now your own files come first by name, then Objectspace's objects by title, with no gaps.

## For contributors

The repository has a README for newcomers, [CONTRIBUTING.md](../CONTRIBUTING.md), a [Code of Conduct](../CODE_OF_CONDUCT.md) and a [security policy](../SECURITY.md). Every push is built and tested on Fedora 45 by GitHub Actions: the build, `make check`, `gui-test`, `install-check` and the compositor's headless tests.

## Known issues

- Programs that hand a file to a copy already running (most GNOME applications, Firefox, Ptyxis) do not join a work area's group: their windows belong to that copy.
- The full list of what is open or planned is in [roadmap.md](roadmap.md) and [open-issues.md](open-issues.md).

## Authorship and license

Designed and built by Sungsoo Khim, with code written with the assistance of Claude (Anthropic) through Claude Code. The design and ideas are OS/2's Workplace Shell, from IBM; this project is not affiliated with IBM or the current owners of OS/2. MIT License.
