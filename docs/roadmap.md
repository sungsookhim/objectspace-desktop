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

## Version 0.3 (released 2026-11-22)

Tagged `v0.3`; the RPMs are attached to its GitHub release. All eleven steps of [v0.3-plan.md](v0.3-plan.md) are done, with the owner's changes along the way; Work Area folders and icon packages moved to 0.4. What it holds is in the [release notes](release-notes-0.3.md).

## Version 0.4 (released as 0.4.1, 2026-11-24)

The object-property refactor (R0-R6) and the eleven steps of [v0.4-plan.md](v0.4-plan.md) are done, with the owner's changes along the way (a folder's own title bar colour; folder windows joining a work area's group); 0.4.0 was built but not released (the owner's): 0.4.1 adds WPS Terminal and the `wps` command ([cli-design.md](cli-design.md), steps 12 to 14) and is the release: tagged `v0.4.1`, the RPMs (with `wps-devel`) attached to its GitHub release. What it holds is in the [release notes](release-notes-0.4.1.md).

**Next, 0.5 (the owner's, 2026-11-24):** getting ready to be public: a name of its own in place of IBM's (WPS, Workplace Shell, the WarpCenter, the LaunchPad, the WP* classes), the repository's licenses and documents, and a welcome for contributors; see **Proposed order**. The look of other programs moves to 0.6.

## Open and deferred work

Everything still open after 0.4, by area; the version each goes into is in **Proposed order** below. Done items moved to the release notes. **D** marks something the owner deferred; **?** a choice still to make.

### Icons

- **A Preview view** (the owner's, for 0.4 if there was room; not done): pictures (images, `.wpsicon` packages) shown in place of their type's icon, thumbnails made off the UI thread and cached.
- **?** **Hatching an open object whose notebook opens:** now it keeps its open icon, unhatched; the owner's to decide.
- Left from 0.4 step 3: the title bars' icon (a drawn glyph), the LaunchPad drawers' rows (24), desktop entries' icons (their 64 PNG), an object's own open icon kept as the path chosen (not copied).
- An object's icon is a copy: a changed source image does not follow; icons no object uses stay in `wps/icons`; templates copy their icon's PNG.
- No drop on the icon picker to add an image to the library.

### Folder windows

- **Sort by size:** folders and shadows sort by their own stored size (folders first would be tidier).
- **Big folders:** a batch of changes costs time in proportion to the folder; the selection survives a change by name only up to 256 items.

### Objects: safety and types

- **File types from metadata, as OS/2's .TYPE EA:** WPS honours `user.mime_type` when a file has one; a data file's **Type page** sets it. **After 0.4** (the owner's, 2026-11-22).
- **Programs that hand the file to a running instance** (most GNOME apps, Firefox, Ptyxis) show it in use only for a moment, and do not join a work area's group.

### Palettes and fonts

- Other programs' title bars take only the font for all title bars, not a folder's (by design: they are not folders).
- A notebook open while the Dialogs font changes keeps its size until opened again.
- Free rows keep a two-line title's room (rows of one-line titles could be shorter).

### Windows, session and the compositor

- **D** **More than one monitor:** the desktop covers the first screen only, in both modes.
- **GTK 4 / libadwaita programs keep their own title bars** (decided with the above: their header bars hold their buttons and menus).
- **D** **Sharp X11 programs on scaled screens** (the compositor's own scaling of X11 windows).
- **D** **WPS over RDP:** Mutter's RemoteDesktop and ScreenCast interfaces, or a headless compositor served by wayvnc.
- **Suspend after idle** (GNOME's power settings) is not followed.
- **Inside GNOME:** float on top and Super for the WPS menu are the WPS session's only; without the extension the desktop is an ordinary window; a second `wps-shell` does not hand over to the first; Mutter holds frame callbacks for a scroll layer that covers a surface.

### Look of other programs (not designed)

- **Contents following the WPS scheme:** controls, menus, scroll bars, icons, cursors, fonts and colours: a GTK 3 theme, a Qt style, a WPS icon and cursor theme, all driven by the scheme objects.
- **libadwaita (GTK 4) programs keep their own look** (decided, the owner's, 2026-11-22): no stylesheet over them; their skin is honoured. **An option:** the scheme's colours (accent, light or dark) through libadwaita's supported settings, a Scheme Palette choice, off by default.

### Object core (deferred in its design)

- **Network file systems (NFS, SMB):** extended attributes vary, remote changes are not reported; the Network object.
- **Undo for jobs;** disk full and quota errors during jobs.
- **Central objects:** deduplication against local `.wpobj` files; cleanup after their folder is deleted outside WPS; becoming `.wpobj` files when moved into a folder that keeps files.
- **Find:** titles, classes and settings are not searched; nothing is indexed.
- **Scale:** a per-folder index beyond about a million entries; the watch helper with many thousands of folders.
- **Opt-in shared layouts** for team folders.
- **Plug-ins:** calls between objects inside a plug-in; classes for abstract objects; one host for several packages; a real third-party plug-in.
- **Editors not yet tested** for identity across saves: Kate, VS Code.

### Input methods and accessibility

- The candidate window (e.g. Hanja) is not placed at the text; text being composed is dropped when focus moves; no bidirectional text; no on-screen keyboard; not tried with a real CJK engine headless.
- Screen readers follow the focus only (no exposed contents, no Action interface); not tried with Orca itself. Alt+Space inside GNOME is GNOME's window menu (WPS's: Shift+Esc).

### Tests and quality

- **The visual check** (`make visual-check`) differs from run to run in where the first start's desktop icons land (found in 0.4 step 8): the first start's placement races.
- **Unchecked or flaky:** M4 scenario 38 (a job resumed after logout) timed out once; the two-window drag self-test was not rerun; one headless run in ten once missed an X11 window; under headless GNOME the real pointer can hide a LaunchPad tooltip (tested in the WPS session instead).
- **Not automated:** the session from the login screen (by hand); fingerprint and smart-card logins through PAM (not tried).
- **Speed:** Open With's list and New's templates are read when the menu opens (on wpsd's loop and in the window, a few files each): fine so far, worth watching.

### Packaging

- A COPR repository, so `dnf upgrade` finds new versions (skipped so far, the owner's choice); publishing beyond Fedora.

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
3. **0.4 Icons, folders and objects** (WPS's own; one new build dependency, librsvg). **Released 2026-11-23**, built on the object-property base ([object-properties-design.md](object-properties-design.md)) as [v0.4-plan.md](v0.4-plan.md) planned it. Grouped so each step touches one part of the code:
   1. **Icons** (M–L; `icons.c`, wpsd's icon fields, the Icon page):
      - **A package per icon:** `NAME.wpsicon`, a zip holding PNGs named by their size (`SIZE.png`, square, any size) and optionally `icon.svg`; no manifest. **The standard sizes are 256 (the master), 128, 96, 64 and 32** (the owner's, 2026-11-22): WPS's own packages always hold all five, and `wpsctl icon pack` makes any of them missing from the largest picture given. **Any other size a package holds (16, 40, 48, 80, ...) is used too**, as its exact match. A single picture still works (a package of one); `.ico` and `.icns` are unpacked into theirs. `wpsctl icon pack DIR OUT.wpsicon`; Choose... takes packages.
      - **Choosing a size**, in device pixels (a 16 px menu icon at scale 2 wants 32), best first:
        1. the exact size;
        2. the SVG;
        3. a size that is a whole multiple of it (2x, 3x, ...; the smallest such), averaged down n x n - clean, no uneven steps (48 from 96, 16 from 32);
        4. the nearest larger, scaled down smoothly (40 from 64);
        5. the nearest smaller, scaled up: by a whole factor with nearest-neighbour (pixel art stays sharp: 64 from 32), else smoothly.
        Never a fractional nearest-neighbour scale (uneven pixels).
      - **SVG drawn by librsvg** in the process, sharp at any size.
      - **The tool chain (done 2026-11-22):** `gui/shell/wpsicon.c` reads and writes packages and picks a size by the rules above (the SVG waits for librsvg); `wpsctl icon pack INPUT... OUT.wpsicon` (PNGs, an SVG or folders of them; the standard sizes added), `wpsctl icon list PACKAGE`, `wpsctl icon render PACKAGE SIZE OUT.png`; `tools/icons/make_theme.py` packs the isometric set into a theme per variant; `make icons` builds the themes into `build/icon-themes` (`wps-flat`, `wps-outline`, and `classic` from 0.2's pictures by `wpsctl icon pack`), `make install` and the RPM put them in `/usr/share/wps/icon-themes/THEME/` (`theme.ini` and `NAME.wpsicon`); `tests/test_wpsicon.c`.
      - **Icon themes, a System choice** (the owner's, 2026-11-22): **Icon theme** on the System object: WPS (flat) - the default -, WPS (outline), Classic, and themes added to `~/.local/share/wps/icon-themes`. Applying one changes every object kind's and WPS's own icons at once; an object's own icon (its Icon page) stays.
      - **Sizes are a starting point** (the owner's): the default icon size will be adjusted later; the themes hold 256, 128, 96, 64, 48, 40, 32 and 16, each drawn on its own.
      - **WPS's own icons become packages of the isometric set** drawn by `tools/icons/make_iso.py` (2026-11-22: BeOS / Warp 4 style; sets A System Setup objects, B special folders (closed and open), C data files, D the basic kinds, E WPS's own icons redrawn; outline and flat variants). Every size is drawn on its own from the same shapes, not scaled from the master: the five standard sizes by default (`--sizes` adds others); sizes 48 and under get the small design (a bigger identifying feature, fewer details). The generator writes `.wpsicon` packages (its per-size PNGs; no SVG needed) into `data/icons`, and the RPM installs them; the flat variant is the default, the outline one a choice (the owner's).
      - **The open icon** (OS/2's animation icon): a second icon, shown while the object is open; the Icon page's second field, "Open".
      - **Icon size** for icon views and the desktop (System: Small 32/16/16, Default 64/32/16, Large 96/48/24): a package's exact size first; otherwise the rules (with the standard sizes, 16 from 32, 48 and 24 from 96).
   2. **Folder layout and windows** (M; `layout.c`, the container, the folder's settings):
      - **OS/2's free icon layout** (cells as wide as their titles), grid or free per folder;
      - **folder windows remember** their position, size and view;
      - the search field's place (top, bottom, floating); the resize pointer on column edges;
      - positions from 0.2 that overlap long titles: arranged once, if the folder has never been arranged in 0.3;
      - **the desktop's status line:** its messages shown for a moment at the bottom of the desktop.
   3. **Objects: safety and types** (M; wpsd's delete and rename, WPDataFile, WPProgram):
      - **Prevent delete** (General page; WPS's own objects on);
      - **confirm an extension change on rename;**
      - (`user.mime_type` and the Type page: after 0.4, the owner's);
      - **a Program page for executable files** (type, close on exit, working directory, as for programs);
      - **a shadow's menu is its original's** (Open With on a shadow of a file);
      - programs started by wpsd get an **activation token** (they come to the front).
   4. **The Colour Palette and fonts everywhere** (M; the palettes, `ui_notebook`, `ui_dialog`):
      - **the Colour Palette** (OS/2's): colours dragged onto backgrounds (a folder's, the desktop's), a window's title bar and frame, a notebook's colour field;
      - **the Background page's polish:** the swatch shows the colour in use; a preview of the picture;
      - **the Font Palette's fonts in every WPS window** (notebooks, dialogs, Find, the window list: a font dropped there sets "dialogs");
      - font sizes follow GNOME's text scaling; Edit font gets a style list.
   5. **Work Area folders** (M–L; the compositor, wpsd's tracking, the session): a folder set as a work area groups the programs opened from it; opening, minimizing and closing the folder do the same to their windows; restored at login. Builds on the in-use tracking, the compositor's per-program window control and the restored windows of 0.2; inside GNOME through the extension.
   6. **Fixes and tests carried** (S): the flaky and unrerun tests above; Open With's list read off wpsd's loop; the LaunchPad drawer cascade opening outward on the left; the release (RPMs, notes, tag).
4. **0.5 Ready to be public** (the owner's, 2026-11-24: the repository opens to the public when 0.5 is done; the name **Objectspace**, the repository `objectspace-desktop`, the commands `osd`, TheBar and Launcher: [v0.5-names.md](v0.5-names.md); the steps: [v0.5-plan.md](v0.5-plan.md)). No new features; the project made safe and welcoming to publish:
   1. **A name of its own** (L; the whole tree, mostly mechanical). The code is written from scratch, but some names are IBM's products' (trademarks) or mirror IBM's interfaces; they are replaced, OS/2 kept only as a description of where the ideas come from:
      - **The product:** WPS and "Workplace Shell" become the new name, everywhere a user or another program sees it: the commands (`wps`, `wpsd`, `wpsctl`, `wps-shell`, `wps-terminal`, `wps-compositor`, `wps-session`, `wps-watchd`, `wps-plughost`), the packages and the session's name on the login screen, the app IDs and D-Bus names (`org.wps.*`), the GNOME extension (`wps@wps-linux`), the folders and files (`~/.config/wps`, `~/.local/share/wps`, `.wpsicon`, `.wpobj`), the extended attributes (`user.wps.*`), the environment (`WPS_*`), the repository (`wps-linux`). Today about 290 files, 4,700 lines.
      - **IBM's product names for parts:** the WarpCenter and the LaunchPad (OS/2's; also another company's "Launchpad") get names of their own (61 and 54 files). Generic words stay (Font Palette, Colour Palette, Scheme Palette, Work Area, Templates, Shadows, System Setup).
      - **IBM's class names:** WPObject, WPFolder, WPDataFile... (the names of OS/2's SOM classes; 144 files) and the runtime's `wpo_` (78 files) take the new prefix; the method names (`wpQueryTitle`, ...) too.
      - **What the user reads:** menus, notebooks, dialogs and messages say no "OS/2" or "Warp"; the release notes and README describe the project as inspired by OS/2's Workplace Shell, with a trademark notice (OS/2 and Workplace Shell are IBM's; no affiliation).
      - **Code comments and the design documents** keep "as OS/2 did" where it explains why something is so (a description, not a name), trimmed where it says nothing.
      - **A clean break** (the owner's, 2026-11-24): nothing carried over from 0.4.1. Every name to change, by importance, with the candidates: [v0.5-names.md](v0.5-names.md).
   2. **What is in the repository** (S-M):
      - Every source file says its license (an SPDX line, MIT); third parties' notices (the Wayland protocol files in `proto/`; the libraries linked, none copied in).
      - **The pictures:** every icon checked to be drawn here (the isometric set by `tools/icons/make_iso.py`; the Classic set, 0.2's pictures; the sounds, the theme's or none).
      - **Internal documents kept as the project's history** (the owner's): the discussion notes, the AI review, the plans; the README and `docs/` say that up to 0.4.1 the project was private and named WPS, and that 0.5, the first public version, took its own name for that reason.
      - **A new repository under the new name, with the old history in it** (the owner's); the old one stays private. Nothing private was found in the history (no host names, addresses or keys; the owner's GitHub name and e-mail only, as now).
   3. **For people who come to it** (M):
      - A README for strangers: what it is and is not, screenshots, installing, building from source, the status; a CONTRIBUTING guide (how to build, test, the code's style, the design documents' role), a code of conduct, a security policy (how to report), issue and pull request templates.
      - **Continuous integration:** GitHub Actions building on Fedora 45 and running the unit tests and the headless suites that need no GPU (`make check`, `gui-test`, the compositor's tests where the runner allows).
      - The authorship note kept (designed by the owner, code written with Claude through Claude Code).
   4. **Checks:** a search for the old names finds none outside the history notes; the RPMs build and install under the new names; an upgrade from 0.4.1 (if moving over is chosen); the suites green under the new names.
5. **0.6 The look of other programs and more screens** (the compositor and themes; was 0.5):
   - **Themes driven by the scheme objects:** a GTK 3 theme, an icon theme (from 0.4's packages) and cursor theme, a Qt style.
   - **libadwaita programs keep their own look and title bars** (the owner's); the scheme's colours offered to them as an option (Scheme Palette, off by default).
   - **More than one monitor** (the desktop on each; the bar on one) (**D** until then).
   - **Sharp X11 programs on scaled screens.**
   - **Suspend after idle.**
   - **A Preview view** (pictures in place of their icons).
6. **0.7 Network and jobs** (the service and the object core; was 0.6):
   - **The Network object** and network file systems (NFS, SMB).
   - **Undo for jobs;** disk full and quota errors.
   - **Central objects:** deduplication, cleanup, becoming `.wpobj` files.
   - **Find** in titles, classes and settings; an index.
   - **File types from `user.mime_type`** and the Type page (after 0.4, the owner's).
   - **RDP** (**D**: Mutter's RemoteDesktop/ScreenCast interfaces, or wayvnc).
7. **1.0 Release:** accessibility beyond focus (exposed contents, the Action interface, tried with Orca); input methods (the candidate window at the text, composing kept across focus, bidirectional text); a first-run tour; the deferred tests; COPR and publishing beyond Fedora; plug-ins proven by a third-party one.
