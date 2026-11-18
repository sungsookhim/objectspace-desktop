# WPS for Linux: Object Core Design

Version 0.1 · 2026-11-17 · Exported from the working doc. Review feedback and accepted changes are in [reviews/2026-11-18-gemini-review.md](reviews/2026-11-18-gemini-review.md) and are not yet folded into this text.

## Summary

The object core is a headless C service that makes every file, folder and abstract item on a Linux system a persistent, extensible object in the OS/2 Workplace Shell model. It is built and tested with no GUI. The GUI is designed later, as a client of the API in this document.

The core rests on four decisions:

- **A small SOM-like runtime written in C**, not GObject, so it can also run on X64-DOS.
- **Object state lives on the file** in extended attributes, with SQLite for abstract objects and for filesystems without extended attributes.
- **Object identity is a kernel file handle plus the filesystem UUID**, not a path, so objects survive renames.
- **fanotify tracks changes made outside WPS**, for example from the command line, so shadows and state stay correct.

### Goals

- An object model in WPS style: users can extend classes, objects persist, and there are shadows, templates and settings pages.
- Objects stay correct when files are changed by ordinary Linux tools: `mv`, `cp`, editors, scripts.
- Shadows are visible to every Linux program, not only to WPS.
- Fully testable from the command line before any GUI exists.
- Portable: the runtime and class code build for Linux and for a future X64-DOS backend.

### Non-goals

- Compatibility with OS/2 programs or binary compatibility with SOM.
- Replacing the Linux filesystem, permissions or desktop entry standards.
- Graphics, windows or the toolkit. Those come in the GUI design.
- Multi-user or network-shared object state in version 1.

## Architecture

The core is two processes and a library. `wpsd` is a per-user service that holds every loaded object; `wps-watchd` is a small root helper that only reports filesystem events. Clients never touch object state directly: they call methods through the service, which keeps object identity consistent.

```
  +----------------------+  +----------------------+  +----------------------+
  | GUI (designed later) |  | wpsctl               |  | Scripts              |
  +----------+-----------+  +----------+-----------+  +----------+-----------+
             |      Unix socket, CBOR messages                   |
             v                         v                         v
  +--------------------------------------------------------------------------+
  | wpsd: per-user object service                                            |
  |  +------------------+  +----------------------+  +---------------------+ |
  |  | libwpo runtime   |  | Classes and plug-ins |  | Change handler      | |
  |  +--------+---------+  +----------+-----------+  +----------^----------+ |
  +-----------|-----------------------|-------------------------|------------+
              | file state            | abstract objects,       | events
              v                       v ID map                  |
  +---------------------+  +----------------------+  +----------+----------+
  | Extended attributes |  | objects.db (SQLite)  |  | wps-watchd (root)   |
  | user.wps.* on inode |  | per user, rebuildable|  | ~500 lines          |
  +---------+-----------+  +----------------------+  +----------^----------+
            v                                                   | fanotify
  +--------------------------------------------------------------------------+
  | Linux kernel and filesystems: ext4, btrfs, FAT, network                  |
  +--------------------------------------------------------------------------+
```

The helper is the only part with extra privileges, and it holds no object state. `wpsd` also adds inotify watches directly for open folders, which needs no helper.

| Component | Kind | Role |
| --- | --- | --- |
| `libwpo` | C library | The object runtime; shared with X64-DOS later |
| `wpsd` | Per-user service | Loads classes and plug-ins, owns objects, writes state, answers the API |
| `wps-watchd` | System service | Watches whole filesystems with fanotify and forwards events |
| `wpsctl` | Command-line tool | Calls any method by name; the test client |
| `wpoc` | Build tool | Turns class interface files into C headers and registration code |
| GUI | Future client | Draws folders, menus and settings from the API; out of scope here |

## Object runtime

The runtime, `libwpo`, is a C library of roughly 3,000–5,000 lines. It provides what made SOM different from a C++ class system: classes are objects themselves, methods can be found by name, and the class set can change at runtime. It depends only on libc and a small platform layer, so the same code builds for X64-DOS.

### Model

- **Classes are objects.** Each class is an instance of a metaclass. Class methods such as `wpclsQueryTitle` and `wpclsNew` are ordinary methods of the metaclass, so subclasses can override them.
- **Single inheritance.** A class has one parent. There are no mixins in version 1.
- **Instance data is per class.** Each class owns a private block inside the object. Its offset is fixed when the class is registered, so a parent can grow without recompiling its subclasses.
- **Method tables.** Each class has a table of function pointers. A subclass's table starts as a copy of its parent's, with overrides replaced.

### Dispatch

| Way to call | Use | Cost |
| --- | --- | --- |
| Static: a generated macro looks up a fixed slot in the method table | Built-in and plug-in C classes | One indirect call |
| By name: `wpo_send(obj, "wpQueryTitle", args)` | Scripts, the service API, the GUI | Hash lookup, then cached |
| Parent call: `WPO_PARENT(cls, obj, method)` | An override that extends its parent's behaviour | One indirect call |

Every method has a signature descriptor (argument types and return type), so a call by name can be checked and packed for the service API without hand-written glue.

### Defining classes

- A class is described in a small interface file (`.wpi`): name, parent, instance data, new methods and overrides.
- A generator, `wpoc`, turns it into a C header and registration code, much as SOM's compiler turned IDL into bindings.
- Built-in classes are linked into the service. Plug-in classes are shared libraries (`.so`) loaded from a classes directory at startup; each exports one registration function.

### Class replacement

`wpo_replace_class("WPFolder", "MyFolder")` makes every new request for `WPFolder` create `MyFolder`, which must be a subclass of it. This is the equivalent of OS/2's `WinReplaceObjectClass`. It is how third-party code changes the behaviour of every folder without modifying the core. Objects already loaded keep their class until they are next loaded.

### Binary stability

- Method slots are added only at the end of a class's table and are never reordered.
- Each class carries a version. A plug-in built against an older version still loads: its offsets stay valid because instance data is located per class.
- A plug-in that needs a newer version than the installed one is refused with a clear error, and the rest of the desktop keeps working.

### Lifetime and threads

- Objects are reference-counted. The service keeps a cache of loaded objects and drops idle ones; a dropped object is reloaded on demand from storage.
- Version 1 runs all object methods on one thread, the service's event loop. Disk I/O and scans run in worker threads and post results back to it. This avoids locking inside class code, which plug-in authors would otherwise get wrong.

## Class hierarchy and core methods

Version 1 ships seven classes. File-backed objects descend from `WPFileSystem`; everything that exists only as an object descends from `WPAbstract`.

| Class | Parent | Represents | Stored in |
| --- | --- | --- | --- |
| `WPObject` | — | Root: title, icon, object ID, settings | — |
| `WPFileSystem` | `WPObject` | Anything with a directory entry | Extended attributes on the inode |
| `WPDataFile` | `WPFileSystem` | A regular file; subclassed by type (text, image, …) | Extended attributes |
| `WPFolder` | `WPFileSystem` | A directory, with its view state and icon positions | Extended attributes on the directory |
| `WPAbstract` | `WPObject` | Objects with no file of their own | SQLite |
| `WPProgram` | `WPAbstract` | A program to start, built from a Linux `.desktop` entry | SQLite |
| `WPShadow` | `WPAbstract` | A pointer to another object; see Links | A managed symlink plus SQLite, or SQLite alone |

Data file subclasses are chosen by MIME type (via shared-mime-info), so `image/png` can map to a `WPImageFile` plug-in. Desktop entries for programs are read from the standard XDG directories and kept in sync with them.

### Core methods

Names follow WPS where the meaning matches, which makes XWorkplace's source a usable reference.

| Group | Methods | Purpose |
| --- | --- | --- |
| Lifecycle | `wpInitData`, `wpUnInitData`, `wpclsNew`, `wpFree` | Create, load and destroy an object |
| Persistence | `wpSaveState`, `wpRestoreState`, `wpSaveDeferred` | Write and read the object's state; `wpSaveDeferred` batches writes |
| Identity | `wpQueryTitle`, `wpSetTitle`, `wpQueryIcon`, `wpQueryHandle` | Name, icon and stable object ID |
| Menus | `wpModifyPopupMenu`, `wpMenuItemSelected` | Each class adds its own right-click items and handles them |
| Settings | `wpAddSettingsPages` | Each class adds pages to the Properties notebook |
| Open | `wpOpen`, `wpQueryDefaultView` | Open a view or start the program |
| Move and copy | `wpMoveObject`, `wpCopyObject`, `wpCreateShadow`, `wpCreateFromTemplate` | Operations that keep object identity correct |
| Drag and drop | `wpDragOver`, `wpDrop` | Each target class decides what a drop means |
| Folder contents | `wpPopulate`, `wpQueryContent`, `wpAddToContent`, `wpDeleteFromContent` | Enumerate and track a folder's objects |
| Notification | `wpObjectChanged` | Called when the change tracker reports an outside change |

Menus and settings pages are returned as data descriptions, not widgets. The core never draws; the GUI turns those descriptions into menus and pages.

### Templates

Any object can be marked as a template. Dragging a template, or calling `wpCreateFromTemplate`, creates a new object of the same class with the template's settings copied. For a data file, the template's contents are copied too.

## Object identity

Every object has a permanent **object ID**: a 128-bit random value created once and never reused. Everything that refers to an object (shadows, folder icon positions, the GUI, scripts) stores the object ID, never a path. A separate table maps each ID to wherever the object is now.

### Locating a file-backed object

| Field | Source | Purpose |
| --- | --- | --- |
| Filesystem UUID | `blkid` / the superblock; for btrfs, also the subvolume ID | Which filesystem, stable across reboots and remounts |
| File handle | `name_to_handle_at()` | Which inode on that filesystem. On ext4 and btrfs it includes the inode's generation number, so a reused inode number gets a different handle |
| Cached path | The last path seen | Fast lookup; may be stale |
| ctime and size | `statx()` | Detects changes made while the service was not running |

The object ID is also written to the file's `user.wps.id` extended attribute. If the database and the file disagree, the extended attribute wins, because it travels with the file.

### Resolving an ID

1. Look up the object ID in the database to get the filesystem UUID, handle and cached path.
2. `stat` the cached path and compare its handle. If it matches, done.
3. Otherwise, if the filesystem is mounted, open the file by its handle with `open_by_handle_at()` and read its current path from `/proc/self/fd`. This call needs root, so the privileged helper does it on request. Then update the cached path.
4. If the handle no longer opens, the file has been deleted or replaced. Wait for the change tracker's verdict (see Edge cases) before treating the object as gone.
5. If the filesystem is not mounted, the object is **unavailable**, not deleted. Shadows to it show as unavailable until the volume returns.

### Filesystems without handles

FAT, exFAT and some network filesystems either cannot produce file handles or produce ones that do not survive a remount. Objects there are keyed by volume UUID plus path, and moves are tracked only while the service watches (see Change tracking). This is weaker, and the GUI should say so in the object's settings.

## Persistence

An object's state is stored on the thing it describes wherever possible. That follows OS/2, where the WPS kept state in extended attributes on top of an ordinary filesystem. Files, folders and their state then move together under `mv`, backups and `cp -a`.

### Where each kind of state lives

| State | Store | Key |
| --- | --- | --- |
| A file's or folder's own settings (title, icon, class, template flag) | Extended attributes on its inode | `user.wps.*` |
| A folder's view of its contents (icon positions, sort order, view type) | Extended attributes on the **folder** | Each child's object ID |
| Abstract objects (programs, shadows, future printers and settings objects) | SQLite | Object ID |
| ID → location map (UUID, handle, cached path) | SQLite | Object ID |
| Fallback for filesystems without extended attributes | SQLite | Volume UUID + path |

Folder layout is stored on the folder, not on its children, as OS/2 did with its `.ICONPOS` attribute. A file with several hard links can then appear at a different position in each folder.

### Extended attribute format

- **One attribute per class**: `user.wps.WPObject`, `user.wps.WPFolder`, and so on. Each class's `wpSaveState` writes only its own attribute, so a plug-in class cannot corrupt its parent's data.
- Values are a small tagged binary format (key, type, value) with a version byte. Unknown keys are kept on rewrite, so older code does not delete newer settings.
- `user.wps.id` holds the object ID (see Object identity).
- ext4 limits all of a file's extended attributes to about one filesystem block (normally 4 KB) unless the `ea_inode` feature is on; btrfs limits each value to about 16 KB. A folder whose icon positions exceed the limit spills the excess into SQLite and records that in its attribute.

### The database

- One SQLite file per user: `~/.local/share/wps/objects.db`, in WAL mode.
- Tables: `objects` (ID, class, abstract state), `locations` (ID, UUID, handle, path, ctime, size), `shadows` (shadow ID, target ID, symlink handle), `volumes` (UUID, last generation or scan time).
- The database can be rebuilt. Everything in it except abstract objects can be regenerated by scanning the extended attributes on mounted volumes. Abstract objects are also exported to a plain text file daily, as a backup.

### Write policy

`wpSaveDeferred` marks an object dirty. The service writes dirty objects after 2 seconds of quiet, on shutdown, and before any move or copy of that object. A crash loses at most the last few seconds of layout changes, never object identity.

## Links

A WPS shadow of a file or folder is a **managed symlink**: a real symlink on disk that the service rewrites whenever its target moves. Every Linux program can follow it, unlike OS/2 shadows, which only WPS could see. Symlinks and hard links made by other tools keep their Linux meaning.

| Kind | On disk | When the target moves | When the target is deleted | WPS shows it as |
| --- | --- | --- | --- | --- |
| Shadow of a file or folder | A symlink + a row in `shadows` | The service rewrites the symlink | The service removes the shadow | Shadow (italic title, as in OS/2) |
| Shadow of an abstract object | A row in `shadows` only | Nothing to do: targets are found by ID | The shadow is removed | Shadow |
| Symlink made by the user (`ln -s`) | A symlink only | Left alone; may dangle | Left alone | Link, with a badge; "Track target" turns it into a shadow |
| Hard link (`ln`) | Another name for the same inode | Not affected | The file lives while any name remains | The same object, shown in each folder |

### Managed symlinks

- Linux refuses `user.*` extended attributes on symlinks, so a shadow's own state is stored in SQLite, keyed by the symlink's file handle.
- The symlink holds an **absolute** path, so moving the shadow itself does not break it.
- When a target is renamed, the change tracker reports the new path, and the service rewrites every symlink pointing to that object ID. The rewrite is atomic: write a new symlink beside the old one, then rename it over the old one.
- If the target is on a volume that is not mounted, the symlink is left as it is, and WPS shows the shadow as unavailable.
- Shadows of abstract objects cannot be symlinks, because there is no file to point at.

### Symlinks made by the user

A user-made symlink may deliberately point at a path, such as whatever is mounted at `/media/backup`. WPS therefore never rewrites it unless the user chooses "Track target". That records it in `shadows` and makes it managed from then on.

### Hard links

- All names of a hard-linked file share one inode, so they share one file handle, one set of extended attributes and **one object ID**. That is correct: they are one file.
- Per-folder state (position, sort order) lives on each folder, so each name can sit at a different place.
- When one name is replaced by an atomic save, it gets a new inode and the link silently splits. The tracker detects this (the old inode had more than one link and lost one to a rename) and marks the object with a warning in its settings.

### Safety rules

- Never follow symlinks in tree walks, scans, recursive copies or folder size calculations. Use `O_NOFOLLOW` and `AT_SYMLINK_NOFOLLOW`, and treat a symlink as a leaf object. A symlink to a parent directory would otherwise loop.
- Deleting a shadow deletes the symlink, never the target.
- Removing shadows when a target is deleted applies only to managed symlinks.

## Change tracking

Changes made outside WPS are caught in three layers. fanotify watches whole filesystems while the service runs. inotify refreshes open folder windows and is the fallback when the helper is not installed. A catch-up scan at mount covers whatever happened while nothing was watching. Access time is not used: with the default `relatime` mount option it is updated at most once a day, and it records reads, not moves.

### Layer 1: fanotify, through a privileged helper

- Watching a whole filesystem (`FAN_MARK_FILESYSTEM`) needs `CAP_SYS_ADMIN`. A small helper, `wps-watchd`, runs as a systemd service with only that capability and `CAP_DAC_READ_SEARCH` (for `open_by_handle_at()`).
- It marks every mounted filesystem that the user's home or mounted volumes live on, with `FAN_REPORT_DFID_NAME | FAN_REPORT_FID`.
- Events used: `FAN_RENAME` (kernel 5.17 or later; old and new location in one event), `FAN_CREATE`, `FAN_DELETE`, `FAN_DELETE_SELF`, `FAN_ATTRIB` (extended attribute changes) and `FAN_CLOSE_WRITE`. Close-after-write tells the service when a file's contents may have changed.
- Events carry file handles, not paths. They match the handles stored in `locations` directly, so most events are a single indexed database lookup.
- The helper forwards events to the user's service over a Unix socket, filtered to paths that user can read. It keeps no object state itself and is about 500 lines, so it is easy to audit.
- On queue overflow (`FAN_Q_OVERFLOW`), the service runs a catch-up scan of the affected filesystem.

### Layer 2: inotify for open folders

- Every folder open in the GUI gets an inotify watch. It needs no root, so the folder refreshes at once even without the helper.
- Renames arrive as `IN_MOVED_FROM` and `IN_MOVED_TO`, paired by their cookie. A move out of watched folders looks like a delete; it is confirmed by the next catch-up scan.
- Without the helper, the service also watches every folder it has stored state for, up to a limit (default 50,000 watches, about 50 MB of kernel memory).

### Layer 3: catch-up scan at mount

Run when a volume is mounted, when the service starts, and after an overflow.

| Filesystem | Method | Cost |
| --- | --- | --- |
| btrfs | `btrfs subvolume find-new` from the generation saved at the last clean shutdown | Proportional to the changes |
| ext4, XFS | Walk the tree, skipping directories whose ctime has not changed; compare each file's handle, ctime and size with `locations` | Proportional to the number of directories |
| FAT, exFAT, network | Walk the tree and match by path | Proportional to the number of files |

The scan runs in a worker thread at low I/O priority. Objects it has not yet reached are shown as normal, and corrected when it reaches them.

## Edge cases

Most outside changes need no special handling: `mv` within a filesystem keeps the inode, so the object and its extended attributes move together, and only shadows need updating. The cases below are the ones where the inode changes or disappears.

| Situation | What the kernel reports | What the service does |
| --- | --- | --- |
| `mv` within one filesystem | `FAN_RENAME`, same handle | Update the cached path; rewrite managed symlinks to it |
| Atomic save: an editor writes a temp file and renames it over the original | `FAN_RENAME` onto an existing name; the old handle is gone, a new one takes its name | Same object, new body: keep the object ID, copy the extended attributes onto the new inode if they were lost, update the handle |
| `mv` across filesystems (copy, then delete) | `FAN_CREATE` on one filesystem, `FAN_DELETE` on the other | Pair them if the name and size match within 5 seconds; otherwise a new object, and the old one is deleted |
| `cp` without `-a` | `FAN_CREATE`, a new handle with no `user.wps.id` | A new plain object of the right class |
| `cp -a` or a restore from backup | `FAN_CREATE`, carrying a `user.wps.id` that already exists | Both copies claim one ID: the original keeps it, the copy gets a new ID with its settings kept |
| `rm` | `FAN_DELETE_SELF` once the last link is gone | Remove the object and its managed shadows; keep its row for 7 days so an undo or a restore can reclaim the ID |
| Hard link split by an atomic save | Rename onto a name whose inode had more than one link | Keep both objects; mark the object with a warning |
| Volume unplugged or unmounted | Unmount, no deletes | Mark its objects unavailable; never delete them |
| Volume changed on another machine | Nothing while away | Catch-up scan when it is mounted again |
| A filesystem without extended attributes | Setting the attribute fails with `ENOTSUP` | Store state in SQLite by volume UUID and path |
| Extended attributes stripped (by `tar` without `--xattrs`, rsync without `-X`, some editors) | `FAN_ATTRIB`, or found by the scan | Restore from the database copy if the handle still matches |
| Two programs change one object at once | Events in order on the service's loop | The last write wins; a conflicting GUI edit is shown again from the file |

### Detecting an atomic save

The pattern is: a new file appears in the same directory, then a `FAN_RENAME` moves it onto an existing name whose inode then disappears. When the replaced name had an object, the service treats the new inode as that object. This covers vim, Emacs, most GUI editors and `sed -i`.

It deliberately does not apply when the new file arrives from another directory, because `mv other/report.txt report.txt` replaces one document with a different one. In that case the replaced object is deleted and the moved one keeps its own ID.

## Service API

The GUI, `wpsctl` and scripts all talk to the per-user service `wpsd` through one interface: messages over a Unix socket at `$XDG_RUNTIME_DIR/wps.sock`. The API is generic — call any method by name on any object — so a new class or plug-in method is available to every client without changing the protocol.

### Messages

Length-prefixed messages, encoded in CBOR (compact binary JSON, with small C libraries). A client may have many requests in flight; each carries an ID that its reply echoes.

| Message | Arguments | Returns |
| --- | --- | --- |
| `resolve` | path, or object ID | Object ID, class, title |
| `call` | object ID, method name, arguments | Method's return value, or an error |
| `describe_class` | class name | Parent, methods with signatures, version |
| `query_content` | folder ID, offset, count | Children with ID, class, title, icon and folder position |
| `subscribe` | object IDs, or a folder ID | Subscription ID; change events follow |
| `unsubscribe` | subscription ID | — |

### Events from the service

| Event | Sent when |
| --- | --- |
| `object_changed` | An object's title, icon or settings changed, from WPS or from outside |
| `content_added`, `content_removed` | A subscribed folder gains or loses a child |
| `object_moved` | An object moved between folders; both folders' subscribers get it |
| `object_unavailable`, `object_available` | Its volume was unmounted or mounted again |

### Menus and settings pages as data

`wpModifyPopupMenu` and `wpAddSettingsPages` return descriptions, not widgets:

- A **menu** is a list of items, each with an ID, a label, an optional submenu, and enabled or checked state. Choosing an item sends `call(obj, "wpMenuItemSelected", item_id)`.
- A **settings page** is a list of fields: label, type (text, number, checkbox, choice, file, icon, colour), current value, and the property it is bound to. Saving the page sends the changed values back through `call`.

The GUI needs no knowledge of a class to show its menus and settings, so a plug-in class gets a working Properties notebook without any GUI code. A class can still provide a custom page later, as a GUI plug-in.

### Permissions

The socket is readable only by its user. The service acts with that user's permissions, so a file the user cannot change cannot be changed through WPS either. Only the helper `wps-watchd` runs with extra capabilities, and it accepts no commands except handle-to-path lookups for files the asking user can read.

## Testing

The core is finished when every scenario below passes from the command line, with no GUI. `wpsctl` is the test client: it speaks the same API the GUI will use, so passing tests mean the GUI starts on a working base.

### Test setup

- Each test run creates fresh loopback filesystems — one ext4, one btrfs, one FAT — and a throwaway home directory, as X64-DOS's `make test` does with its disk images.
- `wpsd` and `wps-watchd` run against them. Tests then act from outside with ordinary tools (`mv`, `cp`, `rm`, `vim -c wq`, `sed -i`, `tar`) and check the results through `wpsctl`.
- Runs in a VM or container with `CAP_SYS_ADMIN`, so it works in CI.

### Scenarios

| # | Scenario | Pass when |
| --- | --- | --- |
| 1 | Set a file's title; restart the service | Title is restored from extended attributes |
| 2 | Delete the database; restart | Everything except abstract objects is rebuilt by scanning |
| 3 | Create a shadow; `mv` the target to another folder | The symlink points to the new path; `cat` through it works |
| 4 | Same, with the service stopped during the move | Fixed by the catch-up scan at the next start |
| 5 | Edit the target with `vim` and with `sed -i` | Same object ID; settings kept; the shadow still works |
| 6 | `mv` the target from ext4 to btrfs | The shadow follows; the object ID is kept |
| 7 | `rm` the target | The managed shadow is removed; a user-made symlink is left |
| 8 | `cp -a` a file | Both exist, with different object IDs |
| 9 | Hard-link a file into two folders; set positions in each | One object ID; a separate position in each folder |
| 10 | Atomic save through one hard link | The split is detected and the object is marked |
| 11 | Unmount a volume with shadows pointing into it; mount again | Shadows show unavailable, then work again |
| 12 | A symlink loop (`ln -s .. loop`) inside a scanned tree | The scan finishes; no loop is followed |
| 13 | Flood 100,000 renames to force a queue overflow | The rescan runs; every object ends correct |
| 14 | Load a plug-in class and replace `WPFolder` with it | New folders get the plug-in's menu item |
| 15 | Load a plug-in built against an older class version | It loads and works |
| 16 | Run without `wps-watchd` | Open folders refresh via inotify; scenario 4 still passes |

### Other testing

- Unit tests for the runtime: dispatch, parent calls, class replacement, instance data offsets.
- A fuzzer for the extended attribute format and the socket protocol, since both read data from outside.
- A timing benchmark: populate a folder of 10,000 files and report the time, so slowdowns are caught early.

## Milestones

The core takes about 6–7 weeks of focused work to reach M3 and about 2.5–3 months to reach M5. Durations are estimates for one developer. Each milestone ends with the named test scenarios passing.

1. **M0: Tracking prototype (3–5 days).** A standalone C program using `FAN_RENAME` and `name_to_handle_at()` that prints old path, new path and handle while `mv` and `vim` run in another terminal. Confirms the kernel behaviour this design depends on before anything is built on it.
2. **M1: Runtime (2 weeks).** `libwpo`, the `wpoc` generator, the seven classes as empty shells, plug-in loading and class replacement. Unit tests and scenarios 14–15 pass.
3. **M2: Persistence and identity (1–2 weeks).** Extended attributes, the database, object IDs, `wpsd` and `wpsctl`. Scenarios 1, 2, 8 and 9 pass.
4. **M3: Change tracking (2 weeks).** `wps-watchd`, inotify fallback, atomic save detection, catch-up scan. Scenarios 4–7, 10, 12, 13 and 16 pass.
5. **M4: Shadows and volumes (1–2 weeks).** Managed symlinks, unavailable volumes, cross-filesystem moves. Scenarios 3, 6 and 11 pass. All 16 scenarios now pass.
6. **M5: API complete (2–3 weeks).** Subscriptions, events, menus and settings pages as data, `.desktop` programs, MIME type classes, templates. GUI design starts from here.

## Open questions

- [ ] Should the class interface files (`.wpi`) be a custom format, or a subset of an existing IDL to reuse its tools?
- [ ] One database per user, or one per volume stored on the volume, so a USB disk carries its abstract objects with it?
- [ ] Is a 5-second window right for pairing a cross-filesystem move, given that large copies take longer than that?
- [ ] Should `wps-watchd` be required, or optional with a clearly weaker mode? Distributions may hesitate to ship a root service.
- [ ] How should objects inside the trash behave: kept with their IDs until emptied, or treated as deleted?
- [ ] Is version 1 single-threaded dispatch fast enough for folders of 100,000 files? M2's benchmark should decide.
- [ ] Which parts of the runtime need a platform layer for X64-DOS, and should it be defined now or after M5?
