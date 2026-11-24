# M0 results: kernel behaviour behind the object core design

Run 2026-11-18 on Fedora, kernel 7.3.3, against loopback ext4, btrfs and FAT filesystems, in two parts:

- **Kernel behaviour and real programs** (`m0.py`): 25 scenarios, **20 PASS, 1 FINDING, 4 INFO**.
- **Per-user event filtering** (`filter.py`, prototype helper `watchd.c`): 11 scenarios with two users, **all PASS**.

The design's core assumptions hold. One real gap was found, vim's default way of saving a file, and design v0.4 adds a rule for it. Full output of the last runs is in `build/m0/run.txt` and `build/m0/filter-run.txt` (not committed); rerun with the steps at the end.

## Summary

| # | Assumption | Verdict | What happened |
|---|---|---|---|
| 1 | `mv` within a filesystem is one `FAN_RENAME` carrying the moved file's handle | PASS | One event, handle matches, ID kept; event arrived about 2 ms after `mv` returned |
| 2 | Atomic save, temp file in the same folder | PASS | `FAN_RENAME` carries the incoming file's handle; it has no ID, so the rule can decide |
| 3 | Atomic save, temp file in another folder | PASS | Same as 2 |
| 4 | vim saving a file | **FINDING** | Default mode renames the original away and creates a new file. See below |
| 5 | `sed -i` | PASS | Temp file renamed over the original; the rule applies |
| 6 | Which tools keep `user.osd.id` | PASS | `mv` across filesystems keeps it. Table below |
| 7 | `mv` across filesystems: ID on the copy before the original is deleted | PASS | The ID is written to the copy (`FAN_ATTRIB`) before the source `FAN_DELETE`, so matching by ID needs no time window |
| 8 | `user.*` attributes refused on symlinks | PASS | `EPERM`, as assumed |
| 9 | `XATTR_CREATE` lets one of two simultaneous ID assignments win | PASS | 300 races, never two winners |
| 10 | The kernel decides who may write shared attributes | PASS | Needs write permission on the file. On a sticky folder, only its owner may set attributes on the folder itself. Setgid folders give new files the shared group |
| 11 | Handles change when an inode number is reused (ext4) | PASS | Inode reused on the first create; new handle differs; old one returns `ESTALE` |
| 12 | `open_by_handle_at` needs privilege | PASS | Refused for a user, works for root, so the helper must do lookups |
| 13 | `RENAME_NOREPLACE` refuses to overwrite | PASS | ext4, btrfs and FAT all return `EEXIST` |
| 14 | FAT handles | INFO | Marks work; handles survive moves within a mount but go stale after a remount |
| 15 | btrfs `find-new` lists changes since a generation | PASS | Works for the catch-up scan |
| 16 | A filesystem-wide watcher sees other users' private files | INFO | Root saw file names in another user's mode-700 folder. Per-user filtering in the helper is mandatory |
| 17 | Event flood | INFO | 50,000 creates in 2.2 s produced about 100,000 events with no overflow, with the default queue of 16,384 |
| 18 | Atomic save through one name of a hard link | INFO | The link splits; the surviving inode reports `FAN_ATTRIB` (link count dropped) and keeps the ID |
| 19 | GNOME Text Editor's save (GtkSourceView 5 FileSaver, run without a window) | PASS | Writes `.goutputstream-XXXXXX`, renames it over the original; rename-over rule applies |
| 20 | LibreOffice Writer saving an `.odt` (headless, through UNO `store()`) | PASS | Writes `lu*.tmp` in the same folder and renames it over the original; also creates and removes `.~lock.report.odt#` |
| 21 | `gio save` (GLib file replace, used by most GNOME apps) | PASS | Same pattern as 19 |
| 22 | Full vim (`vim-enhanced` 9.2, built with `+xattr`), three save modes | PASS | Default and `backupcopy=no` still rename the original away, but vim copies extended attributes to the new file, so the ID survives |
| 23 | gedit's save code (libgedit-gtksourceview, GtkSource 300) | PASS | Temp file renamed over the original |
| 24 | The real gedit 51 window, edited and saved through the accessibility bus | PASS | Same as 23 |
| 25 | The real GNOME Text Editor 51 window, edited and saved through the accessibility bus (its window's `page.save` action) | PASS | Same as 19 |

### Tools and `user.osd.id` (scenario 6)

| Tool | ID |
|---|---|
| `mv`, same filesystem | kept |
| `mv`, ext4 → btrfs | kept |
| `mv`, ext4 → FAT | lost (FAT has no extended attributes) |
| `cp` | lost |
| `cp -a`, `cp --preserve=xattr`, `cp -a` across filesystems | kept |
| `rsync -a` | **lost** |
| `rsync -aX` | kept |
| `tar` without `--xattrs` | lost |
| `tar --xattrs` | kept |

## The finding: how vim saves

vim (here the `vi` from vim-minimal) has three save modes:

| Mode | What vim does | Inode | ID |
|---|---|---|---|
| `backupcopy=yes` | Copies the original to `file~`, then overwrites the original in place | same | kept |
| `backupcopy=no` | Renames the original to `file~`, creates a new `file`, then deletes `file~` | new | lost |
| `backupcopy=auto` (the default) | Chose the same as `no` here | new | lost |

The event sequence in the default mode:

```
RENAME  file → file~          (the object, with its ID, moves to the backup name)
CREATE  file                  (a new inode, no ID)
CLOSE_WRITE file
DELETE  file~                 (the object is deleted)
```

The design's atomic-save rule only looks at a rename **onto** an existing name. Here the original is renamed **away** and a new file is created. Under the current rules the object would follow `file~` and then be deleted. Its settings and shadows would be lost on every save in vim, the most common editor on Linux.

### Rule added in design v0.4

A sequence rule, with no timing:

1. When an object is renamed within the same folder, remember its former name as **displaced**.
2. If a file without an ID is then created at that former name, link the two as a candidate pair.
3. When the displaced object is deleted while the pair exists, its identity moves to the new file: same ID, settings copied across, shadows retargeted.
4. Any other change to the pair cancels it: the new file being removed or renamed, or the displaced object being renamed again or edited.

This covers vim's default behaviour, and editors that keep no backup. If the user keeps backups (`set backup`), `file~` is never deleted, and the object would stay with the backup. A second, optional rule could treat a rename to a typical backup name (`~` suffix, `.bak`, `.orig`) as displacement immediately. That is a design decision to make, because it is a name heuristic.

Also to check: full vim (`vim-enhanced`, not installed here) may copy extended attributes onto the new file. If so, the ID survives the save anyway, but the rule above is still needed for vim-minimal and other editors.

Full vim (scenario 22) renames the original away too, but it is built with `+xattr` and copies the attributes to the new file, so the ID survives without the rule. The rule is needed for vim-minimal (`vi`), for vim builds without `+xattr`, and for any other program that saves this way.

No other editor tested renames the original away. GNOME Text Editor, gedit (both through their save libraries and through their real windows), GLib's `gio save`, LibreOffice and `sed -i` all use rename-over (scenarios 5, 19–21, 23–25).

## Per-user filtering (`filter.py`)

`watchd.c` is a prototype of `osd-watchd`'s filtering. Two clients stand in for two users' services: A (UID 60001) and B (UID 60002), both in group 60000. They register folders by passing open descriptors.

| # | Scenario | Result |
|---|---|---|
| F1 | A creates a file in A's private folder | A gets the events; B gets nothing |
| F2 | B tries to register A's private folder | The kernel refuses B's open; nothing to register |
| F3 | A moves a file from the shared folder into A's private folder | B sees a rename to `<hidden>`; the new name never reaches B. A sees both sides |
| F4 | B registered a shared folder; A changes it to mode 0700 | B gets a `revoked` notice and no further events |
| F5 | B opened the folder before A revoked access, and registers that descriptor afterwards | Refused: the helper checks access itself, as B |
| F6 | A registration with no descriptor | Refused |
| F7 | B registers one file in a folder B did not register; A deletes it | B gets the deletion |
| F8 | Activity in a folder nobody registered | Nobody gets events |
| F9 | A creates 20,000 files in a private folder | A receives all 20,000 within 1 s; B receives nothing |
| F11 | A third client stops reading for 8 s while 60,000 files are created | The stalled client receives `{"overflow": 107548}` once it reads again; the healthy client receives all 60,000 events in 2.5 s |
| F10 | Over the whole run, did any name from A's private folder reach B? | No |

Two bugs in the prototype were found and fixed on the way. First, events were silently dropped for a slow client. Second, the overflow notice only went out with the next event, so it never arrived if the burst had already ended. The helper now polls for room on the client's socket and sends the notice as soon as there is space. Access re-checks stayed rare: 11 checks for about 160,000 events.

## Other implications for the design

- **The replaced file is reported by its own handle.** In scenarios 2, 3 and 5, the overwritten inode produced `FAN_DELETE_SELF` carrying its (now stale) handle. The service can identify the replaced object directly from the event, without looking up names.
- **Hard-link splits are visible in the events.** The surviving inode reports `FAN_ATTRIB` when its link count drops, so a split can be noted without extra lookups.
- **Backups with `rsync -a` lose IDs.** That is common in practice. Restoring files from such a backup gives new objects; settings come back only if the database copy can be matched. Documentation should recommend `rsync -aX`. Matching restored files by path and size could be a later improvement.
- **FAT is slightly better than assumed.** Handles stay valid across moves while the volume stays mounted, so moves on FAT can be tracked by handle during a session. Only after a remount is the design's path-based fallback needed.
- **The event queue held up** under 50,000 creates with a prompt reader. The overflow path stays in the design. The helper should still read in large batches and do no blocking work per event.
- **Per-user filtering is not optional.** Confirmed by scenario 16.

## Not covered by M0

- Kate and VS Code are not installed here. LibreOffice was driven headless through UNO, not through its window.
- Registration at scale: the prototype looks up registrations linearly.
- Network filesystems.
- Btrfs subvolumes other than the top level.

## Running it again

```
make -C proto/m0 all     # builds build/m0/fanwatch and build/m0/fhtool
proto/m0/testbed.sh up    # loopback ext4, btrfs, FAT under /var/tmp/wps-m0-$USER (sudo)
proto/m0/m0.py            # kernel and program scenarios; or e.g. m0.py 4 7 for a subset
                          # (24 and 25 open real editor windows; they need a desktop session)
proto/m0/filter.py        # per-user filtering scenarios
proto/m0/testbed.sh down
```

`fanwatch` and `watchd` run as root through sudo. Scenario 20 starts a separate headless LibreOffice with its own profile under the test bed, so it does not touch a LibreOffice you have open. Test users are bare UIDs 60001 and 60002 (group 60000), run with `setpriv`; no accounts are created.
