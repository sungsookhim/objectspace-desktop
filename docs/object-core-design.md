# WPS for Linux: Object Core Design

Version 0.17 · 2026-11-18 · Changes from earlier versions are listed in the [change log](#change-log) at the end.

## Summary

The object core is a headless C service that makes every file, folder and abstract item on a Linux system a persistent, extensible object in the OS/2 Workplace Shell model. It is built and tested with no GUI. The GUI is designed later, as a client of the API in this document.

The core rests on five decisions:

- **A small SOM-like runtime written in C**, not GObject, so it can also run on X64-DOS.
- **Object state lives in the filesystem**: settings in extended attributes, abstract objects as files in their folders, and SQLite only for per-user state and filesystems without extended attributes. This is also what lets several logged-in users share objects safely.
- **Object identity is a kernel file handle plus the filesystem UUID**, not a path, so objects survive renames. IDs are assigned lazily, only to files WPS stores something about.
- **fanotify tracks changes made outside WPS**, for example from the command line, so shadows and state stay correct.
- **Nothing waits on anything it does not own.** No client, plug-in, user, large folder or bulk operation can stall the service for anyone else. This is the lesson of OS/2's single input queue, applied from the start.

### Goals

- An object model in WPS style: users can extend classes, objects persist, and there are shadows, templates and settings pages.
- Objects stay correct when files are changed by ordinary Linux tools: `mv`, `cp`, editors, scripts.
- Shadows are visible to every Linux program, not only to WPS.
- Correct under simultaneous access from many processes and many users.
- Responsive at any scale: a folder of a million files, or a drag of a million objects, never blocks other work.
- Fully testable from the command line before any GUI exists.
- Portable: the runtime and class code build for Linux and for a future X64-DOS backend.

### Non-goals

- Compatibility with OS/2 programs or binary compatibility with SOM.
- Replacing the Linux filesystem, permissions or desktop entry standards.
- Graphics, windows or the toolkit. Those come in the GUI design, which inherits the responsiveness rules below.
- Network-shared object state across machines in version 1.

## Design principles

These rules apply to every section below. A change that breaks one needs an explicit decision in this document.

1. **Bounded work on the main loop.** The service's main loop only does work with a known small cost. Anything that touches the disk, the network, a plug-in or an unknown number of items runs elsewhere and reports back.
2. **Cost follows what is on screen, not what is on disk.** Showing a folder costs the visible page, not the folder's size. Starting a drag costs the same for one object or a million.
3. **Isolation between parties.** A slow client, a hung plug-in or another user's activity can delay only itself.
4. **The kernel arbitrates access.** WPS never grants access the filesystem would refuse, and never assumes it is the only writer.
5. **Lazy identity.** Files become objects only when WPS has something to remember about them.
6. **The filesystem is the only shared state.** Services of different users never talk to each other. Anything more than one user must see lives in the filesystem, under ordinary permissions.

## Architecture

The core is two services, a plug-in host and a library. `wpsd` is a per-user service that owns objects. `wps-watchd` is a small system helper that only reports filesystem events. Third-party classes run in `wps-plughost` processes, so a faulty plug-in cannot stall or crash `wpsd`. Clients never touch object state directly: they call methods through the service.

```
  +----------------------+  +----------------------+  +----------------------+
  | GUI (designed later) |  | wpsctl               |  | Scripts              |
  +----------+-----------+  +----------+-----------+  +----------+-----------+
             |      Unix socket, CBOR messages                   |
             v                         v                         v
  +--------------------------------------------------------------------------+
  | wpsd: per-user object service                                            |
  |  +------------------+  +----------------------+  +---------------------+ |
  |  | Main loop        |  | Job manager          |  | Change handler      | |
  |  | libwpo runtime,  |  | worker pool: folder  |  | matches handles to  | |
  |  | built-in classes |  | listing, copy, move, |  | objects, rewrites   | |
  |  | bounded work only|  | scans, metadata      |  | managed symlinks    | |
  |  +--------+---------+  +----------+-----------+  +----------^----------+ |
  +-----------|-----------------------|-------------------------|------------+
     |        | same protocol         | file state, layout      | events
     v        |                       v                         |
  +-----------+----------+  +----------------------+  +---------+-----------+
  | wps-plughost         |  | Extended attributes  |  | wps-watchd (root)   |
  | third-party classes, |  | + objects.db (SQLite)|  | fanotify, per-user  |
  | killable, restartable|  |   per user           |  | event filtering     |
  +----------------------+  +----------+-----------+  +----------^----------+
                                       v                         | fanotify
  +--------------------------------------------------------------------------+
  | Linux kernel and filesystems: ext4, btrfs, XFS, FAT, network             |
  +--------------------------------------------------------------------------+
```

| Component | Kind | Role |
| --- | --- | --- |
| `libwpo` | C library | The object runtime; shared with X64-DOS later |
| `wpsd` | Per-user service | Owns objects, runs the main loop and job manager, writes state, answers the API |
| `wps-plughost` | Per-user process, one per plug-in package | Runs third-party classes; killed and restarted if it misbehaves |
| `wps-watchd` | System service | Watches whole filesystems with fanotify and forwards each user only the events they may see |
| `wpsctl` | Command-line tool | Calls any method by name; the test client |
| `wpoc` | Build tool | Turns class interface files into C headers and registration code |
| GUI | Future client | Draws folders, menus and settings from the API; out of scope here |

## Responsiveness: avoiding a single queue

OS/2's Presentation Manager delivered all keyboard and mouse input through one system queue. An application that stopped reading its messages froze input for the whole desktop. The Workplace Shell ran every folder and class inside one process, so a slow network folder or a faulty WPS class hung the desktop. This design makes those failures impossible by construction rather than by care.

### Rules for the service

- **Main loop budget.** Each request handled on the main loop should finish in under 10 ms. A watchdog logs any slice over 50 ms with the method and class named, and the test suite fails on it.
- **Asynchronous methods.** A method that needs I/O returns a pending result. Its I/O runs on the job manager's workers, and the result is delivered to the caller when ready. Built-in classes follow this rule everywhere. The runtime provides `wpo_defer(work_fn, done_fn)` so class authors never block the loop to wait.
- **No locks held across I/O.** In-memory locks protect only in-memory data and are never held while waiting for the disk or another process.
- **Separate queues with fair scheduling.** The loop serves three queues: interactive requests from clients, job results, and filesystem events. Each loop turn takes a bounded share from each, with interactive requests first. A flood of events from a build cannot delay a click.
- **Per-client isolation.** Requests from one client never wait behind another client's requests, except where both touch the same object (see Concurrency).
- **Overload sheds work, never stalls.** If the event stream overflows, the service schedules a rescan job and carries on. If a client stops reading its replies, its outgoing buffer is capped and the client is disconnected; nobody else waits.

### Plug-in isolation

- Third-party classes run in `wps-plughost`, one process per plug-in package, using the same message protocol as clients. `wpsd` calls their methods asynchronously with a deadline.
- A call that misses its deadline (default 2 s for interactive methods such as menus) gets the parent class's result instead. Repeated misses mark the plug-in as unresponsive, and its host is restarted.
- If a host crashes, objects of its classes fall back to the nearest built-in parent class (for example `WPDataFile`) until it is back. Files and identity are never affected.
- Class replacement (`wpo_replace_class`) works across the process boundary: the replacing class's methods run in its host; inherited methods run in `wpsd`.
- Built-in classes stay in-process for speed. No third-party class is ever loaded into `wpsd` (decided 2026-11-18).
- Hosts are sandboxed (decided 2026-11-18): no network, `no_new_privs`, and a Landlock ruleset allowing only the plug-in's own files and its data folder. Anything else on disk goes through the service, which applies the user's permissions.

### Requirements carried to the GUI design

- The GUI's input and drawing threads never make synchronous calls to `wpsd`.
- Each folder window updates independently. A folder that is still loading shows progress; it never blocks input to other windows.
- If `wpsd` is restarted, open windows reconnect and refresh; the desktop does not freeze or close.

## Object runtime

The runtime, `libwpo`, is a C library of roughly 3,000–5,000 lines. It provides what made SOM different from a C++ class system: classes are objects themselves, methods can be found by name, and the class set can change at runtime. It depends only on libc and a small platform layer, so the same code builds for X64-DOS.

### Model

- **Classes are objects.** Each class is an instance of a metaclass. Class methods such as `wpclsQueryTitle` and `wpclsNew` are ordinary methods of the metaclass, so subclasses can override them.
- **Single inheritance.** A class has one parent. There are no mixins in version 1.
- **Instance data is per class.** Each class owns a private block inside the object. Its offset is fixed when the class is registered, so a parent can grow without recompiling its subclasses.
- **Method tables.** Each class has a table of function pointers. A subclass's table starts as a copy of its parent's, with overrides replaced. For a class in a plug-in host, the table holds forwarding stubs.

### Dispatch

| Way to call | Use | Cost |
| --- | --- | --- |
| Static: a generated macro looks up a fixed slot in the method table | Built-in and in-process C classes | One indirect call |
| By name: `wpo_send(obj, "wpQueryTitle", args)` | Scripts, the service API, the GUI | Hash lookup, then cached |
| Parent call: `WPO_PARENT(cls, obj, method)` | An override that extends its parent's behaviour | One indirect call |
| Remote: forwarding stub to a plug-in host | Third-party classes | One message round trip, with a deadline |

Every method has a signature descriptor (argument types and return type) and a flag saying whether it may complete asynchronously. A call by name can therefore be checked and packed for the service API or a plug-in host without hand-written glue.

### Defining classes

- A class is described in a small interface file (`.wpi`): name, parent, instance data, new methods and overrides.
- A generator, `wpoc`, turns it into a C header and registration code, much as SOM's compiler turned IDL into bindings.
- Built-in classes are linked into the service. Plug-in classes are shared libraries (`.so`) loaded by a plug-in host; each exports one registration function.

### Class replacement

`wpo_replace_class("WPFolder", "MyFolder")` makes every new request for `WPFolder` create `MyFolder`, which must be a subclass of it. This is the equivalent of OS/2's `WinReplaceObjectClass`. It is how third-party code changes the behaviour of every folder without modifying the core. Objects already loaded keep their class until they are next loaded.

### Binary stability

- Method slots are added only at the end of a class's table and are never reordered.
- Each class carries a version. A plug-in built against an older version still loads: its offsets stay valid because instance data is located per class.
- A plug-in that needs a newer version than the installed one is refused with a clear error, and the rest of the desktop keeps working.

### Lifetime and threads

- Objects are reference-counted. The service keeps a cache of loaded objects and drops idle ones; a dropped object is reloaded on demand from storage.
- Object methods run on the main loop, one at a time, so class code needs no locking. Workers never call object methods; they do I/O and hand results back to the loop.
- Bulk work (listing, copying, moving, scanning) runs as jobs on the worker pool and touches objects only in small batches on the loop. See Scale.

## Class hierarchy and core methods

Version 1 ships seven classes. File-backed objects descend from `WPFileSystem`; everything that exists only as an object descends from `WPAbstract`.

| Class | Parent | Represents | Stored in |
| --- | --- | --- | --- |
| `WPObject` | — | Root: title, icon, object ID, settings | — |
| `WPFileSystem` | `WPObject` | Anything with a directory entry | Extended attributes on the inode, or per-user overlay |
| `WPDataFile` | `WPFileSystem` | A regular file; subclassed by type (text, image, …) | Extended attributes |
| `WPFolder` | `WPFileSystem` | A directory, with its view state and icon positions | Extended attributes on the directory, or SQLite |
| `WPAbstract` | `WPObject` | Objects that are not ordinary files or folders | An object file in the folder where it lives (see Multiple logged-in users) |
| `WPProgram` | `WPAbstract` | A program to start | A standard `.desktop` file |
| `WPShadow` | `WPAbstract` | A pointer to another object; see Links | A managed symlink, recorded on its folder |

Data file subclasses are chosen by MIME type (via shared-mime-info), so `image/png` can map to a `WPImageFile` plug-in. Desktop entries for programs are read from the standard XDG directories and kept in sync with them.

### Core methods

Names follow WPS where the meaning matches, which makes XWorkplace's source a usable reference. Methods marked *async* may return a pending result.

| Group | Methods | Purpose |
| --- | --- | --- |
| Lifecycle | `wpInitData`, `wpUnInitData`, `wpclsNew`, `wpFree` | Create, load and destroy an object |
| Persistence | `wpSaveState`, `wpRestoreState` (async), `wpSaveDeferred` | Write and read the object's state; `wpSaveDeferred` batches writes |
| Identity | `wpQueryTitle`, `wpSetTitle`, `wpQueryIcon`, `wpQueryHandle` | Name, icon and stable object ID |
| Menus | `wpModifyPopupMenu` (async), `wpMenuItemSelected` | Each class adds its own right-click items and handles them |
| Settings | `wpAddSettingsPages` (async) | Each class adds pages to the Properties notebook |
| Open | `wpOpen`, `wpQueryDefaultView` | Open a view or start the program |
| Move and copy | `wpMoveObject`, `wpCopyObject`, `wpCreateShadow`, `wpCreateFromTemplate` | Single-object operations; bulk ones run as jobs |
| Drag and drop | `wpDragOver`, `wpDrop` | Each target class decides what a drop means; both take a selection, not a list |
| Folder contents | `wpPopulate` (async, streaming), `wpQueryContent`, `wpAddToContent`, `wpDeleteFromContent` | Enumerate and track a folder's objects |
| Notification | `wpObjectChanged` | Called when the change tracker reports an outside change |

Menus and settings pages are returned as data descriptions, not widgets. The core never draws; the GUI turns those descriptions into menus and pages.

### Templates

Any object can be marked as a template. Dragging a template, or calling `wpCreateFromTemplate`, creates a new object of the same class with the template's settings copied. For a data file, the template's contents are copied too.

## Object identity

Every object has a permanent **object ID**: a 128-bit random value created once and never reused. Everything that refers to an object (shadows, folder icon positions, the GUI, scripts) stores the object ID, never a path. A separate table maps each ID to wherever the object is now.

### Lazy assignment

A file or folder gets an object ID only when WPS first stores something about it: a title, an icon, a position set by the user, a shadow pointing to it, or a template flag. Until then it is shown from its directory entry alone and costs nothing to track. Temp files, build outputs and the contents of caches therefore never become registered objects.

IDs are written with `setxattr(..., XATTR_CREATE)`, which fails if the attribute already exists. If two processes or two users assign an ID at the same moment, one wins and the other reads the winner's ID.

### Locating a file-backed object

| Field | Source | Purpose |
| --- | --- | --- |
| Filesystem UUID | `blkid` / the superblock; for btrfs, also the subvolume ID | Which filesystem, stable across reboots and remounts |
| File handle | `name_to_handle_at()` | Which inode on that filesystem. On ext4 and btrfs it includes the inode's generation number, so a reused inode number gets a different handle |
| Cached path | The last path seen | Fast lookup; may be stale |
| ctime and size | `statx()` | Detects changes made while the service was not running |

The object ID is stored in the file's `user.wps.id` extended attribute. If the database and the file disagree, the extended attribute wins, because it travels with the file. A user who cannot write a file cannot store an ID on it; that user's service keeps the ID in its own database instead (see Concurrency and multiple users).

### Resolving an ID

1. Look up the object ID in the database to get the filesystem UUID, handle and cached path.
2. `stat` the cached path and compare its handle. If it matches, done.
3. Otherwise, if the filesystem is mounted, ask `wps-watchd` to find the file by its handle. The helper does this under the asking user's filesystem credentials, so it returns only paths that user may see. Then update the cached path.
4. If the handle no longer opens, the file has been deleted or replaced. Wait for the change tracker's verdict (see Edge cases) before treating the object as gone.
5. If the filesystem is not mounted, the object is **unavailable**, not deleted. Shadows to it show as unavailable until the volume returns.

Resolution runs on a worker; callers get a pending result.

### Filesystems without handles

FAT, exFAT and some network filesystems either cannot produce file handles or produce ones that do not survive a remount. M0 found that FAT's handles do stay valid across moves while the volume remains mounted, so moves are tracked by handle during a session; after a remount, objects are matched again by volume UUID plus path. FAT also has no extended attributes, so its object state always lives in the database. This is weaker, and the GUI should say so in the object's settings.

## Persistence

An object's state is stored on the thing it describes wherever possible. That follows OS/2, where the WPS kept state in extended attributes on top of an ordinary filesystem. Files, folders and their state then move together under `mv`, backups and `cp -a`.

### Where each kind of state lives

| State | Store | Key |
| --- | --- | --- |
| A file's or folder's own settings (title, icon, class, template flag), when the user can write the file | Extended attributes on its inode | `user.wps.*` |
| The same settings, when the user cannot write the file | Per-user overlay in SQLite | Filesystem UUID + handle |
| Layout of a folder the user owns (icon positions, sort order, view type) | Extended attribute on the folder if it fits, otherwise SQLite | Each child's object ID |
| Layout of a folder the user does not own | Per-user SQLite | Folder handle + child IDs |
| Abstract objects (programs, shadows, future printers and settings objects) | A file in the folder where the object lives: `.desktop`, symlink or `.wpobj` | The file's `user.wps.id` |
| Shadow records for a folder (symlink name → target object ID) | Extended attribute on the folder, `user.wps.shadows` | Symlink name |
| Abstract objects that are in no folder (internal objects) | SQLite | Object ID |
| ID → location map (UUID, handle, cached path) | SQLite | Object ID |
| Fallback for filesystems without extended attributes | SQLite | Volume UUID + path |

Folder layout is stored on the folder, not on its children, as OS/2 did with its `.ICONPOS` attribute. A file with several hard links can then appear at a different position in each folder.

### Extended attribute format

- **One attribute per class**: `user.wps.WPObject`, `user.wps.WPFolder`, and so on. Each class's `wpSaveState` writes only its own attribute, so a plug-in class cannot corrupt its parent's data.
- Values are a small tagged binary format (key, type, value) with a version byte and a **change counter**. Unknown keys are kept on rewrite, so older code does not delete newer settings.
- `user.wps.id` holds the object ID (see Object identity).
- ext4 limits all of a file's extended attributes to about one filesystem block (normally 4 KB) unless the `ea_inode` feature is on; btrfs limits each value to about 16 KB.

### Folder layout: all or nothing

A folder's layout lives **entirely** in its extended attribute when it fits, or **entirely** in SQLite, never split between them. When it is in SQLite, the attribute holds only a marker and the change counter. Switching writes SQLite first, then the attribute, so a crash between the two leaves the old complete layout. Folders with more than about 2,000 positioned items always use SQLite, stored as indexed rows rather than one blob, so a single moved icon is a single-row update. Only items the user has positioned are stored; the rest are laid out automatically.

Layout is the least valuable state: the worst failure is rearranged icons, never lost files or broken identity.

### The database

- One SQLite file per user: `~/.local/share/wps/objects.db`, in WAL mode. Only that user's `wpsd` opens it; every other client goes through the API.
- Tables: `objects` (ID, class; state only for objects in no folder), `locations` (ID, UUID, handle, path, ctime, size), `overlay` (per-user settings for files the user cannot write), `layout` (large folders and folders the user does not own), `shadows` (an index of shadow records, rebuilt from the folders), `jobs` (the job journal), `volumes` (UUID, last generation or scan time).
- The database can be rebuilt. Everything in it except overlays, per-user layouts, internal objects and the journal can be regenerated by scanning mounted volumes, because abstract objects in folders are files. The non-rebuildable parts are also exported to a plain text file daily, as a backup.
- Database writes run on a dedicated writer thread in batched transactions, never on the main loop.

### Write policy

`wpSaveDeferred` marks an object dirty. The service writes dirty objects after 2 seconds of quiet, on shutdown, and before any move or copy of that object. Writes run on workers. A crash loses at most the last few seconds of layout changes, never object identity.

### State that travels with a volume

Decided 2026-11-18: a removable volume carries its own copy of each user's state about it, so the state follows the disk to another computer, much as a git clone carries its history. The user's main database stays the authority on their own computer and always holds a full copy of every volume's portion.

**What travels.** Everything in a user's database that belongs to one volume and does not already live in the files' extended attributes: per-user overlays, per-user folder layouts, object IDs and settings for files on filesystems without extended attributes (FAT, exFAT), and the path-keyed records used there. Settings stored in extended attributes travel by themselves.

**Where.** `<volume root>/.wps/<identity>.db`, a small SQLite file per user. The `.wps` folder is created mode 1777 (like `/tmp`), so each user can create their own file, and each file is mode 0600. On FAT and exFAT there are no permissions; see Privacy below.

**Which volumes.** Volumes on removable or USB-attached devices (`removable` in sysfs, or a USB bus), plus any volume the user marks as portable. Not the volume holding the user's home folder, and never system volumes. The user can turn it off per volume.

**Identity across computers.** Each user has a WPS identity: a random ID created on first use, stored in `~/.config/wps/identity`. Volume databases are named by it, not by the numeric user ID, which differs between computers. To be the same WPS user on another computer, copy the identity file there, as one would copy an SSH key or a git configuration. A friendlier way to link identities is a GUI matter.

**Synchronisation, like git.**
- Every row carries a revision: a logical clock (one more than the highest revision either side has seen) plus the identity and computer that wrote it.
- While the volume is mounted and writable, every change is written to both databases by the database writer.
- While it is absent or read-only, changes go to the main database only and are marked as not yet pushed.
- On mount, the service pulls (rows newer on the volume replace the main copy) and pushes (rows newer in the main copy, or not yet pushed, are written to the volume), all in a background job.
- Deletions are kept as tombstones for 30 days, so a deletion on one computer is not undone by an old copy on another.
- If the same row was changed on two computers while they were apart, the higher revision wins, with the writer's ID breaking ties. The losing value is kept in a `conflicts` table, so nothing is lost silently; the GUI can show it later.

**Privacy.** On FAT and exFAT, anyone holding the disk can read the `.wps` files. They contain titles, layouts and IDs, not file contents. The per-volume setting lets a user keep a volume's state on their computer only.

## Concurrency and multiple users

The filesystem already arbitrates access between processes and users. WPS adds state of its own on top, so it must not assume it is the only writer of that state. Three kinds of overlap are handled: several clients of one user's service, several processes writing the same extended attributes, and several users sharing files.

### One service per user

- Each user has exactly one `wpsd`, started as a systemd user service and enforced by a lock on the database. Two sessions of the same user (a desktop and an SSH login, say) share it.
- Every client of that user goes through the service, so its in-memory state is the single authority for that user.

### Many clients of one service

- Object methods run one at a time on the main loop, so two clients can never interleave halfway through a method.
- Every object carries its change counter. A settings write from a client includes the counter it last saw. If the object changed since, the write is refused with the current values, and the client shows them (optimistic concurrency).
- Bulk jobs reserve the objects they move or copy. Another operation on a reserved object either queues behind the job or fails at once with "busy", as the caller chooses; it never blocks the loop.

### Many writers of the same extended attributes

Other processes can write `user.wps.*` too: another user's `wpsd` on a shared file, a backup restore, or a script.

- Each read-modify-write of a class attribute happens on a worker under an advisory `flock` on the file, taken with a non-blocking try and retried for at most 200 ms.
- Under the lock, the worker rereads the attribute, merges only the keys this change touched, increments the change counter, and writes it back. Two writers changing different keys both succeed.
- If the lock cannot be taken in time, for example because another user holds it indefinitely, the change goes to this user's overlay and is marked pending. Nobody waits on another user's lock.
- `FAN_ATTRIB` events tell each service when an attribute changed under it, so its cached state is refreshed.

### Many users sharing files

- **Shared settings follow file permissions.** A file's own settings (title, icon, class) live on the inode and are seen by every user who can see the file. Linux allows `user.*` attributes to be changed only by someone with write permission on the file, so the kernel decides who may change them.
- **Without write permission, changes stay private.** A user who cannot write a file can still rename its icon title or set its icon, but the change goes to their own overlay. Their view shows overlay values over shared ones; other users never see them.
- **Layout is per user in folders they do not own.** Two people arranging a shared folder each keep their own arrangement, stored in their own databases. The folder owner's layout lives on the folder.
- **Events are filtered per user.** One `wps-watchd` serves all users. Each user's service registers the directories it tracks by passing open directory descriptors over its socket. Because the service opened them, the kernel has already checked access. The helper forwards only events under directories that user registered, and performs any handle-to-path lookup with the user's filesystem credentials (`setfsuid` in a worker thread). The exact rules, validated by the M0 prototype, are in Change tracking.
- **No user can slow another.** Locks are always taken with a timeout, the helper has a separate queue per user, and an overflowing queue triggers a rescan for that user only.

## Multiple logged-in users

Linux is multi-user all the time. Several people can be logged in at once, at the console, through remote desktops or over SSH, each with a WPS session open on the same shared folders and programs. This section covers what happens when they act at the same moment.

### Model

- Each user has their own `wpsd`, running with that user's credentials inside that user's systemd slice.
- **Services of different users never talk to each other.** The only shared state is the filesystem: files, their extended attributes, and object files. Everything one user's service learns about another user's actions comes from the kernel through fanotify or inotify, filtered by permissions. There is no cross-user channel to attack, and no service can be made to wait for another user's service.
- It follows that anything more than one user must see has to live in the filesystem, under ordinary permissions. Two rules below come from this: abstract objects are files, and shadow records are stored on their folder.

### Shared folders

| User A, in a shared folder | What user B sees | How |
| --- | --- | --- |
| Creates, renames, moves or deletes a file | B's open view of the folder updates within about 1 s | B's service registered the folder; fanotify, or inotify for open folders |
| Changes a file's title or icon, with write permission on it | The new title or icon | Shared attribute on the inode; `FAN_ATTRIB` |
| Rearranges icons | Nothing changes for B | Layout is per user in folders they do not own. B starts from the owner's arrangement and keeps B's own changes |
| Creates a program, shadow or other abstract object | The new object, if B can read the folder | Abstract objects are files (below) |
| Moves a file to the trash | It disappears for B; only A can restore it | Per-user trash on each volume (`.Trash-<uid>`, per the XDG trash specification) |

### Abstract objects are files

In v0.2 abstract objects lived only in their creator's database, so no other user could see them. They are now files in the folder where they live:

| Object | Stored as |
| --- | --- |
| `WPProgram` | A standard `.desktop` file, which every other Linux desktop and launcher can also use |
| `WPShadow` | A managed symlink, with its record on the folder |
| Other abstract classes (future printers, settings objects, web links) | A small `.wpobj` file holding the class name and state in the same tagged format as the extended attributes, with its ID in `user.wps.id` |

- File permissions decide who can open, change or delete the object, exactly as for any file.
- Object files are written to a temporary name and renamed into place with `RENAME_NOREPLACE`, so readers never see a half-written object and two creators cannot overwrite each other.
- This applies in private folders too. One rule is simpler than two, and the database becomes almost fully rebuildable. Only internal objects that are in no folder stay database-only.

### Shadows maintained by any user

A managed symlink in a shared folder may need rewriting while the user who made it is logged out. Shadow records are therefore stored on the folder (`user.wps.shadows`), not in one user's database.

- Any logged-in user's service that sees the target move, and can write the folder, rewrites the symlink. It does so under the folder's lock, checking the change counter, so two services repairing the same shadow at the same moment do the work once.
- If no one with write permission on the folder is logged in, the catch-up scan repairs the symlink at the next login of someone who can.
- A shadow whose target the viewing user cannot read is shown as "no access", not as broken.

### Simultaneous operations

| Situation | Handling |
| --- | --- |
| A and B drop a file with the same name into one folder at the same moment | Every create, copy and move uses no-replace operations (`O_EXCL`, `renameat2` with `RENAME_NOREPLACE`, `linkat`). One wins; the other gets a name conflict and is asked. Nothing is silently overwritten |
| A moves or deletes a folder while B is copying from it | B's job reports the items that disappeared and completes the rest |
| A renames a file while B has its Properties open | B's save carries the old change counter and is refused with the current values |
| A's and B's services both react to the same outside change, such as an atomic save of a shared file | Repairs are idempotent and done under the file's lock with a change-counter check; the second service finds the work already done |
| A and B assign an ID to the same file at once | `XATTR_CREATE` lets exactly one ID win |
| A starts a large copy into a shared folder, then logs out | A's jobs are paused and journaled at logout and resume at A's next login. With systemd lingering enabled for A, they continue |
| A opens a document that B already has open | Each opens it with their own program in their own session. Locking the document is the application's job |

### Shared applications

- **System programs** come from the standard `.desktop` directories (`/usr/share/applications`, `/usr/local/share/applications`). Every user sees them as program objects; only an administrator can change them. A user's change to their title or icon goes to that user's overlay.
- **Programs placed in a shared folder** are `.desktop` files there. Anyone who can write the file can change it for everyone.
- **A program always runs as the user who launches it**, in that user's session, with that user's environment and permissions. It never runs as the user who created the object.
- **Untrusted programs ask first.** A `.desktop` file is launched without confirmation only if it is owned by the launching user or by root. Otherwise WPS shows the command it would run and asks, so one user cannot plant a program that runs when another double-clicks it.
- **Administrator-provided objects**, such as a Programs folder for everyone, live in system folders: `/usr/share/wps/objects` for packages and `/etc/wps/objects` for local additions. They are read-only to users.

### Fairness between users

- Each user's service runs in that user's systemd slice, so the kernel's cgroup controls share CPU, memory and disk I/O between users.
- The helper keeps a separate bounded queue per user, so one user's event flood never delays another user's events.
- Jobs run at reduced I/O priority within their user's slice.

### Root sessions

Running WPS as root is supported but discouraged, because root's service could see every user's files. A root session shows a warning, and never registers objects inside other users' home folders unless root explicitly opens them.

## Links

A WPS shadow is a **WPS object file**, as in OS/2, where a shadow was a WPS object and not a file-system link (decided 2026-11-18; until then shadows were managed symlinks, which became plain symlinks when moved to another folder). Symlinks and hard links made by other tools keep their Linux meaning.

| Kind | On disk | When the target moves | When the target is deleted | WPS shows it as |
| --- | --- | --- | --- | --- |
| Shadow (of a file, folder or abstract object) | `<name>.wpobj`: `Class=WPShadow`, the target's object ID, and its last known path | The service updates the path hint; the ID does not change | The service removes the shadow (a move to the trash is not a deletion) | The target's title and icon, with a link mark and the title in the shadow colour |
| Symlink made by the user (`ln -s`) | A symlink only | Left alone; may dangle | Left alone | Link; opening it opens what it points to |
| Hard link (`ln`) | Another name for the same inode | Not affected | The file lives while any name remains | The same object, shown in each folder |

### Shadow files

- The target's **object ID** is what makes a file a shadow, so a shadow stays a shadow wherever it is moved or copied, by WPS or by any other program, and on another computer that sees the same volume.
- The **path hint** (`TargetPath`) saves a lookup and lets a listing show the target's title and kind at once. When it is stale, opening the shadow finds the target by ID.
- Each service keeps an index of the shadow files it has seen: those it created, loaded, listed, found in a folder it loads, or saw appear in a watched folder. When a target moves, every indexed shadow's hint is updated: in place when the user can write the file (so the shadow's own settings stay), otherwise by writing a new file and renaming it over, which any user who can write the folder may do (see Multiple logged-in users).
- If the target is on a volume that is not mounted, the shadow shows as unavailable.
- Other programs and the terminal see a small text file, not a link they can follow. That is the price of shadows that survive any move.

### Symlinks made by the user

A user-made symlink may deliberately point at a path, such as whatever is mounted at `/media/backup`. WPS therefore never rewrites it unless the user chooses "Track target". That records it in `shadows` and makes it managed from then on.

### Hard links

- All names of a hard-linked file share one inode, so they share one file handle, one set of extended attributes and **one object ID**. That is correct: they are one file.
- Per-folder state (position, sort order) lives on each folder, so each name can sit at a different place.
- When one name is replaced by an atomic save, it gets a new inode and the link silently splits. If the object has settings the user made, the split is noted in its Properties. It is never shown as a pop-up, and files without IDs (build caches, package stores) are never reported.

### Safety rules

- Never follow symlinks in tree walks, scans, recursive copies or folder size calculations. Use `O_NOFOLLOW` and `AT_SYMLINK_NOFOLLOW`, and treat a symlink as a leaf object. A symlink to a parent directory would otherwise loop.
- Deleting a shadow deletes the shadow file, never the target.
- Removing shadows when a target is deleted applies only to shadow files, never to symlinks made by users.

## Change tracking

Changes made outside WPS are caught in three layers. fanotify watches whole filesystems while the service runs. inotify refreshes open folder windows and is the fallback when the helper is not installed. A catch-up scan at mount covers whatever happened while nothing was watching. Access time is not used: with the default `relatime` mount option it is updated at most once a day, and it records reads, not moves.

### Layer 1: fanotify, through a privileged helper

- Watching a whole filesystem (`FAN_MARK_FILESYSTEM`) needs `CAP_SYS_ADMIN`. A small helper, `wps-watchd`, runs as a systemd service with only that capability and `CAP_DAC_READ_SEARCH` (for `open_by_handle_at()`).
- It marks each mounted filesystem holding a directory that some user's service has registered, with `FAN_REPORT_DFID_NAME | FAN_REPORT_FID`.
- Events used: `FAN_RENAME` (kernel 5.17 or later; old and new location in one event), `FAN_CREATE`, `FAN_DELETE`, `FAN_DELETE_SELF`, `FAN_ATTRIB` (extended attribute changes) and `FAN_CLOSE_WRITE`. Close-after-write tells the service when a file's contents may have changed.
- Events carry file handles, not paths. They match the handles stored in `locations` directly, so most events are a single indexed lookup.
- **Filtering happens in the helper, before forwarding:** events under excluded paths are dropped, events for files without an ID are dropped unless their folder is open in a client, and each user receives only events under directories they registered (see Concurrency and multiple users). This keeps a parallel build from flooding the service.
- Events are coalesced per file (several writes become one) and buffered per user. On overflow, whether the kernel's (`FAN_Q_OVERFLOW`) or the helper's, the affected user's service runs a catch-up scan of that filesystem as a background job.
- The helper keeps no object state and should stay around 800 lines, so it is easy to audit.

### Per-user filtering in the helper

Prototyped in M0 (`proto/m0/watchd.c`) and checked by 11 scenarios with two users; all pass. A filesystem-wide watcher sees every user's file names (M0 scenario 16), so these rules are what keeps one user's activity private from another.

- **Register by descriptor.** A user's service registers a folder or a single file by sending an open descriptor for it (`SCM_RIGHTS`). The helper identifies it by file handle, and also checks access itself under the user's credentials, because a descriptor opened earlier may outlive a permission change.
- **Forward only what is registered.** An event is forwarded to a user only if its folder, or the file itself, is registered by that user and still accessible. Events in folders nobody registered go nowhere.
- **Re-check after permission or location changes.** Any change of a folder's permissions or location (`FAN_ATTRIB`, `FAN_RENAME` or `FAN_MOVE_SELF` on a folder) invalidates all cached access checks. Each registration is re-checked before its next event, and a registration that fails is dropped with a `revoked` notice. Re-checks are rare: 11 for about 160,000 events in the M0 run.
- **Hide the invisible side of a rename.** A file moved from a folder a user can see into one they cannot is reported to that user as a rename to `<hidden>`. Neither the new name nor the folder is revealed.
- **Never wait for a slow service.** Each user's socket has a 4 MB buffer. When it is full, lines for that user are dropped and counted. As soon as the socket has room, the user receives `{"overflow": N}` and rescans what it registered. In M0, a service stalled for 8 seconds during 60,000 creates received an overflow notice afterwards, while another user's service received all 60,000 events in 2.5 seconds.
- **Credentials.** The prototype reads supplementary groups from `/proc`; the real helper uses `SO_PEERCRED` and `SO_PEERGROUPS`.

### Excluded paths

Files under excluded paths are never registered as objects and their events are never forwarded. The defaults are `.git`, `node_modules`, `/nix/store`, `~/.cache`, and common build directories (`build`, `target`, `dist` when they contain build-tool markers). Users can edit the list. A folder under an excluded path can still be opened and browsed; it just does not keep WPS state.

### Layer 2: inotify for open folders

- Every folder open in a client gets an inotify watch. It needs no root, so the folder refreshes at once even without the helper.
- Renames arrive as `IN_MOVED_FROM` and `IN_MOVED_TO`, paired by their cookie. A move out of watched folders looks like a delete; it is confirmed by the next catch-up scan.
- Without the helper, the service also watches every folder it has stored state for, up to a limit (default 50,000 watches, about 50 MB of kernel memory).

### Layer 3: catch-up scan at mount

Run as a background job when a volume is mounted, when the service starts, and after an overflow.

| Filesystem | Method | Cost |
| --- | --- | --- |
| btrfs | `btrfs subvolume find-new` from the generation saved at the last clean shutdown | Proportional to the changes |
| ext4, XFS | Walk the tree, skipping directories whose ctime has not changed and excluded paths; compare each registered file's handle, ctime and size with `locations` | Proportional to the number of directories |
| FAT, exFAT, network | Walk the tree and match by path | Proportional to the number of files |

The scan runs at low I/O priority. Objects it has not yet reached are shown as normal, and corrected when it reaches them.

### Events caused by WPS itself

When a job moves or copies files, it registers the events it expects with the change handler. Matching events are consumed silently, so WPS's own operations are not re-detected as outside changes and need no heuristics.

## Edge cases

Most outside changes need no special handling: `mv` within a filesystem keeps the inode, so the object and its extended attributes move together, and only shadows need updating. The cases below are the ones where the inode changes or disappears.

| Situation | What the kernel reports | What the service does |
| --- | --- | --- |
| `mv` within one filesystem | `FAN_RENAME`, same handle | Update the cached path; rewrite managed symlinks to it |
| Atomic save: an editor writes a temp file (in any folder) and renames it over the original | `FAN_RENAME` onto a name with an object; the incoming inode has no ID | Same object, new contents: keep the object ID, copy the settings onto the new inode, update the handle |
| Save by renaming the original away (vim's default: `file` → `file~`, a new `file`, then `file~` deleted) | `FAN_RENAME` of the object within its folder; `FAN_CREATE` of a file without an ID at the old name; later `FAN_DELETE_SELF` of the object | Displacement rule (below): when the renamed-away object is deleted, its identity passes to the new file |
| `mv a b` in one folder, where both already have IDs | `FAN_RENAME` onto a name with an object; the incoming inode has its own ID | A real replacement: `b`'s object is deleted; `a`'s object keeps its ID |
| `mv` across filesystems (copy, then delete) | `FAN_CREATE` of a file carrying an existing `user.wps.id`, later `FAN_DELETE` of the original | Pending duplicate until the original is deleted: then a move, and the ID, settings and shadows carry over. No timer |
| `cp -a` or a restore from backup | `FAN_CREATE` carrying an existing ID; the original stays | Both share the ID until either is changed (its modification time differs from what the copy preserved, or its WPS settings change): then a copy, and the new file gets a new ID with its settings kept |
| A tool that drops extended attributes (moving to FAT, some file managers) | `FAN_CREATE` with no ID, `FAN_DELETE` of an object elsewhere | Best effort: pair by name and size, and say so in the object's settings |
| `cp` without `-a` | `FAN_CREATE`, a new handle with no ID | Nothing is registered until WPS stores something for it |
| Moved to the trash (`~/.local/share/Trash/files`, `<volume>/.Trash-<uid>/files`) | `FAN_RENAME` into the trash folder | Not a deletion: the object keeps its ID and settings, its shadows follow and are reported as in the trash; a restore is a move back |
| `rm` | `FAN_DELETE_SELF` once the last link is gone | Remove the object and its managed shadows; keep its row for 7 days so an undo or a restore can reclaim the ID |
| Hard link split by an atomic save | Rename onto a name whose inode had other links; the surviving inode reports `FAN_ATTRIB` as its link count drops | Keep both; note it in Properties only if the object has user settings |
| Volume unplugged or unmounted | Unmount, no deletes | Mark its objects unavailable; never delete them |
| Volume changed on another machine | Nothing while away | Catch-up scan when it is mounted again |
| A filesystem without extended attributes | Setting the attribute fails with `ENOTSUP` | Store state in SQLite by volume UUID and path |
| Extended attributes stripped (by `tar` without `--xattrs`, `rsync -a` without `-X`, plain `cp`) | `FAN_ATTRIB`, or found by the scan | Restore from the database copy if the handle still matches |
| Two writers change one object at once | Both write under `flock`; or `FAN_ATTRIB` from another user | Keys merged; a conflicting client write is refused with the current values |

### Detecting saves

M0 tested how real programs save a file (`proto/m0/RESULTS.md`). GNOME Text Editor and gedit (through their save libraries and their real windows), GLib's `gio save`, LibreOffice and `sed -i` all write a temporary file and rename it over the original. vim's default renames the original away and creates a new file; full vim built with `+xattr` copies the attributes across, so the ID survives, but vim-minimal and builds without `+xattr` lose it. Two rules cover both, and neither uses timing:

1. **Rename-over rule.** A rename onto a name that holds an object, where the incoming file has no ID, keeps the object: same ID, settings copied to the new inode. The replaced inode is reported by its own handle (`FAN_DELETE_SELF`), so no name lookup is needed. If the incoming file has its own ID, it is a real replacement instead.
2. **Displacement rule.** When an object is renamed within its folder, its former name is remembered as displaced. If a file without an ID is then created at that name, the two form a candidate pair. When the displaced object is later deleted while the pair exists, its identity passes to the new file: same ID, settings copied, shadows retargeted. The pair is cancelled if the new file is removed or renamed, or the displaced object is renamed again or modified.

When the user keeps editor backups (`set backup` in vim), the backup is never deleted, so rule 2 never completes and the object stays with the backup. For that case there is a setting, off by default: with `backup-names` on, a rename to `name~`, `name.bak` or `name.orig` followed by a new file at the old name counts as a save immediately.

## Scale: large folders and bulk operations

Size never reaches the main loop. A folder of a million files, a drag of a million objects or a 50 GB file costs the main loop the same as small ones; the extra work runs as jobs on workers, at lower priority than interactive requests.

### Opening a folder with many files

1. **Directory read in a worker.** The job reads entries with `getdents64` in chunks of about 4,096 and publishes them as lightweight records: name, type from the directory entry, and a provisional icon chosen by file extension. No `stat`, no extended attribute reads yet.
2. **First page first.** The client asks for the range it can show (`query_content` with offset and count, plus a viewport hint as it scrolls). Records stream to the client as they are found, with a `complete: false` flag and the count so far. The target is the first page of a 100,000-file folder in under 100 ms.
3. **Details for what is visible.** `statx` calls and extended attribute reads run in parallel workers, visible range first, then the rest in the background. Each batch arrives as `object_changed` events.
4. **Refinement later.** Content-based type detection (reading the first few kilobytes), thumbnails and plug-in icon queries run last, at idle priority.
5. **Full objects only on demand.** A record becomes a full object only when it is opened, right-clicked, edited or dragged onto something that needs it.

Sorting by name proceeds as records arrive. Sorting by size or date needs every file's details, so it runs as a job with progress; the client shows the folder unsorted with a progress indicator until it finishes. Records are compact (around 100 bytes plus the name). The in-memory record cache is capped (default 128 MB), and records for folders that do not fit are reread on demand.

### Selections and drag and drop

- A **selection** is a server-side set, referenced by a selection ID. It can be an explicit list or a rule such as "everything in this folder except these". Creating one for a million files is constant-cost.
- Drag and drop passes only the selection ID. `wpDragOver` receives a summary (count, classes present, whether any are folders), not the objects, so hovering over a target costs the same for any size.
- `wpDrop` starts a job and returns at once with the job ID.

### Bulk jobs

- **Moves within one filesystem** are one `rename` per top-level item. Moving a folder of a million files is a single rename; object IDs inside are unaffected because identity is inode-based. Shadows pointing into the moved tree are rewritten afterwards as part of the same job.
- **Copies and moves across filesystems** use reflinks (`FICLONE`) where the filesystem supports them, which is instant on btrfs and XFS, and otherwise `copy_file_range` in chunks of about 64 MB. Extended attributes are copied, so IDs carry over for moves.
- **Per-object class hooks only where needed.** If no class in the selection overrides `wpMoveObject` or `wpCopyObject`, the job runs without calling object methods at all. If some do, they are called in batches on the main loop, within the loop's time budget, or in their plug-in host.
- **Progress, pause and cancel.** Jobs report progress events (items, bytes, current name). Cancel takes effect within one chunk; a half-copied file is removed.
- **Conflicts** such as an existing name pause only the affected item and ask once, with "apply to all".
- **Journal.** Each job records its plan and progress in the `jobs` table. After a crash, the service reports the job as interrupted with what was completed, and offers to resume copies. Each individual move is atomic, so no item is left half-moved.
- **Isolation.** Jobs run at lower I/O priority, with a concurrency limit per device, so a large copy cannot starve folder browsing on the same disk.

### Large files

No operation reads a whole file on behalf of the object model. Type detection reads at most the first few kilobytes, in a worker. Sizes come from `statx`. Large copies run as jobs as above.

## Service API

The GUI, `wpsctl` and scripts all talk to the per-user service `wpsd` through one interface: messages over a Unix socket at `$XDG_RUNTIME_DIR/wps.sock`. The API is generic — call any method by name on any object — so a new class or plug-in method is available to every client without changing the protocol. Every request is asynchronous: a client may have many in flight, and each reply echoes its request ID.

### Messages

Length-prefixed messages, encoded in CBOR (compact binary JSON, with small C libraries).

| Message | Arguments | Returns |
| --- | --- | --- |
| `resolve` | path, or object ID | Object ID (or none, if not yet registered), class, title |
| `call` | object ID, method name, arguments, optional change counter | Method's return value, a pending result, or an error (`conflict`, `busy`, `unavailable`) |
| `describe_class` | class name | Parent, methods with signatures, version |
| `query_content` | folder ID or path, offset, count | Records with name, ID if any, class, title, icon and position; `complete` flag and count so far |
| `set_viewport` | folder, offset, count | Moves detail loading to that range first |
| `selection_create`, `selection_add`, `selection_free` | items or a rule | Selection ID |
| `job_cancel`, `job_pause`, `job_resume` | job ID | — |
| `subscribe` | object IDs, a folder, or jobs | Subscription ID; events follow |
| `unsubscribe` | subscription ID | — |

### Events from the service

| Event | Sent when |
| --- | --- |
| `object_changed` | An object's title, icon, settings or details changed, from WPS or from outside |
| `content_added`, `content_removed` | A subscribed folder gains or loses children, sent in batches |
| `object_moved` | An object moved between folders; both folders' subscribers get it |
| `object_unavailable`, `object_available` | Its volume was unmounted or mounted again |
| `result_ready` | A pending result from `call` is complete |
| `job_progress`, `job_paused`, `job_done`, `job_failed` | A job advanced, needs a decision, or ended |
| `in_use` | A program WPS started opened or closed a file (`path`, `in_use`); every subscriber gets it, since shadows of the file may be anywhere |

### Deleting

Built after M5. `trash` and `delete` are jobs on a selection, like move and copy, with progress and cancel.

- **Trash** follows the freedesktop.org Trash specification, so GNOME's Trash and other file managers show and restore what WPS put there. An item on the home filesystem goes to `$XDG_DATA_HOME/Trash`; one elsewhere to `$topdir/.Trash/$uid` (an administrator's sticky `.Trash`) or `$topdir/.Trash-$uid`. The info file is written first, without replacing; the item moves by one rename without replacing (a second item of the same name becomes `name.2`), so it keeps its object ID, settings and shadows, and comes back with them when restored.
- Items no trash can take (already in a trash, or on a drive where none can be made) are listed in the job's `not_trashed`; the GUI asks before deleting them for good.
- **Delete** removes items for good, with their info files when they are in a trash.
- **Central objects** (`vdelete`) are deleted outright: deleting a shadow never touches its original, as in OS/2.

### In-use emphasis for files

Decided 2026-11-18; built after M5. A file or program shows as open (OS/2's hatched in-use emphasis) in two cases. Folder listings carry `in_use: true` for such entries, and for shadows of them.

- **Started from WPS.** A data file opens with its type's default application, read from `mimeapps.list` (desktop-specific lists first, in the XDG order) or else `mimeinfo.cache`, and started directly rather than through `xdg-open`, which hands the file over and exits at once. The started process stands for the open file; it is watched through a pidfd. The service is a child subreaper, so a program that forks and lets its first process exit is adopted and still counted. A file renamed or moved while open stays marked under its new name. Program objects show as open while their program runs.
- **Lock files** next to the file: LibreOffice (`.~lock.NAME#`), vim (`.NAME.swp`), Emacs (`.#NAME`) and Microsoft Office (`~$NAME`). These also mark files opened outside WPS, in any folder WPS lists; the lock file's appearance is an ordinary folder change, so windows update.
- **Executables run.** A file this user may execute that is a binary (ELF) or a script (`#!`) runs when opened, in its own folder, as a program file does in OS/2; one owned by another user (not root) asks first, as program objects do. Text-mode programs run in a terminal window, as OS/2 ran VIO programs in a window: scripts, binaries that link no window library (Wayland, X11, GTK, Qt, SDL and the like; OS/2 told the two kinds apart by the EXE header), and `.desktop` programs and default applications marked `Terminal=true`. The terminal is `$WPS_TERMINAL` (words the command is appended to), else `xdg-terminal-exec`, else the first installed of Ptyxis, GNOME Terminal, Console, Konsole, Xfce Terminal, Alacritty, kitty, foot, WezTerm and xterm, each asked for a window of its own so that the program stays tracked (a setting in the System object comes with the GUI's settings). A file whose name gives no type opens as text when its start has no NUL bytes.
- **Links** are listed with what they point to (`target`: folder, file or missing; `target_path`), so windows draw them as their target with a link mark, and open the target.
- **Limits.** There is no system-wide monitoring. An application that passes the file to an instance already running (most single-window GNOME apps, Firefox) exits at once, so the file shows as open only for a moment, unless the application writes a lock file. Types with no default application, and applications that run in a terminal (`Terminal=true`), still open through `xdg-open`, untracked.

### Menus and settings pages as data

`wpModifyPopupMenu` and `wpAddSettingsPages` return descriptions, not widgets:

- A **menu** is a list of items, each with an ID, a label, an optional submenu, and enabled or checked state. Choosing an item sends `call(obj, "wpMenuItemSelected", item_id)`.
- A **settings page** is a list of fields: label, type (text, number, checkbox, choice, file, icon, colour), current value, and the property it is bound to. Saving the page sends the changed values back through `call` with the change counter.

The GUI needs no knowledge of a class to show its menus and settings, so a plug-in class gets a working Properties notebook without any GUI code. A class can still provide a custom page later, as a GUI plug-in.

### Permissions

The socket is accessible only to its user. The service acts with that user's permissions, so a file the user cannot change cannot be changed through WPS either. Only `wps-watchd` runs with extra capabilities. It accepts only directory registrations (as open descriptors) and handle lookups, and answers lookups under the asking user's credentials.

## Testing

The core is finished when every scenario below passes from the command line, with no GUI. `wpsctl` is the test client: it speaks the same API the GUI will use, so passing tests mean the GUI starts on a working base.

### Test setup

- Each test run creates fresh loopback filesystems — one ext4, one btrfs, one FAT — a throwaway home directory, and two test users, as X64-DOS's `make test` does with its disk images.
- `wpsd` (one per test user) and `wps-watchd` run against them. Tests then act from outside with ordinary tools (`mv`, `cp`, `rm`, `vim -c wq`, `sed -i`, `tar`) and check the results through `wpsctl`.
- Runs in a VM or container with `CAP_SYS_ADMIN`, so it works in CI.
- Every test also checks the main loop watchdog: any slice over 50 ms fails the run.

### Scenarios: identity, tracking and links

| # | Scenario | Pass when |
| --- | --- | --- |
| 1 | Set a file's title; restart the service | Title is restored from extended attributes |
| 2 | Delete the database; restart | Everything except overlays, per-user layouts and internal objects is rebuilt by scanning, including programs and shadows |
| 3 | Create a shadow; `mv` the target to another folder | The symlink points to the new path; `cat` through it works |
| 4 | Same, with the service stopped during the move | Fixed by the catch-up scan at the next start |
| 5 | Edit the target with vi (default settings, which rename the original away), `sed -i`, GNOME Text Editor, LibreOffice, and an editor that writes its temp file in another folder | Same object ID; settings kept; the shadow still works |
| 6 | `mv` a 2 GB target from ext4 to btrfs | The shadow follows; the object ID is kept |
| 7 | `rm` the target | The managed shadow is removed; a user-made symlink is left |
| 8 | `cp -a` a file, then change the copy | Both exist; after the change they have different object IDs |
| 9 | `mv draft.txt final.txt` in one folder, both with IDs | `final.txt`'s object is deleted; `draft.txt`'s ID is kept |
| 10 | Hard-link a file into two folders; set positions in each | One object ID; a separate position in each folder |
| 11 | Atomic save through one hard link | The split is noted only if the object has user settings |
| 12 | Unmount a volume with shadows pointing into it; mount again | Shadows show unavailable, then work again |
| 13 | A symlink loop (`ln -s .. loop`) inside a scanned tree | The scan finishes; no loop is followed |
| 14 | Flood 100,000 renames to force a queue overflow | The rescan runs; every object ends correct |
| 15 | Run a parallel build inside an excluded path | No objects are created; the helper forwards nothing |
| 16 | Run without `wps-watchd` | Open folders refresh via inotify; scenario 4 still passes |

### Scenarios: runtime and plug-ins

| # | Scenario | Pass when |
| --- | --- | --- |
| 17 | Load a plug-in class and replace `WPFolder` with it | New folders get the plug-in's menu item |
| 18 | Load a plug-in built against an older class version | It loads and works |
| 19 | A plug-in that sleeps 60 s in `wpModifyPopupMenu` | Other clients are unaffected; the menu falls back to the parent class's items after the deadline |
| 20 | Kill a plug-in host mid-call | Objects fall back to the parent class; the host restarts; no `wpsd` restart |

### Scenarios: concurrency and multiple users

| # | Scenario | Pass when |
| --- | --- | --- |
| 21 | Two clients change different settings of one object 1,000 times each | No change is lost |
| 22 | Two clients change the same setting at once | One succeeds; the other gets `conflict` with the current value |
| 23 | User A and user B both write settings on a shared writable file | Both changes are kept; each service sees the other's change |
| 24 | User B, without write permission, retitles A's file | B sees the new title; A does not; A's file is unchanged |
| 25 | Users A and B arrange a shared folder differently | Each keeps their own arrangement |
| 25a | Computer A retitles a file on a FAT USB volume; the volume moves to computer B, same WPS identity, its own main database | B shows the new title |
| 25b | A and B both retitle the same file while the volume is apart from one of them | The higher revision wins everywhere; the other value is in `conflicts` |
| 25c | A changes a file while the volume is mounted read-only, then remounts it writable | The change is pushed to the volume |
| 25d | Two users share the USB volume | Each has their own volume database; neither sees the other's state |
| 26 | User B holds `flock` on a shared file forever | A's change goes to A's overlay as pending; A's service stays responsive |
| 27 | User B renames files in B's private folder | A's service receives no event and cannot learn the names |

### Scenarios: several users logged in at once

| # | Scenario | Pass when |
| --- | --- | --- |
| 33 | Users A and B both have a shared folder open; A creates, renames and deletes files | B's view updates within 1 s for each |
| 34 | A and B drop a file named `report.txt` into one folder at the same instant, 100 times | Nothing is ever overwritten; exactly one conflict each time |
| 35 | A creates a shadow in a shared folder and logs out; B moves the target | B's service rewrites the shadow; it works for A at next login |
| 36 | A's and B's services both see an atomic save of a shared file | Settings are carried over once; the object keeps one ID |
| 37 | A creates a program object in a shared folder; B double-clicks it | B is asked to confirm the command; it runs as B |
| 38 | A starts a 10 GB copy and logs out | The job is paused and journaled, and resumes at A's next login |
| 39 | A runs a parallel build in a shared project folder while B browses it | B's interactive requests stay under 20 ms (99th percentile) |
| 40 | A moves away a folder that B is copying from | B's job reports the missing items and completes the rest |
| 41 | Start a root session | A warning is shown; no objects are registered in other users' homes |

### Scenarios: scale

| # | Scenario | Pass when |
| --- | --- | --- |
| 28 | Open a folder of 1,000,000 files | First page within 150 ms; other requests stay under 20 ms (99th percentile) while it loads |
| 29 | Drag-move 1,000,000 files to another folder on the same filesystem | Drag start and hover take constant time; the move finishes in seconds; the loop budget holds |
| 30 | Copy a 20 GB file across filesystems | Progress events arrive; cancel takes effect within 1 s and removes the partial file |
| 31 | Kill `wpsd` during a copy job of 10,000 files | After restart the job is reported as interrupted with an accurate count; resume completes it |
| 32 | Browse folders during a parallel build and a large copy | Interactive requests stay under 20 ms (99th percentile) |

### Other testing

- Unit tests for the runtime: dispatch, parent calls, class replacement, instance data offsets, deferred results.
- A fuzzer for the extended attribute format and the socket protocol, since both read data from outside.
- A latency benchmark run on every change: interactive request time at the 99th percentile under the scale scenarios.

## Milestones

The core takes about 3.5–4 months of focused work for one developer. Durations are estimates. Each milestone ends with the named test scenarios passing.

1. **M0: Tracking prototype — done.** `proto/m0`: a fanotify watcher and 21 kernel scenarios (16 pass, 4 observations, 1 finding: vim's save pattern, now covered by the displacement rule), and a per-user filtering prototype of `wps-watchd` with 11 scenarios, all passing. Results in `proto/m0/RESULTS.md`.
2. **M1: Runtime and main loop — first version done.** `runtime/` (`libwpo` core in C11 using only eight C library functions, the three-queue loop with watchdog and worker pool, plug-in loading), `tools/wpoc.py` (the `.wpi` generator), and the seven classes in `classes/` as first versions. 128 runtime checks and 19 loop checks pass, including plug-ins built against older, newer and incompatible class versions, class replacement by a plug-in, and interactive latency while workers are busy (0.12 ms worst case). Still to do in M1: run the tests under AddressSanitizer (needs `libasan`, `libubsan`).
3. **M2: Persistence, identity and concurrency — done.** `service/`: the attribute store (locked merge, change counters, ID assignment), the database with git-style volume sync (merge base per row, conflicts kept, tombstones), volume and identity handling, the CBOR protocol, `wpsd` and `wpsctl`. Scenarios 1, 2, 10, 21–25, 25a, 25b (which also covers 25c) and 25d pass with real services for three users and two simulated computers (`make m2`), with no sanitizer reports. Simplifications left for later milestones:
    - The service's object table is a linear list; M4 replaces it with an index.
    - Database writes are serialised calls from workers, not yet a batching writer thread (M4, for scale).
    - A file whose lock cannot be taken in time is saved to the user's overlay, but not yet retried into the shared attribute (M3, with change events).
    - Abstract objects (programs) are not persisted yet; they become files in M4.
    - An object passed as a method argument is given an ID when passed.
    - A volume is identified by its filesystem UUID, plus `:<root>` for a btrfs subvolume or bind mount (for example `/home`).
    - A disk's `.wps` folder is created by the first user who can write the disk's root; if nobody can, travelling state stays on each computer until it can be created.
4. **M3: Change tracking — done.** `service/tracker.c` (events from `wps-watchd` or inotify, the identity rules, catch-up scan) and `service/watchd.c` (the real helper, with a hardened systemd unit). Scenarios 4–9, 11, 13–15, 26, 27, 33 and 36, plus the backup-names case, pass in both modes: 30 runs, no sanitizer reports (`make m3`). Scenarios 3, 4 and 7 check object identity here; their shadow parts move to M4 with shadows. Findings and choices made while building it:
    - **btrfs subvolumes refuse fanotify filesystem marks** (`EXDEV`, still on kernel 7.3), and Fedora's `/home` is one. The helper then marks each registered folder instead; the service falls back to inotify for any folder the helper refuses.
    - **Copy tools set the ID after creating the file.** A file created without an ID is checked again when its first attributes arrive.
    - **A duplicate is settled only when the copy is complete**, recognised by its modification time matching the original's (`mv` and `cp -a` set it last). Changes before that are the copy itself.
    - **Moves out of the watched folders** (inotify sees only one side; the helper hides the other user's side) are followed by searching for the object's ID: near where it was first, then the whole volume.
    - **A save that met a locked file** goes to the overlay and is retried into the shared attribute with backoff, then the overlay copy is removed.
    - **Not yet:** the btrfs `find-new` shortcut for the catch-up scan needs root, so it belongs in the helper; the scan walks the tree for now, and only when an object is missing.
5. **M4: Scale, shadows, object files and jobs — done.** `service/shadows.c`, `listing.c` and `jobs.c`, plus programs and `.wpobj` files in the object layer. 18 scenarios (`tests/m4/m4.py`): 3, 4 and 7 for shadows, 12, 28–32, 34, 35, 38, 40, the trash (T1, T2) and abstract objects as files (A1–A3). All pass in inotify mode, and the 12 that are not scale tests also with `wps-watchd`; no sanitizer reports. Findings and choices:
    - **Moving a million separate items takes about a minute** on ext4 (about 16,000 renames per second, kernel-bound); the service's part is constant-time and the loop stays responsive (99th percentile 0.08 ms). Moving a folder that contains a million files is a single rename.
    - **The first page of a folder is records only**, as designed: 8 ms for a million-entry folder from a cold cache. Details for those rows are disk-bound (about 0.6 s for 50 cold inodes on the loopback test disk) and are read in parallel slices.
    - **Temporary names include the user's identity**, because two users' jobs can write into one shared folder; without that, scenario 34 found them colliding.
    - **Jobs run on their own detached threads** (at most two), journal at most four times a second and every 256 items, pause at logout and resume at the next start; a crashed job is reported as interrupted and resumes, skipping complete items.
    - **The helper must not hold descriptors on watched volumes** (it kept them from unmounting), and `open_by_handle_at` refuses an `O_PATH` mount descriptor; both fixed.
    - **Static calls are now checked** in test builds (`WPO_CHECKED`): calling a `WPFileSystem` method on a shadow (which is a `WPAbstract`) was caught that way.
    - Shadow records live in the folder's `WPFolder` attribute as `shadow:<name>` keys rather than a separate `user.wps.shadows` attribute, so they merge like any folder setting.
    - Not yet: progress as events (polled through `job` until M5's subscriptions), sorted listings, and expected-event suppression for jobs (the tracker's normal rules already follow job moves).
6. **M5: API, programs and plug-in host (2–3 weeks). Done (2026-11-18).** Subscriptions and events, menus and settings as data, `wps-plughost`, `.desktop` programs with launch confirmation, system programs, root sessions, file types, templates. Scenarios 17–20, 37 and 41 pass, with M5's own (events E1–E3, menus and settings S1–S2, programs P2, templates P3, file types P4, sandbox SB); M2–M4 still pass. GUI design starts from here.
    - **Events** (`subscribe`): object changed, content added and removed, object moved, job progress, volumes. Job progress now arrives as events rather than by polling.
    - **Menus and settings are data**: classes return them as small text tables (`id, label, parent, flags`; `page`/`field` rows) that the service turns into structured replies, so a GUI draws them without knowing the classes.
    - **Opening a program** runs its `Exec` line as the user who opens it, in a new session, with field codes dropped. A `.desktop` file owned by another user (not root) first returns its command and owner for confirmation (scenario 37).
    - **System programs** in `/usr/share/applications` are `WPProgram` objects; retitling one is stored in the user's overlay and leaves the file untouched.
    - **Templates**: `from_template` copies a template's contents and settings under a new name, never overwriting, without its ID or template mark.
    - **File types** come from shared-mime-info's glob table (longest match, then weight). Mapping types to classes waits for plug-ins that want it.
    - **Root sessions** log a warning and never watch or scan `/home` or `/var/home` on their own; explicitly opened files still work (scenario 41).
    - **Plug-in hosts**: one `wps-plughost` per plug-in package, over a `SOCK_SEQPACKET` socketpair with CBOR messages. The host sets `no_new_privs`, enters an empty network namespace (Landlock TCP rules where that is unavailable), and restricts files with Landlock: system libraries and the plug-in's folder read-only, its data folder (`<state>/plugins/<name>`) writable. wpsd registers a proxy per plug-in class, which behaves like its parent inside wpsd, and applies the plug-in's class replacements. API calls to methods the plug-in introduces or overrides, and its menus, go to the host with the object's settings; the reply carries the settings the method changed, which wpsd saves. Deadline 2 s; a miss or a crash gives the parent's behaviour, and a crash or three misses in a row restarts the host. A plug-in built against a newer class version than installed is refused and reported in `stats` (scenario 18).
    - Measured: while a plug-in hung for 60 s, other requests' 99th percentile stayed at 0.06 ms (63,000 pings); killing the host mid-call gave the parent's menu in 0.5 s and a restarted host 1 s later.
    - Not yet: calls between objects inside a plug-in that need other objects (a host sees only the object it was called for), plug-in classes for abstract objects, and grouping several packages in one host.

## Deferred: decided in principle, details later

These follow from the principles above but need not be designed in full before M0–M2. Each is revisited at the milestone named.

- [x] **Plug-in host protocol and lifecycle** (M5): one host per package; objects are passed by value with their settings, and changes come back with the reply; the sandbox is the security boundary. Grouping packages and object references inside hosts are left for when a real plug-in needs them.
- [ ] **Helper registration at scale** (M3): M0 used one descriptor per folder and linear lookups. For many thousands of folders the helper needs a hash table by handle, and possibly subtree registration with access checks per event.
- [ ] **Other editors** (M3): full vim and gedit are now tested (M0 scenarios 22–25); Kate and VS Code are not.
- [x] **Events for moves into directories a user did not register**: decided and validated in M0; the invisible side is reported as `<hidden>`.
- [ ] **Per-folder on-disk index for folders beyond about 1,000,000 entries** (M4), if the capped record cache proves too slow.
- [ ] **Network filesystems** (after M5): NFS and SMB vary in extended attribute support and report no remote changes through fanotify.
- [ ] **Disk full and quota errors during jobs** (M4).
- [ ] **Undo for jobs** (after M5).
- [ ] **Opt-in shared layouts** for team folders (after M5).
- [ ] **Showing who has a document open**, by reading the lock files that applications such as LibreOffice already write (after M5).
- [ ] **`.wpobj` format details** (M4): versioning, and how a class missing on one user's system shows to that user (proposed: as a plain object with its title and a "class not installed" note).
- [ ] **Group-shared folders with the setgid bit** (M4): confirm that object files and shadow records created there get the right group and permissions.
- [ ] **Security review of `wps-watchd`** (before any release).
- [ ] **X64-DOS platform layer** (after M5): which parts of the runtime need it; X64-DOS is single-user, so the concurrency layer reduces to the loop.

## Open questions

- [x] Class interface files: decided in M1 — a small custom format (`.wpi`), documented in `tools/wpoc.py`. An IDL subset brought tooling we would not use.
- [x] State for removable volumes: decided 2026-11-18. A per-user database on the volume, synchronised git-style with a full copy in the user's main database (see Persistence: State that travels with a volume).
- [x] `wps-watchd`: decided 2026-11-18. Optional. Without it the service uses inotify on the folders it tracks, and says so in its status.
- [x] Trash: decided 2026-11-18. An object moved to the trash keeps its ID and settings; shadows to it follow it into the trash, are reported as "in the trash", and work again after a restore. Emptying the trash, or deleting permanently, is the real deletion.
- [x] Backup names: decided 2026-11-18. A setting, off by default (`backup-names` in the configuration folder). When on, a rename to `name~`, `name.bak` or `name.orig` followed by a new file at the old name counts as a save at once.
- [x] Duplicate IDs: decided 2026-11-18. No timer. A file that appears with another file's ID shares it until either file is changed (content or WPS settings), which makes it a copy with a new ID, or until the original is deleted, which makes it a move.
- [x] Plug-ins in-process: decided 2026-11-18. Never. Every third-party class runs in a plug-in host; only the built-in classes run inside `wpsd`.
- [x] Where WPS-only objects live: decided 2026-11-18. Centrally by default: shadows and other abstract objects (not programs, which stay `.desktop` files other desktops use) are kept in WPS's database, keyed by their folder's object ID, so a folder moved or renamed outside WPS keeps them; on a removable volume or image they go in the volume's own WPS database, which travels with it. Any folder can be set to keep local `.wpobj` files instead, and local files take precedence over central entries with the same object ID. Central objects are private to each user, like icon positions in folders the user does not own; a shared folder set to local files shares them. Orphans of folders deleted outside WPS are cleaned up by the catch-up scan.
- [x] In-use emphasis for files: decided 2026-11-18. Files and programs WPS itself starts show as open while the started process runs (no system-wide monitoring), and lock files of common programs (LibreOffice, vim, Emacs) in folders WPS watches mark files opened elsewhere.
- [x] Shadows: decided 2026-11-18. WPS object files (`.wpobj`, `Class=WPShadow`, the target's ID and a path hint), as in OS/2, replacing managed symlinks, which lost their shadow record when moved to another folder.
- [x] Renaming a file's or folder's icon: decided 2026-11-18. As in OS/2, it renames the file or folder itself (never replacing another), and its title then follows the name. Without permission to rename, the new name becomes its title, shared or private by the usual rules. Programs and abstract objects only have titles. Service request `rename`.
- [x] Plug-in sandbox: decided 2026-11-18. Plug-in hosts run sandboxed: no network, no new privileges, and (Landlock) filesystem access only to the plug-in's own files and data folder. File work goes through the service, which applies the user's permissions.

## Change log

| Version | Date | Changes |
| --- | --- | --- |
| 0.17 | 2026-11-18 | In-use emphasis for files built: default applications started directly and tracked by pidfd (with subreaping), lock files, `in_use` in listings and events |
| 0.16 | 2026-11-18 | Decided: WPS-only objects kept centrally (local files optional, and preferred); in-use emphasis for files WPS opens and lock files |
| 0.15 | 2026-11-18 | Decided: shadows are WPS object files carrying their target's ID, not managed symlinks (Links rewritten) |
| 0.14 | 2026-11-18 | Decided: renaming an icon renames the file or folder (as in OS/2); `rename` request |
| 0.13 | 2026-11-18 | M5 done: events, menus and settings as data, opening programs, system programs, templates, file types, root sessions, sandboxed plug-in hosts; findings listed under M5 |
| 0.12 | 2026-11-18 | Decided: plug-ins never run in-process; plug-in hosts are sandboxed |
| 0.11 | 2026-11-18 | M4 done: shadows, trash, abstract objects as files, streaming listings, selections and jobs; findings listed under M4 |
| 0.10 | 2026-11-18 | Decided: trashed objects keep their ID and settings until the trash is emptied; shadows follow them |
| 0.9 | 2026-11-18 | M3 done: tracker and helper; per-folder marks on btrfs subvolumes; settled-duplicate rule; ID search for objects that leave view |
| 0.8 | 2026-11-18 | Decided: backup-name rule as an option, off by default; `wps-watchd` optional; duplicate IDs shared until either file changes (no timer) |
| 0.7 | 2026-11-18 | M2 done: service, client, persistence, overlays, volume sync; simplifications listed under M2 |
| 0.6 | 2026-11-18 | Decided: state travels with removable volumes in per-user volume databases, synchronised git-style with the main database; WPS identity per user; four new scenarios (25a–25d) |
| 0.5 | 2026-11-18 | M1 first version: runtime, loop, `wpoc`, seven classes, tests; `.wpi` format decided |
| 0.4 | 2026-11-18 | M0 results folded in: displacement rule for saves that rename the original away (vim); save detection section; replaced inode identified by its handle; per-user filtering rules validated by the prototype (register by descriptor, re-check on folder changes, hidden rename targets, overflow notices for slow services); FAT handles valid within a mount; rsync note; M0 marked done |
| 0.3 | 2026-11-18 | Added Multiple logged-in users: services of different users never talk to each other; abstract objects are stored as files (`.desktop`, symlinks, `.wpobj`) so other users can see them; shadow records stored on their folder so any user can maintain them; simultaneous-operation rules with no-replace file operations; shared and system programs, always launched as the launching user, with confirmation for programs owned by others; per-user fairness through systemd slices; root sessions. Nine new test scenarios; milestones extended to 3.5–4 months |
| 0.2 | 2026-11-18 | Folded in the [external review](reviews/2026-11-18-gemini-review.md): lazy ID assignment; replacement decided by incoming identity instead of timing; cross-filesystem moves matched by ID with no time window; all-or-nothing layout storage; hard-link notes only for objects with user settings; excluded paths. Added design principles; responsiveness rules against a single-queue stall (loop budget, deferred results, three queues, plug-in hosts); concurrency and multi-user model (per-user service, change counters, locked merge, overlays, per-user layout, per-user event filtering); scale section (streaming folder listing, selections, jobs with journal, reflink copies); API messages for selections, jobs and viewports; 16 new test scenarios; revised milestones; deferred items |
| 0.1 | 2026-11-17 | First version |
