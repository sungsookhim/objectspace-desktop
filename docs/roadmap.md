# Roadmap

Draft, 2026-11-21. Version 0.1 is the baseline: everything below is what is open, deferred or not yet designed after it. The owner adds objectives to this draft; the next design and implementation plan follows from it.

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

### Packaging and first run (not started)

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

## Proposed order (to discuss)

A first grouping, before the owner's objectives are added:

1. **0.2 Daily use:**
   - Details-view sorting and columns.
   - The folder search field.
   - Open With.
   - The Trash object.
   - Program settings: type, close on exit, terminal.
   - The desktop's own menu and status line.
2. **0.3 OS/2 fidelity:**
   - The icon layout.
   - The LaunchPad as a settings object, with tooltips.
   - Templates' "New" cascade and moving templates.
   - The Font Palette; the Sound object's picker.
   - More OS/2 objects: Drives, the Minimized Window Viewer as a folder.
3. **0.4 Look of other programs:** the theming pieces above, and frames for GTK 4 programs.
4. **0.5 Platform:**
   - More than one monitor; sharp X11 programs when scaled.
   - Remote desktop.
   - Network filesystems.
   - Undo for jobs.
5. **1.0 Release:**
   - Packaging, upgrades, first run.
   - Accessibility beyond focus.
   - The deferred tests.
