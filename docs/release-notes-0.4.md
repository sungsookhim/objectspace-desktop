# WPS for Linux 0.4: icons, folders and objects

Released 2026-11-23. Everything you set is now a property of an object: one folder, all folders, a file type or a whole class, with OS/2's Class... and Default. On that base: icon themes and sizes, OS/2's free icon layout, folder windows that remember where they were, the Colour Palette, fonts in every window, and Work Area folders.

## Install

Download the RPMs from this release, then:

```
sudo dnf install ./wps-0.4.0-1.fc45.x86_64.rpm ./wps-gnome-shell-extension-0.4.0-1.fc45.noarch.rpm
```

Add `./wps-devel-0.4.0-1.fc45.x86_64.rpm` to write plug-ins (see *Plug-ins and the SDK*).

On a machine with 0.3, the same command upgrades it; log out and in again afterwards. Your objects, positions and settings are kept and carried over (see *Upgrading from 0.3*).

New installs: reboot (the login screen reads its list of sessions at start), then either choose **WPS** on the login screen, or log in to GNOME and run `gnome-extensions enable wps@wps-linux`, then log out and in.

To remove: `sudo dnf remove wps wps-gnome-shell-extension wps-devel`; your objects in `~/.local/share/wps` and `~/.config/wps` are kept.

## What's new since 0.3

### Settings as properties of objects

- **One object, or all of them.** Every setting is a property, looked up from the object, then its file type, then its class and the classes above it, then the scheme, then the default. A notebook field shows where an inherited value comes from ("from: all folders").
- **Class...** on any notebook opens the notebook of all objects of that class, or of that file type ("all image files"): set a folder's sort, layout, icon theme or background there, and every folder follows unless it has its own.
- **Default** on a page removes an object's own values: it goes back to what it inherits.
- **Actions** (a new page): what happens when an object is opened, made, put in the Trash Can or deleted: a sound, a command, a method, or settings, for one object, a file type or a class. Work areas add their own events.
- **Schemes are objects:** Classic, Dark and High contrast live in System Setup's Schemes folder; **Create another** makes your own, and every colour, size and font of WPS's windows is one of its fields (including the WarpCenter's and the LaunchPad's colours and their meters).
- `wpsctl get`, `set`, `props` and `reset` do the same from the command line, with setup strings (`icon_font=12.Sans Bold;bg_color=#336699`).

### Icons

- **Icon themes:** WPS (flat), WPS (outline) and Classic, chosen for everything on the System's **Theme** page, or for one folder (and what is in it) on its own. WPS's own objects and every file type have pictures from the theme.
- **Icon sizes:** Small, Default and Large, for everything or per folder; menus, the LaunchPad and the WarpCenter follow. Icons keep their arrangement when the size changes.
- **Open icons:** a folder shows its open picture while its window is open; any object can have its own **Open icon**; **Hatched while open** (OS/2's hatching) per object or for all.
- **Choose...** takes `.wpsicon` packages, and Windows `.ico` and macOS `.icns` files.

### Folders

- **OS/2's free icon layout** (the default): each icon as wide as its title, rows as tall as their tallest; Arrange and sort in it; the grid is a choice on the Icon view page. A big folder opens at its first icons, in name order.
- **Folder windows remember** their place, size, view and scroll position, per view; a plain Open opens the view last closed.
- **The search field** at the top, the bottom, or floating over the list.
- **The desktop's messages** in a box above the WarpCenter; the column edges show the resize pointer.
- A pop-up menu's object is outlined while its menu is open (OS/2's source emphasis); a scroll bar's arrows repeat while held.

### Objects and programs

- **Prevent delete** (the General page): such an object cannot be trashed or deleted; WPS's own folders and objects have it.
- **Renaming to another extension** asks first: *Use .md*, *Keep .txt* or *Cancel*.
- **A shadow's menu is its original's** (Open With on a shadow of a file, Empty trash on a shadow of the Trash Can).
- **Executables have a Program page:** window or text mode, close the terminal on exit, the working directory.
- Programs started from WPS come to the front (activation tokens).

### The Colour Palette

- **OS/2's Colour Palette** in System Setup: drag a colour onto a folder or the desktop (its background), with **Ctrl** (its icons' titles), with **Shift** (every folder's); onto a folder's title bar (that folder's own title bar colour; with Shift, the scheme's for every window); onto the WarpCenter or the LaunchPad; or onto a notebook's colour field. **Edit colour...** with a hue and saturation square, brightness, and `#rrggbb`.
- The Background page's swatch shows the colour in use, and a picture shows as a small preview.

### Fonts

- **A Dialogs font**, each scheme's own: notebooks, dialogs, Find, the Window List, the palettes and the pickers, laid out larger with a larger font. Drop a font from the Font Palette on one of them to set it; on a notebook's font field, that field.
- **Font sizes follow GNOME's text scaling.**
- **Edit font has a Style list** from the fonts installed (Light, Medium, SemiBold, ...); a font name can say its style ("10.Noto Sans Medium").
- Other programs' system menus (in the WPS session) are drawn as WPS's menus, in the menus' font.

### Work Area folders

- **Work area** on a folder's new **File** page (OS/2's): what you open from it (programs and files, folders through shadows or links, folders in it) is its group. Minimize its window and the group minimizes; restore it, it comes back; close it, its programs are asked to close, and opening it again opens them again.
- **At logout** what a work area had open is kept; at the next login it opens again with it, even with *Restore open windows at login* off.
- In the WPS session and inside GNOME.

### Plug-ins and the SDK

- Plug-ins declare properties, events and file types; their pages, setters and menu items work like the built-in classes'. The **wps-devel** package has the headers, the classes' interfaces, `wpoc`, and a sample; `wpsctl plugin new NAME` makes a skeleton.

## Upgrading from 0.3

At the first login with 0.4, wpsd carries your settings over once: the System's folder and font settings become *all folders'* (`class:WPFolder`), `sounds.conf` moves into the Sound object, a folder's sort and columns become its properties; Prevent delete is put on WPS's own folders and objects; the Colour Palette is added to System Setup. Icon positions are kept (the free layout may place unarranged icons differently; **Arrange** lays a folder out anew). Run `tools/install-extension.sh` again only if you installed from source.

## Known issues

- Programs that hand a file to a copy already running (most GNOME applications, Firefox, Ptyxis) do not join a work area's group: their windows belong to that copy.
- A notebook open while the Dialogs font changes keeps its size until opened again.
- Deferred: a Preview view (pictures in place of their icons), the theme's picture for text-mode programs. The full list is in [docs/roadmap.md](roadmap.md).

## Authorship and license

Designed and built by Sungsoo Khim, with code written with the assistance of Claude (Anthropic) through Claude Code. The design and ideas are OS/2's Workplace Shell, from IBM; this project is not affiliated with IBM or the current owners of OS/2. MIT License.
