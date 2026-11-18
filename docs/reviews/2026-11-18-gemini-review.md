# Review of the Object Core Design v0.1 (2026-11-18)

An external review of [../object-core-design.md](../object-core-design.md) was done with Gemini. Below are its five findings as received, our assessment of each, and the design changes we accepted. The accepted changes are folded into design v0.2.

## Summary

| Finding | Assessment | Accepted change |
|---|---|---|
| A. Atomic save heuristic | Mechanism misread; the conclusion is half right | Decide by whether the incoming file already has an identity, not by timing |
| B. 5-second cross-filesystem window | **Valid: the real bug** | Match on the object ID that `mv` copies with the file; no timer |
| C. Layout spilled across extended attributes and SQLite | Valid, low impact | Never split: all in the attribute, or all in SQLite |
| D. Single-threaded loop and 100,000-file folders | Valid concern, wrong cause | Never create 100,000 objects; list folders in pages from worker threads |
| E. Hard-link split warnings | Overstated | Warn only for objects with settings the user made; excluded paths |

## A. Atomic save heuristic

**Finding.** Detection relies on a temp file appearing just before a `FAN_RENAME`. Slow writes, several concurrent temp files, or interleaved events could mismatch inodes and detach `user.wps.*` attributes.

**Assessment.** No timing or cross-event correlation is needed. `FAN_RENAME` is a single event carrying the source name, destination name and the moved file's handle, and the service already knows which object was at the destination. Interleaving cannot mismatch it.

The finding does expose a real flaw. `mv draft.txt final.txt` in the same folder is not a save, but the v0.1 rule would give `final.txt`'s identity to `draft.txt`. Editors that keep their temp files in a different folder were also excluded by v0.1.

**Accepted change.**
- Incoming inode has **no object ID** of its own (a fresh temp file): same object, new contents. Keep the ID and move the settings across. This works whatever folder the temp file came from.
- Incoming inode **has its own ID**: a real move that replaces another file. The replaced object is deleted; the moved one keeps its ID.
- New core rule, **lazy ID assignment**: a file gets an object ID only when WPS first stores state for it (a title, a position, a shadow), never when it is created. Temp files and build outputs are therefore never registered.

## B. Cross-filesystem move window

**Finding.** Pairing `FAN_CREATE` and `FAN_DELETE` within 5 seconds fails for large files; the object ID, layout and shadows are lost.

**Assessment.** Correct. A large move easily exceeds any fixed window. But `mv` across filesystems copies extended attributes, so the copy arrives carrying `user.wps.id`, which allows an exact match.

**Accepted change.**
1. A file appears carrying an ID that already exists: record it as a **pending duplicate**. Both copies still resolve by their own handles meanwhile.
2. The original is deleted: it was a **move**, and the new file keeps the ID, settings and shadows.
3. The original still exists after a grace period (proposed: 1 hour), or is changed: it was a **copy**, and the copy gets a new ID with its settings kept. This replaces the separate `cp -a` rule.
4. Name and size matching remains only as a fallback for tools that drop extended attributes and for destinations without them, such as FAT. That fallback is documented as best effort.

## C. Spilled layout consistency

**Finding.** Splitting folder layout between the attribute and SQLite risks inconsistency after a crash or concurrent change.

**Assessment.** Valid. Impact is low because layout is the least valuable state: the worst case is rearranged icons, never lost files or identity.

**Accepted change.** All-or-nothing storage. A folder's layout lives entirely in its extended attribute when it fits, otherwise entirely in SQLite, with the attribute holding only a marker and a version number. Switching writes SQLite first, then the attribute, so a crash in between leaves the old complete layout.

## D. Single-threaded event loop with very large folders

**Finding.** Running all methods on one thread will block the service, and therefore the GUI, for folders of 100,000 files.

**Assessment.** The GUI is a separate process, so drawing is never blocked, but it would wait for replies. The real risk is creating 100,000 full objects to show one folder, not the threading model.

**Accepted change.**
- Folders are listed as lightweight records (name, class, icon, position) read by worker threads from the directory listing and extended attributes, returned in pages (`query_content` already takes offset and count).
- Full objects are created only on demand: when opened, right-clicked or edited.
- Plug-in methods get a time limit; one that runs over is logged with the plug-in named. Asynchronous methods may come later.
- The benchmark becomes a test with a target: the first page of a 100,000-file folder within 100 ms.
- The single-threaded model for object methods is kept.

## E. Hard-link split warnings

**Finding.** Hard links are common in build caches and content-addressed stores; warning on every split would flood users with false positives.

**Assessment.** Overstated. Users do not arrange icons or create shadows inside pnpm stores, ccache, `/nix/store` or `.git`. With lazy ID assignment, those files never become WPS objects and cannot produce warnings.

**Accepted change.**
- A split is recorded only when the object has settings the user made.
- It is shown in the object's Properties, never as a pop-up.
- An **excluded paths** list, defaulting to `.git`, `node_modules`, `/nix/store` and common build directories.

## Additional issues raised in our assessment

- **Lazy ID assignment** (from A) is a core rule that A, B and E depend on; it belongs in the Object identity section.
- **Event volume during builds.** A parallel build produces floods of create, write and rename events. The helper drops events for excluded paths and for files without IDs before forwarding them.
- **Privacy in the helper.** A filesystem-wide fanotify mark sees every user's activity. The readability check must run in `wps-watchd` for every event before it is forwarded, and it needs its own test.

## Test scenario changes for v0.2

- Replace scenario 8 (`cp -a`) with: `cp -a` a file and wait out the grace period → two IDs; `mv` a 2 GB file across filesystems → ID kept.
- Add: `mv draft.txt final.txt` within one folder, where both have IDs → `final.txt`'s object deleted, `draft.txt` keeps its own ID.
- Add: an editor saving through a temp file in another folder → same object ID.
- Add: a parallel build inside an excluded path → no objects created, helper forwards nothing.
- Add: another user's file activity → never forwarded to this user's service.
- Change the benchmark to: first page of a 100,000-file folder within 100 ms.
