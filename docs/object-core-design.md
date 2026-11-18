# WPS for Linux: Object Core Design

Version 0.3 · 2026-11-18 · Changes from earlier versions are listed in the [change log](#change-log) at the end.

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
- Built-in classes stay in-process for speed. A plug-in can request in-process loading only if the user marks it trusted.

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

FAT, exFAT and some network filesystems either cannot produce file handles or produce ones that do not survive a remount. Objects there are keyed by volume UUID plus path, and moves are tracked only while the service watches (see Change tracking). This is weaker, and the GUI should say so in the object's settings.

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
- **Events are filtered per user.** One `wps-watchd` serves all users. Each user's service registers the directories it tracks by passing open directory descriptors over its socket. Because the service opened them, the kernel has already checked access. The helper forwards only events under directories that user registered, and performs any handle-to-path lookup with the user's filesystem credentials (`setfsuid` in a worker thread).
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

A WPS shadow of a file or folder is a **managed symlink**: a real symlink on disk that the service rewrites whenever its target moves. Every Linux program can follow it, unlike OS/2 shadows, which only WPS could see. Symlinks and hard links made by other tools keep their Linux meaning.

| Kind | On disk | When the target moves | When the target is deleted | WPS shows it as |
| --- | --- | --- | --- | --- |
| Shadow of a file or folder | A symlink + a record in its folder's `user.wps.shadows` | The service rewrites the symlink | The service removes the shadow | Shadow (italic title, as in OS/2) |
| Shadow of an abstract object | A managed symlink to the object's file | The service rewrites the symlink | The service removes the shadow | Shadow |
| Symlink made by the user (`ln -s`) | A symlink only | Left alone; may dangle | Left alone | Link, with a badge; "Track target" turns it into a shadow |
| Hard link (`ln`) | Another name for the same inode | Not affected | The file lives while any name remains | The same object, shown in each folder |

### Managed symlinks

- Linux refuses `user.*` extended attributes on symlinks, so a shadow's record (its target's object ID) is stored on the folder that holds it, in `user.wps.shadows`. Any user who can write that folder can therefore maintain it (see Multiple logged-in users). On filesystems without extended attributes, the record goes to the creating user's database.
- The symlink holds an **absolute** path, so moving the shadow itself does not break it.
- When a target is renamed, the service rewrites every symlink pointing to that object ID. The rewrite is atomic: write a new symlink beside the old one, then rename it over the old one. When a whole folder moves, shadows pointing into it are found through an index on target path prefix and rewritten as a job.
- If the target is on a volume that is not mounted, the symlink is left as it is, and WPS shows the shadow as unavailable.
- Shadows of abstract objects point to the object's file, so they are symlinks like any other shadow.
- A user can create a managed shadow only in a folder they can write; the kernel enforces this.

### Symlinks made by the user

A user-made symlink may deliberately point at a path, such as whatever is mounted at `/media/backup`. WPS therefore never rewrites it unless the user chooses "Track target". That records it in `shadows` and makes it managed from then on.

### Hard links

- All names of a hard-linked file share one inode, so they share one file handle, one set of extended attributes and **one object ID**. That is correct: they are one file.
- Per-folder state (position, sort order) lives on each folder, so each name can sit at a different place.
- When one name is replaced by an atomic save, it gets a new inode and the link silently splits. If the object has settings the user made, the split is noted in its Properties. It is never shown as a pop-up, and files without IDs (build caches, package stores) are never reported.

### Safety rules

- Never follow symlinks in tree walks, scans, recursive copies or folder size calculations. Use `O_NOFOLLOW` and `AT_SYMLINK_NOFOLLOW`, and treat a symlink as a leaf object. A symlink to a parent directory would otherwise loop.
- Deleting a shadow deletes the symlink, never the target.
- Removing shadows when a target is deleted applies only to managed symlinks.

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
| `mv a b` in one folder, where both already have IDs | `FAN_RENAME` onto a name with an object; the incoming inode has its own ID | A real replacement: `b`'s object is deleted; `a`'s object keeps its ID |
| `mv` across filesystems (copy, then delete) | `FAN_CREATE` of a file carrying an existing `user.wps.id`, later `FAN_DELETE` of the original | Pending duplicate until the original is deleted: then a move, and the ID, settings and shadows carry over. No time window |
| `cp -a` or a restore from backup | `FAN_CREATE` carrying an existing ID; the original stays | After a 1-hour grace period, or when either copy is changed: a copy, and it gets a new ID with its settings kept |
| A tool that drops extended attributes (moving to FAT, some file managers) | `FAN_CREATE` with no ID, `FAN_DELETE` of an object elsewhere | Best effort: pair by name and size, and say so in the object's settings |
| `cp` without `-a` | `FAN_CREATE`, a new handle with no ID | Nothing is registered until WPS stores something for it |
| `rm` | `FAN_DELETE_SELF` once the last link is gone | Remove the object and its managed shadows; keep its row for 7 days so an undo or a restore can reclaim the ID |
| Hard link split by an atomic save | Rename onto a name whose inode had more than one link | Keep both; note it in Properties only if the object has user settings |
| Volume unplugged or unmounted | Unmount, no deletes | Mark its objects unavailable; never delete them |
| Volume changed on another machine | Nothing while away | Catch-up scan when it is mounted again |
| A filesystem without extended attributes | Setting the attribute fails with `ENOTSUP` | Store state in SQLite by volume UUID and path |
| Extended attributes stripped (by `tar` without `--xattrs`, rsync without `-X`, some editors) | `FAN_ATTRIB`, or found by the scan | Restore from the database copy if the handle still matches |
| Two writers change one object at once | Both write under `flock`; or `FAN_ATTRIB` from another user | Keys merged; a conflicting client write is refused with the current values |

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
| 5 | Edit the target with `vim`, `sed -i`, and an editor that writes its temp file in another folder | Same object ID; settings kept; the shadow still works |
| 6 | `mv` a 2 GB target from ext4 to btrfs | The shadow follows; the object ID is kept |
| 7 | `rm` the target | The managed shadow is removed; a user-made symlink is left |
| 8 | `cp -a` a file and wait out the grace period | Both exist, with different object IDs |
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

1. **M0: Tracking prototype (3–5 days).** A standalone C program using `FAN_RENAME` and `name_to_handle_at()` that prints old path, new path and handle while `mv`, `vim` and a cross-filesystem `mv` run in another terminal. It also confirms that `mv` across filesystems keeps `user.*` attributes. Validates the kernel behaviour the design relies on before anything is built on it.
2. **M1: Runtime and main loop (2–3 weeks).** `libwpo`, `wpoc`, the seven classes as shells, deferred results, the three-queue loop, the watchdog and the worker pool. Unit tests pass.
3. **M2: Persistence, identity and concurrency (2–3 weeks).** Extended attributes with locking and merge, overlays, lazy IDs, the database writer, `wpsd`, `wpsctl`. Scenarios 1, 2, 10 and 21–25 pass.
4. **M3: Change tracking (2–3 weeks).** `wps-watchd` with per-user filtering and excluded paths, inotify fallback, the replacement and move rules, catch-up scan. Scenarios 4, 5, 7–9, 11, 13–16, 26, 27, 33 and 36 pass.
5. **M4: Scale, shadows, object files and jobs (3–4 weeks).** Streaming folder listing, selections, the job manager with journal and logout handling, managed symlinks with folder records, `.wpobj` files, no-replace operations, unavailable volumes. Scenarios 3, 6, 12, 28–32, 34, 35, 38, 39 and 40 pass.
6. **M5: API, programs and plug-in host (2–3 weeks).** Subscriptions and events, menus and settings as data, `wps-plughost`, `.desktop` programs with launch confirmation, system object folders, root sessions, MIME classes, templates. Scenarios 17–20, 37 and 41 pass; all 41 now pass. GUI design starts from here.

## Deferred: decided in principle, details later

These follow from the principles above but need not be designed in full before M0–M2. Each is revisited at the milestone named.

- [ ] **Plug-in host protocol and lifecycle** (M5): how hosts are grouped, how objects are proxied, the security boundary for untrusted plug-ins.
- [ ] **Helper registration at scale** (M3): passing a descriptor per registered directory may be too many for very large trees; consider registering subtree roots and checking access per event.
- [ ] **Events for moves into directories a user did not register** (M3): what a user's service may learn about where its object went.
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

- [ ] Should the class interface files (`.wpi`) be a custom format, or a subset of an existing IDL to reuse its tools?
- [ ] Should per-user overlays and layouts for a removable volume be stored on that volume, so they travel with a USB disk?
- [ ] Should `wps-watchd` be required, or optional with a clearly weaker mode? Distributions may hesitate to ship a root service.
- [ ] How should objects inside the trash behave: kept with their IDs until emptied, or treated as deleted?
- [ ] Is 1 hour the right grace period for deciding that a duplicate ID is a copy rather than a move?
- [ ] Should plug-ins ever run in-process, even when trusted, or is the speed gain not worth the risk?

## Change log

| Version | Date | Changes |
| --- | --- | --- |
| 0.3 | 2026-11-18 | Added Multiple logged-in users: services of different users never talk to each other; abstract objects are stored as files (`.desktop`, symlinks, `.wpobj`) so other users can see them; shadow records stored on their folder so any user can maintain them; simultaneous-operation rules with no-replace file operations; shared and system programs, always launched as the launching user, with confirmation for programs owned by others; per-user fairness through systemd slices; root sessions. Nine new test scenarios; milestones extended to 3.5–4 months |
| 0.2 | 2026-11-18 | Folded in the [external review](reviews/2026-11-18-gemini-review.md): lazy ID assignment; replacement decided by incoming identity instead of timing; cross-filesystem moves matched by ID with no time window; all-or-nothing layout storage; hard-link notes only for objects with user settings; excluded paths. Added design principles; responsiveness rules against a single-queue stall (loop budget, deferred results, three queues, plug-in hosts); concurrency and multi-user model (per-user service, change counters, locked merge, overlays, per-user layout, per-user event filtering); scale section (streaming folder listing, selections, jobs with journal, reflink copies); API messages for selections, jobs and viewports; 16 new test scenarios; revised milestones; deferred items |
| 0.1 | 2026-11-17 | First version |
