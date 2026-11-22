# Roadmap

Draft, 2026-11-21. Version 0.2 is released (tag `v0.2`; [release notes](release-notes-0.2.md)); it was designed in [v0.2-design.md](v0.2-design.md) and planned in [v0.2-plan.md](v0.2-plan.md). Version 0.1 is the baseline: everything below is what is open, deferred or not yet designed after it, with 0.2's objectives marked done. The owner adds objectives to this draft; the next design and implementation plan follows from it.

## Version 0.1 (the baseline)

Tagged `v0.1`. What it holds:

- **Object core (M0–M5).** Persistent objects for files, folders and abstract items with identity that survives moves, copies and edits; change tracking (`wps-watchd` or inotify); persistence with volume sync; shadows, templates, the trash; streaming listings for million-entry folders; jobs with a journal; programs and `.desktop` files with launch confirmation; events, menus and settings as data; sandboxed plug-in hosts; several logged-in users.
- **GUI (G0–G5).**
  - Desktop, folder windows in icon, tree and details views; settings notebooks.
  - Drag and drop: move, copy, shadow, ask; to and from other programs.
  - Templates, shadows, the trash.
  - Scheme, sound and system settings objects, kept in step with GNOME's.
  - LaunchPad with drawers, reordering, and drops on programs; Window List; Find, with dragging and saving results; per-object icons.
  - Accessibility through AT-SPI; input methods, IBus included; type-ahead.
- **Two ways to run.** Inside GNOME (stage 1, with the GNOME Shell extension), or as its own session from the login screen (stage 2: `wps-compositor` on wlroots 0.20):
  - WPS's frames for every program that accepts them: xdg-decoration, GTK 3 through KDE's protocol, X11.
  - XWayland; drag and drop; clipboard and primary selection; data control for clipboard helpers.
  - Lock screen through PAM; screen blanking and idle lock; logind's lock and sleep.
  - Fullscreen; scaled screens, changed live.
  - GNOME's keyboard layouts and IBus sources followed live, with Super+Space to switch.
  - XDG autostart; VirtualBox guest support: software drawing on vmwgfx, its shared clipboard.
- **Tried by hand** on Fedora 45 in a VirtualBox VM, Korean locale: logins between GNOME and WPS both ways, programs (Firefox, Ptyxis, Text Editor, xterm), Hangul input.

## Version 0.2 (released 2026-11-21)

Tagged `v0.2`; the RPMs are attached to its GitHub release. The objectives below (1–6, the RPM, open windows restored at login) are done, and the tray too; COPR was left out (the owner's choice). What it holds is in the [release notes](release-notes-0.2.md).

## Open and deferred work

Each item says where it stands. **D** marks something the owner deferred; **?** a choice still to make.

### Folders and objects (daily use)

- **D** **Details view sorting and columns.** Click a heading to sort (again: reversed); choose the columns globally (System Setup) and per folder. Sorting belongs in wpsd (rows are paged and shared). **?** Kept per folder (OS/2's "Always maintain sort order", also used by Arrange) or per window; OS/2's column set or today's plus Type.
- **D** **OS/2's icon layout.** Every title whole, rows as tall as their tallest title, Arrange spacing to suit. The 0.1 step: two lines, the whole title when selected.
- **A search field in folder windows** that filters the view (today Ctrl+F opens Find on the folder). Needs the folder model to show a subset.
- **Sort by size:** folders and shadows sort by their own stored size, so they mix with files; folders first would be tidier.
- **The owner's to-do (2026-11-22):**
  - **Icon size** for icon views and the desktop, a System choice: 32, 40 or 64 pixels (today 32).
  - **Prevent delete** on an object's General page (OS/2's no-delete style): Delete and drops on the Trash Can refuse it, by accident or not, until it is unchecked. The Trash Can and My System are the examples; WPS's own objects could start with it on.
  - **Lock position:** in 0.3 step 6 (also asked for here).
  - **UI:** the resize pointer over a details view's column edges; the Background page's polish (open-issues).
  - **Where the search field sits** (Ctrl+F): a setting - top (today), bottom, floating.
- **The Trash Can:** done in 0.2 step 3 (browse, restore, empty, drops, shadows trashed and restorable, empty and full icons).
- **Lock icon positions** (owner's to-do, 2026-11-21): OS/2's "Lock position" on an object's menu (and per folder): a locked icon cannot be dragged to another spot (it can still be dragged onto other objects? **?**). Stored with the icon's position.
- **Open With** ("comes in G2" in the menu); types with no default application go through `xdg-open`, untracked.
- **Program settings missing:**
  - Program type: text-mode or not is guessed from the ELF file, and scripts always get a terminal.
  - "Close window on exit".
  - The terminal to use: `WPS_TERMINAL` today.
- **Hidden and lock files** (`.~lock.*#`, `.*.swp`) are listed. **Decided** (0.3): lock files never listed; Show hidden files is one system choice in the System object.
- **Templates:**
  - Moving a template by dragging (every drag makes a new object).
  - A "New" cascade listing the templates.
  - Create another asking for a name and folder, as OS/2 did.
  - New data files' titles include the extension.
- **Central objects:**
  - Deduplication against local `.wpobj` files.
  - Cleanup after their folder is deleted outside WPS.
  - Becoming `.wpobj` files when moved into a folder that keeps files.
- **Delete in tree view:** done in 0.3 step 1 (the whole selection).
- **Confirmations:** read from the System object (checked in 0.3 step 1).
- **Big folders:** a batch of changes costs time in proportion to the folder; the selection survives a change by name only up to 256 items.

### Desktop and session

- **D** **More than one monitor:** the desktop covers the first screen only, in both modes.
- **D** **WPS over RDP.** Remote logins start headless and the compositor needs a seat. Options:
  - Implement Mutter's RemoteDesktop and ScreenCast interfaces, so GNOME Remote Desktop works.
  - A headless compositor served by wayvnc.
- **D** **Sharp X11 programs on scaled screens.** wlroots 0.20 offers no XWayland scaling; it needs the compositor's own scaling of X11 windows.
- **The desktop's own menu** lacks OS/2's Lockup now, Shut down and **System Setup** (the owner's, after 0.2: a cascade with the settings objects, as OS/2's desktop menu had); **the desktop has no status line**, so its messages are not shown.
- **The desktop's background image** (the owner's, after 0.2): a Background page in the desktop's notebook (right click, Properties) to set the image. Today the desktop shows GNOME's background setting.
- **LaunchPad:**
  - No tooltips.
  - Not a settings object yet: orientation, small icons, float on top (OS/2 had a notebook for it).
  - Drawers are read when opened, not followed.
  - No keyboard shortcut to reach it.
- **Input methods:**
  - The candidate window (e.g. Hanja) is not placed at the text.
  - No bidirectional text.
  - Text being composed is dropped when focus moves.
  - No on-screen keyboard.
- **Session settings read at login:** idle and lock times. Suspend after idle (GNOME's power settings) is not done.
- **Fonts:** WPS follows GNOME's interface font; no Font Palette of its own.
- **Sound object:** events are text fields; a list to pick from with Play would be friendlier.
- **Inside GNOME (stage 1):**
  - Without the extension the desktop is an ordinary window.
  - Programs started by wpsd get no activation token.
  - One process per desktop: a second `wps-shell` does not hand over to the first.
  - Mutter holds frame callbacks for a scroll layer that covers a surface.

### Look of other programs (not designed)

- **Contents following the WPS scheme:** controls, menus, scroll bars, icons, cursors, fonts and colours.
  - Likely pieces: a GTK 3 theme, a GTK 4 / libadwaita stylesheet (limited by design), a Qt style, a WPS icon and cursor theme.
  - All of it driven by the scheme objects.
  - **?** How far to go with libadwaita.
- **WPS's frame around GTK 4 / libadwaita programs:** they offer no protocol for it. A per-application setting would hide their title bar and frame them.

### Object core (deferred in its design)

- **Network filesystems (NFS, SMB):** extended attributes vary, and remote changes are not reported.
- **Undo for jobs:** a move, copy or delete taken back.
- **Disk full and quota errors during jobs.**
- **Opt-in shared layouts** for team folders.
- **Scale:**
  - A per-folder index for folders beyond about a million entries.
  - The watch helper registering many thousands of folders (a hash table by handle, subtree registration).
- **Editors not yet tested** for identity across saves: Kate, VS Code.
- **Plug-ins:**
  - Calls between objects inside a plug-in.
  - Plug-in classes for abstract objects.
  - Grouping packages in one host.
  - A real third-party plug-in to prove the API.
- **Find** searches names only: titles, classes and settings are not searched, and nothing is indexed.

### Accessibility

- Screen readers follow the focus only: a window's other contents are not exposed, and there is no Action interface.
- Not tried with Orca itself.
- Alt+Space is GNOME's own window menu inside GNOME; WPS's system menu is on Shift+Esc.

### Tests and quality

- **The session from the login screen** is tested by hand only; automated tests cover the compositor, desktop and lock screen headless.
- **Unchecked or flaky tests:**
  - M4 scenario 38 (a job resumed after logout) timed out once.
  - The two-window drag self-test was not rerun after the latest drag-and-drop changes.
  - One headless run in ten once missed an X11 window.
- **Not tried:** fingerprint and smart-card logins through PAM.
- **Fresh test sessions** create the desktop's shadows in random order, so tests address icons by position with care.

### Packaging and first run (the RPM done in 0.2)

- **Done in 0.2:** install to `/usr`, the RPM (`make rpm`, built in mock), versioned databases and upgrades, the default desktop at the first start.
- **Open:**
  - Manual pages for `wpsctl`, `wps-shell`, `wps-session` (rpmlint asks for them).
  - A COPR repository, so `dnf upgrade` finds new versions (skipped for 0.2).
  - At a new user's first start the desktop can briefly show an extra icon before it settles.
  - A short tour or help object; OS/2 had a Tutorial and an Information folder.
- **Build dependencies** are listed in the README but not checked by the build.

## Objectives for 0.2 (the owner's, 2026-11-21; done)

Version 0.2 is about the desktop OS/2 users knew - the objects on it from the first start - and being installable. Each objective with what it overlaps in the lists above, how doable it is, and what is still to decide. Sizes are relative: **S** small, **M** medium, **L** large.

### 1. Trash can (on the desktop by default)

OS/2's Shredder is not carried over; a trash can is.

- **What:** a Trash Can object on the desktop. Opening it shows what is in the trash, with each item's original folder.
  - Its menu and the folder view's menu have **Empty trash**.
  - Items in it have **Restore**.
  - Dropping objects on it trashes them.
  - Its icon shows empty or full.
- **Overlaps:** replaces "No WPS Trash object" above.
- **Doable: yes, M.**
  - wpsd already moves to and from the freedesktop trash (`service/trash.c`, the same folders GNOME's Trash uses): one Trash Can for both.
  - Empty is a delete job.
  - New: the class, its listing (trash/files with info/*.trashinfo), Restore, and the drop target.

### 2. Computer, Drives and disk objects

- **What:**
  - A **Computer** object (the machine itself).
  - Opening it shows **Drives**, a folder with one object per volume, as the partitions are: internal, external and removable.
  - A **/ (root)** object as well.
  - A drive object opens its file system (mounted on demand), and its menu has Mount, Unmount, Eject and Properties (size, free space, file system, device).
- **Overlaps:** takes "More OS/2 objects: Drives" from 0.3; Network is left for later (objective 7).
- **Doable: yes, M–L.**
  - wpsd already follows mounted volumes by UUID (`service/volumes.c`, from mountinfo).
  - Volumes not mounted, and mounting without root, go through **udisks2** over D-Bus (what GNOME's Files uses; installed on Fedora): Mount, Unmount, Eject, and change signals for drives plugged in.
  - Fixed internal partitions may need polkit's administrator password to mount; udisks asks for it.
- **?** Drives as a live folder of central objects (not stored), or stored objects that remember their place and icon. OS/2 kept them; proposed: central objects, identified by the volume's UUID.

### 3. WarpCenter

A bar along the top or bottom of the screen, as OS/2 Warp 4's WarpCenter.

- **What, from left to right:**
  - **Start menu:** OS/2's WarpCenter "Desktop" menu, the desktop's objects as cascades (folders open into submenus).
  - **Tray and system resources** in the middle: other programs' status icons; CPU, memory, disk and network meters.
  - **Keyboard / input method:** the current source (EN, 한 ...); clicking switches, as Super+Space.
  - **Messages:** a count; clicking opens the **Messages** object, which lists every message (notifications from programs, WPS's own: "Moved to the trash: 2 objects", errors).
  - **Clock** at the right end.
- **Placement:** top or bottom; left or right later.
- **Overlaps:**
  - Replaces "the desktop has no status line": WPS's messages go to Messages.
  - Covers the keyboard indicator the input methods lacked.
  - Lives beside the LaunchPad (both are OS/2's).
- **Doable: yes, in parts; L in all.**
  - **The bar, start menu, clock, meters, keyboard indicator: M.** Our toolkit; the compositor keeps windows clear of it (the usable screen area), as it already places the LaunchPad.
  - **Messages: M.** The WPS session has no notification service at all today: programs' notifications go nowhere. WPS becomes it (org.freedesktop.Notifications): pop-ups near the WarpCenter, and the Messages object keeps them.
  - **Tray: M–L.** Programs' status icons are StatusNotifierItem/AppIndicator (D-Bus), with their menus over dbusmenu. Few Fedora programs use it, so it can come last within 0.2, or in 0.3.
- **Inside GNOME (stage 1):** GNOME has its own top bar, and a program cannot reserve screen space under GNOME. Proposed: the WarpCenter in the WPS session only for 0.2; inside GNOME, GNOME's own bar stands in.
- **Decided by the owner:** the WarpCenter is shown by default and the LaunchPad is not (in the WPS session); the LaunchPad is later updated to share features with the WarpCenter.

### 4. The System folder (on the desktop by default)

Today's "System Setup" becomes OS/2's System folder.

- **What it holds:**
  - **Drives** (2).
  - **Minimized Window Viewer:** an object that opens the minimized windows, today the desktop menu's "Minimized windows".
  - **LaunchPad** and **WarpCenter** objects: their notebooks (orientation, placement, small icons, float on top, what the WarpCenter shows).
  - **System Setup** (today's settings objects).
  - **Startup**, a folder whose objects are opened at login, as OS/2's Startup folder.
- **Overlaps:**
  - Takes "the LaunchPad as a settings object" from 0.3.
  - The Startup folder meets **XDG autostart**, which the WPS session already runs (`~/.config/autostart`).
- **Doable: yes, M.** The folder and moving objects into it are small; the LaunchPad and WarpCenter notebooks come with 3; the Viewer exists as a menu item.
- **?** Startup and XDG autostart. Proposed: the Startup folder shows `~/.config/autostart`'s entries as objects. Dropping a program or a script there makes an entry; the login runs them, as now. Objects that are not programs (a folder, a document) are opened by the desktop at login, as OS/2 did. One place for both.
- **Existing desktops:** a desktop made by 0.1 has a "setup" shadow; the first 0.2 start puts the System folder in its place.

### 5. Programs (on the desktop by default)

- **What:** a **Programs** folder of the installed programs (the desktop entries in `/usr/share/applications`, `~/.local/share/applications`, Flatpak's), grouped into folders by category (Office, Internet, Graphics, ...). Kept up to date as programs are installed and removed.
- **Overlaps:** the WarpCenter's start menu can show it too.
- **Doable: yes, M.** A live folder like Drives: wpsd already reads desktop entries (programs, file types). Categories come from their `Categories=`.
- **?** Whether the user can rearrange it (OS/2's Programs folders were ordinary folders the user owned). Proposed: generated, with user shadows allowed alongside.

### 6. Trash can on the desktop by default

With 1. A desktop made by 0.1 gets it at its first 0.2 start.

### 7. Network object

A to do, after 0.2 (with network file systems below).

### An RPM: installed with `dnf install`

- **What:** an RPM built from the tree. It installs:
  - the programs into `/usr` (`/usr/bin`, `/usr/libexec/wps`);
  - the user service; the session on the login screen; the PAM file for the lock screen; the GNOME Shell extension;
  - the default icons, sounds and schemes.
- **Overlaps:** "Packaging and first run" above moves from 1.0 into 0.2.
- **Doable: yes, M.**
  - Today everything runs from the source checkout: `tools/wps-session`, `wpsd.service` and `install-session.sh` point at `build/`. They move to installed paths, and a `make install` with `DESTDIR`.
  - A `.spec` with the build dependencies, built with `rpmbuild` (or mock).
  - **Upgrades:** the object database and attribute formats get a version, and 0.2 reads 0.1's.
  - **First start:** the default desktop objects (1–6) are made for a new user.
- **Decided:** RPM files attached to the GitHub release; COPR after 0.2.

### Open windows restored at login (added 2026-11-21)

- **What:** as OS/2 did, the windows open when the session ends come back at the next login: WPS's own windows - folder windows (view, size, position), Find windows, settings notebooks. A setting turns it off.
- **Where:** 0.2, plan step 7 (with Startup: both are what happens at login).
- **Doable: yes, M.**
  - The desktop records its windows as they change and reopens them at login.
  - In the WPS session the compositor places them where they were: Wayland does not let programs place their own windows, so one command on the desktop's link.
  - Inside GNOME they reopen, and GNOME places them.
- **Not in 0.2:** other programs' windows (Firefox, Text Editor). Wayland has no settled session-management protocol and most programs do not support one; restarting them belongs with Work Area folders.

### Still deferred for 0.2

- **RDP:** after 0.2.
- **Window borders for programs that draw their own** (GTK 4 / libadwaita): deferred.

## Proposed order

1. **0.2 The OS/2 desktop, installable** (the owner's objectives). **Released 2026-11-21.** Suggested sequence, each step usable on its own:
   1. **Groundwork:** `make install` and installed paths; versioned database and attributes. This makes every later step installable.
   2. **The System folder;** the default desktop objects made at the first start, and 0.1's desktops brought along.
   3. **Trash Can.**
   4. **Computer and Drives,** with udisks2.
   5. **Programs.**
   6. **WarpCenter:**
      - the bar, start menu, clock and keyboard indicator;
      - Messages, with WPS as the notification service;
      - meters;
      - the tray last (it can move to 0.3).
   7. **The LaunchPad and WarpCenter notebooks; Startup;** the Minimized Window Viewer.
   8. **The RPM** (`.spec`, RPM files on the release), tried on a fresh Fedora VM.
2. **0.3 Daily use and OS/2 fidelity** (designed in [v0.3-design.md](v0.3-design.md), planned in [v0.3-plan.md](v0.3-plan.md)):
   - Details-view sorting and columns; the folder search field; Open With.
   - Program settings: type, close on exit, terminal.
   - The desktop's own menu (Lockup, Shut down, System Setup with the settings objects); a Background page for the desktop's image (both the owner's).
   - The icon layout; locking icon positions.
   - Templates' "New" cascade and moving templates.
   - The Font Palette; the Sound object's picker; the tray if not in 0.2.
   - **The Colour Palette** (OS/2's; the owner's, 2026-11-22, for later): colours dragged onto a folder's or the desktop's background, a window's parts or a notebook's colour field, as fonts are from the Font Palette. The Background page has a click palette meanwhile.
   - The LaunchPad updated to share features with the WarpCenter (its parts, settings and code), with tooltips.
3. **0.4 Look of other programs and Work Areas:** the theming pieces above, and frames for GTK 4 programs.
   - **Icons in several sizes** (the owner's spec, 2026-11-21; analysed and agreed for 0.4):
     - **A package per icon:** `NAME.wpsicon`, a zip holding any of `16.png`, `32.png`, `64.png`, `96.png`, `128.png`, `256.png` and `icon.svg` (the names give the sizes; no manifest). 256 is the master when there is no SVG. A single picture still works (a package of one); `.ico` and `.icns` are unpacked into theirs. `wpsctl icon pack DIR OUT.wpsicon`; Choose... takes packages.
     - **Choosing a size**, in device pixels (a 16 px menu icon at scale 2 wants 32): the exact size; else the SVG; else the nearest larger, scaled down; else the nearest smaller, scaled up. (The SVG comes before scaling a larger picture down: a 256 shrunk to 22 blurs.)
     - **SVG drawn by librsvg** in the process (a new build dependency, on every GNOME system), sharp at any size.
     - **WPS's own icons** (computer, drives, trash, programs) become packages made from SVG masters.
     - **The open icon** (OS/2's animation icon; the owner's): a second icon, shown while the object is open (a folder with a window, a program running). Empty for most objects (the normal one shows, with the in-use emphasis as now); folders and some programs set it. The Icon page gets a second icon field, "Open".
     - **Later:** icons travelling with their files (OS/2 kept them in extended attributes; here a file holds a path).
     - **Size: M–L.**
   - **Work Area folders** (OS/2's; added 2026-11-21; moved from 0.3 by the owner; designed in [v0.3-design.md](v0.3-design.md) section 12). A folder set as a work area treats the programs opened from it as one group, and also restores them at login (programs restarted in their folder). Opening, minimizing and closing the folder do the same to the group's windows.
     - **Builds on:** wpsd's tracking of which programs were started from which object (the in-use emphasis), the compositor's per-program window control (step 6), and open windows restored at login (0.2).
     - **New:** linking a program's process to its windows (the compositor), the folder's setting in its notebook, and the group's state kept at logout.
     - **Inside GNOME:** through the extension.
     - **Size: M–L.**

4. **0.5 Platform:**
   - RDP; more than one monitor; sharp X11 when scaled.
   - The Network object and network file systems.
   - Undo for jobs.
5. **1.0 Release:** accessibility beyond focus, first-run tour, the deferred tests, publishing beyond Fedora.
