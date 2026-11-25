# Objectspace 0.6.0-alpha.1 (unstable)

Released 2026-11-25. **A preview of 0.6, not a finished release**: TheBar's trays and the folder windows' menu bar are done and checked; the Column, Preview and Gallery views and suspend after idle are still to come ([v0.6-plan.md](v0.6-plan.md)). For daily use, [0.5.0](release-notes-0.5.md) is the stable version.

## Install

On Fedora 45, download the RPMs from this release, then:

```
sudo dnf install ./objectspace-0.6.0.alpha.1-*.fc45.x86_64.rpm ./objectspace-gnome-shell-extension-0.6.0.alpha.1-*.fc45.noarch.rpm
```

(GitHub shows the files' `~` as `.`; inside, the version is `0.6.0~alpha.1`, which RPM puts before 0.6.0 and after 0.5.0.)

Over 0.5.0 it is an upgrade: log out and in again afterwards. Your objects and settings are kept; at the first login the desktop gets the new objects (Find in My System, the Trays folder and the first tray).

## What's new since 0.5

- **TheBar's trays** (OS/2 Warp 4's WarpCenter trays): named sets of objects on the bar, one shown at a time, after the system buttons. The tray button shows the current tray; a click goes to the next, its arrow opens the trays' menu (choose, add, rename, delete). A click on an object opens it; its menu has **Remove from tray**; objects dropped on the bar join the tray. The first tray, **Default**, holds My System, Programs, Command Prompt and Find. A tray is a folder of shadows (the **Trays** folder in My System); **Trays** on TheBar's Parts page.
- **Find** is an object in My System, so it can be put in trays, the Launcher and folders.
- **A menu bar in folder windows** (Warp 4's folders had one): Folder, Edit, View, Selected, Help. On or off for one folder (its View page) or for all (My System, System, the Folders page), off by default. **Alt** or **F10** shows it for a moment, with the keyboard on it: Left and Right along the titles, Down, Enter or a title's first letter opens one; Left and Right in a menu move to the next; Escape leaves. With the mouse, a click on another title or moving along them switches menus.
- **Mouse mappings take two-button chords:** for example Edit title on `button1+button2` (one pressed while the other is held), without taking the popup menu's button 2 alone.

## Fixed

- An Alt held into another window (Alt+F4 closing a folder window) no longer counts as an Alt press there.
- Events meant for every window (a file in use, the Trash Can changed) were sometimes taken by the Launcher's folder.
- The Trash Can keeps its corner on a new desktop when the stored positions are read (an object made at once came after an empty spot).
- In a container (toolbox, distrobox) whose root filesystem is not a disk, objects can be made.

## Known issues

- A menu bar's item cannot yet be chosen by pressing a title, dragging into its menu and releasing; click the title, then the item.
- TheBar's tray icons cannot yet be dragged off the bar or along it.
- See [roadmap.md](roadmap.md) and [open-issues.md](open-issues.md).
