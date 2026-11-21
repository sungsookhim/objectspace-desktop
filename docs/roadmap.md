# Roadmap

Draft, 2026-11-21. Version 0.2 is designed in [v0.2-design.md](v0.2-design.md) and planned in [v0.2-plan.md](v0.2-plan.md). Version 0.1 is the baseline: everything below is what is open, deferred or not yet designed after it. The owner adds objectives to this draft; the next design and implementation plan follows from it.

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

## Open and deferred work

Each item says where it stands. **D** marks something the owner deferred; **?** a choice still to make.

### Folders and objects (daily use)

- **D** **Details view sorting and columns.** Click a heading to sort (again: reversed); choose the columns globally (System Setup) and per folder. Sorting belongs in wpsd (rows are paged and shared). **?** Kept per folder (OS/2's "Always maintain sort order", also used by Arrange) or per window; OS/2's column set or today's plus Type.
- **D** **OS/2's icon layout.** Every title whole, rows as tall as their tallest title, Arrange spacing to suit. The 0.1 step: two lines, the whole title when selected.
- **A search field in folder windows** that filters the view (today Ctrl+F opens Find on the folder). Needs the folder model to show a subset.
- **Sort by size:** folders and shadows sort by their own stored size, so they mix with files; folders first would be tidier.
- **No WPS Trash object** to browse, restore from or empty; no dropping on a trash or shredder icon.
- **Open With** ("comes in G2" in the menu); types with no default application go through `xdg-open`, untracked.
- **Program settings missing:**
  - Program type: text-mode or not is guessed from the ELF file, and scripts always get a terminal.
  - "Close window on exit".
  - The terminal to use: `WPS_TERMINAL` today.
- **Hidden and lock files** (`.~lock.*#`, `.*.swp`) are listed; **?** a "show hidden files" setting.
- **Templates:**
  - Moving a template by dragging (every drag makes a new object).
  - A "New" cascade listing the templates.
  - Create another asking for a name and folder, as OS/2 did.
  - New data files' titles include the extension.
- **Central objects:**
  - Deduplication against local `.wpobj` files.
  - Cleanup after their folder is deleted outside WPS.
  - Becoming `.wpobj` files when moved into a folder that keeps files.
- **Delete in tree view** deletes only the item with the cursor.
- **Confirmations** are read from `system.conf`, not yet from the System object.
- **Big folders:** a batch of changes costs time in proportion to the folder; the selection survives a change by name only up to 256 items.

### Desktop and session

- **D** **More than one monitor:** the desktop covers the first screen only, in both modes.
- **D** **WPS over RDP.** Remote logins start headless and the compositor needs a seat. Options:
  - Implement Mutter's RemoteDesktop and ScreenCast interfaces, so GNOME Remote Desktop works.
  - A headless compositor served by wayvnc.
- **D** **Sharp X11 programs on scaled screens.** wlroots 0.20 offers no XWayland scaling; it needs the compositor's own scaling of X11 windows.
- **The desktop's own menu** lacks OS/2's Lockup now, Shut down and System setup; **the desktop has no status line**, so its messages are not shown.
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

### Packaging and first run (not started; the RPM is a 0.2 objective)

- **The session runs from the source checkout:**
  - `tools/install-session.sh` points at it.
  - `wpsd.service` runs `build/wpsd`.
- **To make it installable:**
  - Install to `/usr`, and an RPM.
  - A versioned protocol between the GUI and wpsd.
  - Upgrades of the object database.
- **First login:**
  - What a new user sees: the desktop's first contents, the LaunchPad, the sound theme.
  - A short tour or help object; OS/2 had a Tutorial and an Information folder.
- **Build dependencies** are listed in the README but not checked by the build.

## Objectives for 0.2 (the owner's, 2026-11-21)

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
  - **The bar, start menu, clock, meters, keyboard indicator: M.** Our toolkit; the compositor keeps windows clear of it (a work area), as it already places the LaunchPad.
  - **Messages: M.** The WPS session has no notification service at all today: programs' notifications go nowhere. WPS becomes it (org.freedesktop.Notifications): pop-ups near the WarpCenter, and the Messages object keeps them.
  - **Tray: M–L.** Programs' status icons are StatusNotifierItem/AppIndicator (D-Bus), with their menus over dbusmenu. Few Fedora programs use it, so it can come last within 0.2, or in 0.3.
- **Inside GNOME (stage 1):** GNOME has its own top bar, and a program cannot reserve screen space under GNOME. Proposed: the WarpCenter in the WPS session only for 0.2; inside GNOME, GNOME's own bar stands in.
- **?** Whether the WarpCenter and the LaunchPad both show by default (OS/2 Warp 4 had both, the LaunchPad off at first).

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
- **?** Where it is published: a COPR repository (`dnf copr enable ...` then `dnf install wps`) or an RPM file to download.

### Still deferred for 0.2

- **RDP:** after 0.2.
- **Window borders for programs that draw their own** (GTK 4 / libadwaita): deferred.

## Proposed order

1. **0.2 The OS/2 desktop, installable** (the owner's objectives). Suggested sequence, each step usable on its own:
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
   8. **The RPM** (`.spec`, and COPR or a file), tried on a fresh Fedora VM.
2. **0.3 Daily use and OS/2 fidelity:**
   - Details-view sorting and columns; the folder search field; Open With.
   - Program settings: type, close on exit, terminal.
   - The desktop's own menu (Lockup, Shut down, System setup).
   - The icon layout.
   - Templates' "New" cascade and moving templates.
   - The Font Palette; the Sound object's picker; the tray if not in 0.2.
3. **0.4 Look of other programs:** the theming pieces above, and frames for GTK 4 programs.
4. **0.5 Platform:**
   - RDP; more than one monitor; sharp X11 when scaled.
   - The Network object and network file systems.
   - Undo for jobs.
5. **1.0 Release:** accessibility beyond focus, first-run tour, the deferred tests, publishing beyond Fedora.
