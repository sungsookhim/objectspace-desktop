# WPS for Linux 0.3: daily use and OS/2 fidelity

Released 2026-11-22. Daily use and OS/2 fidelity: folders that sort, search and keep their places; Open With and program settings; templates where you need them; OS/2's Font Palette; a LaunchPad that matches the WarpCenter.

## Install

Download the two RPMs from this release, then:

```
sudo dnf install ./wps-0.3.0-1.fc45.x86_64.rpm ./wps-gnome-shell-extension-0.3.0-1.fc45.noarch.rpm
```

On a machine with 0.2, the same command upgrades it; log out and in again afterwards. Your objects, positions and settings are kept, and the first login after the upgrade brings the desktop up to date (see *Upgrading from 0.2*).

New installs: reboot (the login screen reads its list of sessions at start), then either choose **WPS** on the login screen, or log in to GNOME and run `gnome-extensions enable wps@wps-linux`, then log out and in.

To remove: `sudo dnf remove wps wps-gnome-shell-extension`; your objects in `~/.local/share/wps` and `~/.config/wps` are kept.

## What's new since 0.2

### The desktop

- **The desktop's menu**, as OS/2's: System setup (opens the System Setup folder), Lockup now, Shut down..., after the folder's own items; **New ▸** on every folder's background menu.
- **The desktop's own background:** a picture (any format GNOME reads, WebP and AVIF too), how it is shown, a colour; or **GNOME's background** (the default). GNOME's own setting is never changed.
- **Folders' backgrounds:** a picture or a colour per folder, on its Background page, with Browse... and a colour palette.
- **Titles:** the System folder is *My System - hostname*, Computer is *My Storages* (with a shadow of your home folder in it). WPS's own folders cannot be renamed, and are put back if renamed outside WPS.
- **The WarpCenter's buttons:** Lockup, Find, Log off and Shut down.
- **Super**, pressed alone, opens the WPS menu in the WPS session; pressed again, it closes it.

### Folders

- **Details view:** click a heading to sort (again: the other way), folders first; choose the columns (Real name, Type, Size, Last write) and drag their widths; kept per folder.
- **Search field:** Ctrl+F shows a field under the title bar that filters the folder as you type; Ctrl+F again or Escape takes it away. Ctrl+Shift+F is Find.
- **Hidden files** shown or not (System: Folders); lock files are never listed.
- **OS/2's icon layout:** every title shown whole; each row as tall as its tallest title. A folder's Icon view page can go back to two lines.
- **Lock position** on an icon's menu and its General page, or for all icons of a folder: a locked icon stays where it is in its folder (dragging it there shows the no-drop pointer) and Arrange works around it; it can still be dragged to other folders and objects.
- **Show file extensions in titles** (System: Folders; on by default): off, data files are titled without them, and renaming keeps the file's extension.

### Programs

- **Open ▸** on data files: the default program (checked), the others for the type, and **Other program...** (every installed program, with **Always use for this type**, shared with GNOME).
- **The Program page:** working directory, program type (automatic, window, text mode), and **Close window on exit** for text-mode programs.
- **The terminal** for text-mode programs, chosen in System: Programs (or a command of your own).
- **The Association page** of programs: the file types they open; changes are written to your own copy of the program's entry.

### Templates

- **New ▸** on background menus: the Templates folder's templates, made where you clicked.
- **Create another** asks for a name and a folder.
- A template dragged makes a new object (also within its own folder); **Shift+drag** moves the template itself.

### Fonts and sounds

- **The Font Palette** (System Setup): font samples to drag onto a folder (its icons' font), its title bar (that folder's title bar), the WarpCenter; with **Shift**, every folder or every title bar. Edit font... changes a sample; a sample's menu sets the menus' font too.
- **The Sound object:** each event a sound file or theme sound, with Browse..., None and **Play**.

### The LaunchPad

- Its drawers' folders open as cascades with icons, as in the WarpCenter's menu; an open drawer follows its folder.
- An object button's menu is the object's own (Open, Properties, Open With, ...).
- **Tooltips** on the LaunchPad and on the WarpCenter (the clock's date, the meters' values, tray icons).
- New settings: horizontal or vertical (drawers on the left or right, opening outward), small icons, text on buttons, float on top, close drawer after open.

### Also

- Manual pages for `wpsctl`, `wps-shell` and `wps-session`.
- Fixed: `.c` files were taken for C++ sources; a Trash Can window missed changes on new accounts; delete in tree view now takes the whole selection.

## Upgrading from 0.2

At the first login with 0.3, wpsd updates the desktop once: the System folder and Computer get their new titles (Computer's folder becomes *Storages*; its shadows follow), a shadow of your home folder is put in My Storages, and the Font Palette is added to System Setup. Icon positions stored by 0.2 were laid out for two-line titles: in folders with long names, titles may overlap the row below until you choose **Arrange**.

## Known issues

- Programs other than WPS's own windows have their title bars in the font for all title bars, not a folder's.
- Float on top (the LaunchPad) and Super for the WPS menu apply in the WPS session; inside GNOME, GNOME keeps its own.
- Deferred: OS/2's free icon layout (variable widths), folder windows remembering their place and size, a confirmation when a rename changes a file's extension, icon packages, Work Area folders. The full list is in [docs/roadmap.md](roadmap.md).

## Authorship and license

Designed and built by Sungsoo Khim, with code written with the assistance of Claude (Anthropic) through Claude Code. The design and ideas are OS/2's Workplace Shell, from IBM; this project is not affiliated with IBM or the current owners of OS/2. MIT License.
