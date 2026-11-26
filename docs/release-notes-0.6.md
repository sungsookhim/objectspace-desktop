# Objectspace 0.6: TheBar's trays and folder windows

Released 2026-11-25. TheBar gets trays, folder windows a menu bar and two new views (Column and Gallery), pictures can stand in for their icons, and the session suspends after a while idle. The six steps of [v0.6-plan.md](v0.6-plan.md), each checked by the owner on a VM. 0.6.0-alpha.1 was a preview of the first two.

## Install

On Fedora 45, download the RPMs from this release, then:

```
sudo dnf install ./objectspace-0.6.0-1.fc45.x86_64.rpm ./objectspace-gnome-shell-extension-0.6.0-1.fc45.noarch.rpm
```

Add `./objectspace-devel-0.6.0-1.fc45.x86_64.rpm` to write plug-ins.

Over 0.5.0 or 0.6.0-alpha.1 it is an upgrade: log out and in again afterwards. Your objects and settings are kept; at the first login the desktop gets the new objects (Find in My System, the Trays folder and the first tray). On a new install, reboot first (the login screen reads its list of sessions at start), then choose **Objectspace** on the login screen.

To remove: `sudo dnf remove objectspace objectspace-gnome-shell-extension objectspace-devel`; your objects' state in `~/.local/share/objectspace` and `~/.config/objectspace` is kept.

For pictures of PDFs, videos and other files, install the thumbnailers you want (for example `glycin-thumbnailer` for images, `evince-thumbnailer` for PDFs, `totem-video-thumbnailer` or `ffmpegthumbnailer` for videos); Objectspace uses whatever is installed.

## What's new since 0.5

- **TheBar's trays** (OS/2 Warp 4's WarpCenter trays): named sets of objects on the bar, one shown at a time, after the system buttons. The tray button shows the current tray; a click goes to the next, its arrow opens the trays' menu (choose, add, rename, delete). A click on an object opens it; its menu has **Remove from tray**; objects dropped on the bar join the tray. The first tray, **Default**, holds My System, Programs, Command Prompt and Find. A tray is a folder of shadows (the **Trays** folder in My System); **Trays** on TheBar's Parts page.
- **Find** is an object in My System, so it can be put in trays, the Launcher and folders.
- **A menu bar in folder windows** (Warp 4's folders had one): Folder, Edit, View, Selected, Help. On or off for one folder (its View page) or for all (System, the Folders page), off by default. **Alt** or **F10** shows it for a moment, with the keyboard on it: Left and Right along the titles, Down, Enter or a title's first letter opens one; Left and Right in a menu move to the next; Escape leaves. With the mouse, a click on another title or moving along them switches menus.
- **The Column view** (NeXT's browser; Ctrl+4, View or Open > Column view): the folder's contents in a column, a folder chosen opens in the next column to its right, a file's details in the last. Left and Right move between columns, Up and Down within one, Enter opens; a scroll bar beneath when the columns do not all fit. Changes in any column show at once.
- **Show pictures** (a folder's Icon view page): pictures, PDFs, videos and other files shown as what they hold, in place of their type's icon, wherever a thumbnailer for them is installed. Thumbnails are made in the background (two at a time, each given ten seconds) and kept in the shared cache, `~/.cache/thumbnails`, the one GNOME's Files and other programs use, so each file is done once for all of them. A folder's `.sh_thumbnails` (left by other systems) is read; a folder's `.osd-thumbnails` (lines of `NAME<Tab>PICTURE`) chooses the picture of an object by hand. Neither is listed in the folder.
- **The Gallery view** (Ctrl+5, View or Open > Gallery view): one object shown big, with its title, size and date, and the folder's others as pictures in a strip beneath; Left and Right move along the strip, Enter opens. A folder opened again comes back in the view it was left in.
- **Suspend after idle:** the session follows GNOME's Automatic Suspend (Settings, Power: on mains and on battery, after how long, suspend or hibernate). After that long idle the screen locks and the computer suspends; not while a program keeps the screen on (a video playing, a presentation).
- **Mouse mappings take two-button chords:** for example Edit title on `button1+button2` (one pressed while the other is held), without taking the popup menu's button 2 alone.

## Fixed

- An Alt held into another window (Alt+F4 closing a folder window) no longer counts as an Alt press there.
- Events meant for every window (a file in use, the Trash Can changed) were sometimes taken by the Launcher's folder.
- The Trash Can keeps its corner on a new desktop when the stored positions are read (an object made at once came after an empty spot).
- In a container (toolbox, distrobox) whose root filesystem is not a disk, objects can be made.

## Known issues

- A menu bar's item cannot yet be chosen by pressing a title, dragging into its menu and releasing; click the title, then the item.
- TheBar's tray icons cannot yet be dragged off the bar or along it.
- An object's picture cannot yet be chosen by hand from the desktop; write the folder's `.osd-thumbnails` instead.
- See [roadmap.md](roadmap.md) and [open-issues.md](open-issues.md).

## Next

0.7: themes driven by the scheme objects for other programs (GTK, icons, cursors, Qt), and more than one screen. See [roadmap.md](roadmap.md).
