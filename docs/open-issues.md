# Open issues

Known problems and gaps found while building and trying G2, as of 2026-11-19. Each says where it showed up and what is planned. Fixed items move to the change log of the design document they belong to.

## Reported by hand (trying the desktop and folder windows)

- **Recalling minimized windows is awkward.** A minimized window disappears, as in OS/2, but getting it back means GNOME's Activities overview (top left), which does not feel like OS/2. OS/2 had the window list (Ctrl+Esc or both mouse buttons on the desktop) and the Minimized Window Viewer. Both are planned for G4 (window list, launch bar, minimized window viewer); consider moving the window list forward. In a GNOME session the window events come through the `wps@wps-linux` extension.
- **The desktop has no status line**, so messages a folder window shows there ("Moved to the trash: 2 objects", errors such as "Cannot delete: ...") are not shown on the desktop. Needs a place: a transient message, or a notification.
- **The desktop's pop-up menu is the folder's own.** OS/2's desktop menu adds Lockup now, Shut down, System setup and the like; not there yet.

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

## Remaining G2 work

- Templates (next).
- Incremental live updates for very large folders (a burst of changes now costs at most two re-reads of the folder).

## Tests

- **M4 scenario 38** (a job paused at logout resumes at the next start) timed out once without a result; not investigated.
- **The two-window drag self-test** (`WPS_SHELL_SELFTEST=dnd`) was not rerun after the latest drag-and-drop changes; the headless GNOME drags (`PROBE_MODE=interact tests/gnome/run.sh`) cover dropping within the desktop and into a folder window.
