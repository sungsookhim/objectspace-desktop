# Open issues

Known problems and gaps found while building and trying G2, as of 2026-11-19. Each says where it showed up and what is planned. Fixed items move to the change log of the design document they belong to.

## Reported by hand (trying the desktop and folder windows)

- **Recalling minimized windows is awkward.** A minimized window disappears, as in OS/2, but getting it back means GNOME's Activities overview (top left), which does not feel like OS/2. OS/2 had the window list (Ctrl+Esc or both mouse buttons on the desktop) and the Minimized Window Viewer. Both are planned for G4 (window list, launch bar, minimized window viewer); consider moving the window list forward. In a GNOME session the window events come through the `wps@wps-linux` extension.
- **The desktop has no status line**, so messages a folder window shows there ("Moved to the trash: 2 objects", errors such as "Cannot delete: ...") are not shown on the desktop. Needs a place: a transient message, or a notification.
- **The desktop's pop-up menu is the folder's own.** OS/2's desktop menu adds Lockup now, Shut down, System setup and the like; not there yet.

- **Other programs keep GNOME's look.** Programs started from WPS work, but look like GNOME, not like WPS. The goal is that they follow WPS's visual cues too. Two parts:
  - *Window frames*: already planned for stage 2 (gui-design.md: the WPS session's compositor draws our frames for every program that accepts server-side decorations; GTK 4 / libadwaita programs draw their own title bars, left as they are by default, with a per-program setting to put our frame around them). In stage 1 (inside GNOME) GNOME draws the frames.
  - *Contents* (controls, menus, scroll bars, icons, cursors, fonts, colours): not designed yet. Likely pieces: a WPS GTK 3 theme; a stylesheet for GTK 4 and libadwaita (which allow only limited theming: colours, accent, some CSS through `~/.config/gtk-4.0/gtk.css`); a Qt style (a Kvantum theme or a Qt style plug-in); a WPS icon theme and cursor theme; fonts and colours through the GNOME settings WPS already shares. All of it driven by the scheme objects (G3), so changing the WPS scheme changes the programs too. To be decided: how far to go with libadwaita programs, which resist theming by design.

## Desktop and GNOME (stage 1)

- **Primary monitor only.** The desktop covers the primary monitor's work area; other monitors show GNOME's own background. One desktop window per monitor (or one spanning them) is needed.
- **Without the extension** the desktop is an ordinary full-screen window: it covers other windows when clicked and is in Alt+Tab.
- **The desktop once ended by itself** about 10 s after starting (exit status 0) in the nested session; the extension restarted it. Not seen since. `try-desktop.sh` sets `WPS_UI_DEBUG_EXIT=1`, so the next occurrence logs why the main loop ended.
- **Toolkit: a scroll layer that covers a whole surface stops its frame callbacks in Mutter** (Mutter holds the callbacks of a surface its subsurface hides). The desktop avoids it by drawing without a scroll layer; the general fix is to take frame timing from the layer when it covers the window.
- **Programs started by wpsd get no activation token**, so GNOME may open them behind the active window or without focus. The shell should pass an xdg-activation token with `open` (as it now passes its display).
- **One process per desktop.** Folder windows opened from the desktop live in the desktop's process; `wps-shell FOLDER` started separately is another process, so "one window per folder and view" does not hold across the two. Later: a second `wps-shell` hands its request to the running one.
- **Self-tests on a real session need their windows visible**: GNOME sends no frame callbacks to windows it does not show (behind a maximized window, or during the overview), and the frame-driven self-tests then stall. The desktop self-test runs in the headless GNOME instead (`SELFTEST=desktop tests/gnome/run.sh`).

## Opening and in-use emphasis

- **Programs that hand the file to an instance already running** (most single-window GNOME apps, Firefox) exit at once, so the file shows as open only for a moment unless the program writes a lock file.
- **Types with no default application** still go through `xdg-open`, untracked.
- **Text-mode or not is guessed** from the ELF dynamic section: programs that load their window library at run time (some Rust and Go programs) get a terminal window they do not need; scripts always get one, even those that only start a GUI program. OS/2 had a per-program "Program type" setting; add it to program settings.
- **A text-mode program's window closes when it ends**, so a script that prints and exits flashes by. OS/2's program setting "Close window on exit" belongs with the above.
- **The terminal is chosen by `WPS_TERMINAL`** (or the first installed); it should be a setting of the System object.
- **Lock files are listed as icons** (`.~lock.NAME#`, `.NAME.swp`, `.#NAME`), since hidden files are listed. A "show hidden files" setting (off by default?) is undecided.
- **Open With** in the menu still says "comes in G2".

## Delete and trash

- **No WPS Trash object** yet to browse, restore from or empty (GNOME's Trash works on the same folders), and no dropping onto a trash or shredder icon.
- **Delete in tree view** deletes only the item with the cursor, not a selection.
- **Confirmations are read from `<config>/system.conf`** (`confirm_delete`, `confirm_folder_delete`) until the System object exists.

## Central objects

- Local `.wpobj` files should take precedence over central objects with the same identity (deduplication).
- Central objects left behind by a folder deleted outside WPS are not cleaned up; belongs in the catch-up scan.
- Moving a central object into a folder that keeps objects as files should turn it into a `.wpobj` file.
- Shadows made before central objects stay as they are (old symlinks are plain links; old `.wpobj` shadows still work).

## Templates (built 2026-11-19)

- **No way to move a template by dragging**: every drag of a template makes a new object (OS/2 moved a template with Shift held, but GNOME does not tell a program which keys are held during a drag). Move templates with cut and paste once that exists, or turn the template mark off first.
- **Create another makes the object beside the original**, its name ready to edit; OS/2 asked for a name and a folder in a dialog.
- **No "New" cascade** in folder menus listing the templates (XWorkplace had one; GNOME Files has "New Document").
- **Titles of new data files include the extension** when made by Create another ("New Data File.txt").

## Big folders (built 2026-11-19)

- Each batch of changes still costs the window time in proportion to the folder (moving rows after a removal, matching icon spots by name); a batch of more than 64 changes reads the folder again instead. Fine at 200,000 entries; a million-entry folder under constant change would want a cheaper structure.
- The selection survives a change by name only when at most 256 items are selected (as with reading the folder again).

## G3 (built 2026-11-19)

- **Sounds need libsndfile-devel** at build time for the sound theme's Ogg files (WAV plays without it), and an audio output (this machine's is a dummy).
- **Window events and settings sync need the desktop process** (the extension starts it): without the desktop, other programs' windows make no sounds and GNOME's settings are only read (the font), not kept in step.
- **Fonts**: WPS follows GNOME's interface font; it has no font setting (OS/2's Font Palette) of its own yet.
- **The Sound object's events are text fields** (a theme sound name or a file path); a list to pick from, with a Play button, would be friendlier.
- **Shadows** are drawn by WPS for its own windows only (GNOME draws none for windows with their own frames); menus have none.

## Remaining G2 work

G2's planned items are all built. What remains are the issues above.

## Tests

- **M4 scenario 38** (a job paused at logout resumes at the next start) timed out once without a result; not investigated.
- **The two-window drag self-test** (`WPS_SHELL_SELFTEST=dnd`) was not rerun after the latest drag-and-drop changes; the headless GNOME drags (`PROBE_MODE=interact tests/gnome/run.sh`) cover dropping within the desktop and into a folder window.

## LaunchPad (G4)

- Buttons keep the order they were added in (the LaunchPad folder's icon positions); no reordering by drag yet (OS/2 allowed it).
- No tooltips: titles are drawn under the icons, cut short.
- Not yet a setting: shown on every desktop start (`WPS_NO_LAUNCHPAD` turns it off); OS/2 had it as an object with its own notebook (orientation, small icons, text, float on top).
- Drawers are read when opened, not followed; a duplicate in a drawer is not refused.

## Find (G4 search)

- Names only: object titles (which can differ from file names), classes and settings are not searched yet; the design asks for all four.
- No search field inside folder windows: Ctrl+F opens a Find window on that folder, which searches as you type. Filtering a folder's own view in place would need the folder model to show a subset (it maps view rows to listing rows one to one).
- Results cannot be dragged out yet, nor saved as a folder of shadows (OS/2's Find made its results a folder).
- Each search walks the file system; nothing is indexed. A home folder of about 13,000 entries (hidden ones skipped) takes about 50 ms; very large trees stop at 2,000 results.

## Input methods and text (G4)

- Not tried with a real CJK input engine: the headless tests run GNOME's IBus with its plain engine (keys pass through text-input as typed), and inject composed and committed text into the toolkit's handler (`ui_text_input_inject`). Worth a check in a real session with e.g. ibus-anthy or ibus-hangul.
- Text is laid out left to right: right-to-left scripts are shaped within a run, but there is no bidirectional reordering of mixed text, and the caret moves by bytes in visual order.
- The input method's text being composed is dropped (not committed) when focus moves to another field.

## Accessibility (G4)

- The LaunchPad gets the keyboard only from a click (GNOME keeps it out of Alt+Tab); there is no shortcut to reach it yet.
- Alt+Space is GNOME's own window menu (mutter's activate-window-menu); WPS's system menu is on Shift+Esc.
- Screen readers follow the focus only: a window's other contents (the rest of a folder's items, labels, the status line) are not exposed, so a reader cannot explore a window, only hear where the keyboard is. Not tried with Orca itself (tested with libatspi, as Orca uses it).
- No Action interface yet (a reader cannot press a button itself), and component extents are in window coordinates (Wayland gives no screen positions).

## Icons (G4 follow-up)

- The self-test (`selftest.sh icons`) hands the dropped image straight to the Properties code; the drop itself (text/uri-list on the Icon page's picture) is not driven by a headless drag yet.
- The icon library is a plain folder: no drop on the picker to add an image to it (open it with "Open library folder" and drop there).
- An object's icon is a copy: changing the source image later does not change it. Icons no object uses any more stay in `wps/icons`.
- Templates copy their icon setting (the same PNG); deleting an object leaves its PNG.

## wps-compositor (G5)

- GTK 4 / libadwaita programs (Ptyxis, Text Editor, ...) keep their own title bars; the per-application setting to put WPS's frame around them is still to come.
- The compositor's frames use the classic scheme: the WPS scheme and font chosen in the desktop are not followed yet (step 4, with the session's settings).
- One headless run in about ten saw an X11 test window never appear (XWayland's first start, slow?); not seen again.
- No scaled (HiDPI) screens, one screen only for the desktop, no fullscreen yet.
- The session has not been started from the login screen in tests (it needs a console): the headless tests cover the compositor, the desktop, the lock screen (with a test password) and the scheme lines; `tools/wps-session` was only checked for its syntax and its reading of GNOME's settings.
- The lock screen needs pam-devel at build time; without it Lockup says so and does nothing. Fingerprint and smart-card logins go through PAM as configured (not tried).
- No screen blanking, idle lock or power management yet; no on-screen keyboard; the keyboard layout is read when the session starts (a change in GNOME's settings needs a new session).
- Over RDP the WPS session shows nothing: GDM starts remote logins headless (no seat), and the compositor needs a seat; GNOME Remote Desktop streams only GNOME's own compositor (through Mutter's RemoteDesktop and ScreenCast D-Bus interfaces). Options: implement those interfaces in wps-compositor (the remote login then works as with GNOME), or a headless compositor served by wayvnc (VNC, by hand). Testing continues on a VirtualBox machine with its own screen.

