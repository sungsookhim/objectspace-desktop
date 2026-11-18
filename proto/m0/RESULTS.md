# M0 results: kernel behaviour behind the object core design

Run 2026-11-18 on Fedora, kernel 7.3.3, against loopback ext4, btrfs and FAT filesystems. 18 scenarios: **13 PASS, 1 FINDING, 4 INFO**. The full output of the last run is in `build/m0/run.txt` (not committed); rerun with the steps at the end.

The design's core assumptions hold. One real gap was found: vim's default way of saving a file is not covered by the atomic-save rule.

## Summary

| # | Assumption | Verdict | What happened |
|---|---|---|---|
| 1 | `mv` within a filesystem is one `FAN_RENAME` carrying the moved file's handle | PASS | One event, handle matches, ID kept; event arrived about 2 ms after `mv` returned |
| 2 | Atomic save, temp file in the same folder | PASS | `FAN_RENAME` carries the incoming file's handle; it has no ID, so the rule can decide |
| 3 | Atomic save, temp file in another folder | PASS | Same as 2 |
| 4 | vim saving a file | **FINDING** | Default mode renames the original away and creates a new file. See below |
| 5 | `sed -i` | PASS | Temp file renamed over the original; the rule applies |
| 6 | Which tools keep `user.wps.id` | PASS | `mv` across filesystems keeps it. Table below |
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

### Tools and `user.wps.id` (scenario 6)

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

### Proposed rule (for design v0.4)

A sequence rule, with no timing:

1. When an object is renamed within the same folder, remember its former name as **displaced**.
2. If a file without an ID is then created at that former name, link the two as a candidate pair.
3. When the displaced object is deleted while the pair exists, its identity moves to the new file: same ID, settings copied across, shadows retargeted.
4. Any other change to the pair cancels it: the new file being removed or renamed, or the displaced object being renamed again or edited.

This covers vim's default behaviour, and editors that keep no backup. If the user keeps backups (`set backup`), `file~` is never deleted, and the object would stay with the backup. A second, optional rule could treat a rename to a typical backup name (`~` suffix, `.bak`, `.orig`) as displacement immediately. That is a design decision to make, because it is a name heuristic.

Also to check: full vim (`vim-enhanced`, not installed here) may copy extended attributes onto the new file. If so, the ID survives the save anyway, but the rule above is still needed for vim-minimal and other editors.

## Other implications for the design

- **The replaced file is reported by its own handle.** In scenarios 2, 3 and 5, the overwritten inode produced `FAN_DELETE_SELF` carrying its (now stale) handle. The service can identify the replaced object directly from the event, without looking up names.
- **Hard-link splits are visible in the events.** The surviving inode reports `FAN_ATTRIB` when its link count drops, so a split can be noted without extra lookups.
- **Backups with `rsync -a` lose IDs.** That is common in practice. Restoring files from such a backup gives new objects; settings come back only if the database copy can be matched. Documentation should recommend `rsync -aX`. Matching restored files by path and size could be a later improvement.
- **FAT is slightly better than assumed.** Handles stay valid across moves while the volume stays mounted, so moves on FAT can be tracked by handle during a session. Only after a remount is the design's path-based fallback needed.
- **The event queue held up** under 50,000 creates with a prompt reader. The overflow path stays in the design. The helper should still read in large batches and do no blocking work per event.
- **Per-user filtering is not optional.** Confirmed by scenario 16.

## Not covered by M0

- Per-user event filtering by passing open folder descriptors to the helper (design only so far; part of M3).
- Full vim, GUI editors (gedit, Kate, VS Code), LibreOffice.
- Network filesystems.
- Btrfs subvolumes other than the top level.

## Running it again

```
make -C proto/m0 all     # builds build/m0/fanwatch and build/m0/fhtool
proto/m0/testbed.sh up    # loopback ext4, btrfs, FAT under /var/tmp/wps-m0-$USER (sudo)
proto/m0/m0.py            # all scenarios; or e.g. m0.py 4 7 for a subset
proto/m0/testbed.sh down
```

`fanwatch` runs as root through sudo. Test users are bare UIDs 60001 and 60002 (group 60000), run with `setpriv`; no accounts are created.
