#!/usr/bin/env python3
"""M0: check the kernel behaviour the object core design relies on.

Runs each scenario against the loopback test bed (testbed.sh up), with
fanwatch recording fanotify events, and reports one verdict per scenario:

  PASS     the kernel behaves as the design assumes
  FINDING  it does not; the design needs a change
  INFO     an observation the design should take into account

usage: m0.py [scenario-number ...]      (default: all)
Writes build/m0/report.json and prints a summary.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from multiprocessing import Barrier, Process, Queue
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WORK = Path(os.environ.get("WORK", f"/var/tmp/wps-m0-{os.environ.get('USER', os.getlogin())}"))
MNT = {fs: WORK / "mnt" / fs for fs in ("ext4", "btrfs", "fat")}
BIN = ROOT / "build" / "m0"
LOG = BIN / "events.jsonl"
ID = "user.wps.id"

results = []
scenarios = []


def scenario(num, title):
    def reg(fn):
        scenarios.append((num, title, fn))
        return fn
    return reg


# ---------------------------------------------------------------- helpers

def run(cmd, user=None, check=False, input=None):
    """Run a command; as test user `user` (60001/60002) through setpriv."""
    if user:
        cmd = ["sudo", "setpriv", f"--reuid={user}", "--regid=60000",
               "--groups=60000", "--"] + cmd
    p = subprocess.run(cmd, capture_output=True, text=True, input=input)
    if check and p.returncode != 0:
        raise RuntimeError(f"{cmd}: {p.stderr.strip()}")
    return p


def handle(path):
    out = run([str(BIN / "fhtool"), "handle", str(path)]).stdout.split()
    return out[0] if out and out[0] != "error" else None


def inode(path):
    return os.lstat(path).st_ino


def set_id(path, value):
    os.setxattr(path, ID, value.encode(), os.XATTR_CREATE)


def get_id(path):
    try:
        return os.getxattr(path, ID).decode()
    except OSError:
        return None


def fresh_dir(fs, name):
    d = MNT[fs] / name
    run(["rm", "-rf", str(d)])
    d.mkdir(parents=True)
    return d


def write(path, data="hello x\n"):
    with open(path, "w") as f:
        f.write(data)


def now_ms():
    return int(time.time() * 1000)


class Events:
    """Reads fanwatch's log incrementally."""

    def __init__(self):
        self.offset = 0

    def _read(self):
        with open(LOG) as f:
            f.seek(self.offset)
            data = f.read()
            # Only consume complete lines.
            cut = data.rfind("\n") + 1
            self.offset += len(data[:cut].encode())
        return [json.loads(l) for l in data[:cut].splitlines() if l.strip()]

    def drain(self):
        time.sleep(0.3)
        self._read()

    def collect(self, quiet=0.5, limit=20.0):
        """Return events until none arrive for `quiet` seconds."""
        evs, last, start = [], time.time(), time.time()
        while time.time() - last < quiet and time.time() - start < limit:
            new = self._read()
            if new:
                evs += new
                last = time.time()
            time.sleep(0.05)
        return evs


EV = Events()


def has(e, kind):
    return kind in e.get("mask", "").split("|")


def fid(e):
    return e.get("fid", {}).get("h")


def summarize(evs, root):
    """Compact one-line descriptions of events, paths relative to root."""
    out = []
    for e in evs:
        if "mask" not in e:
            continue
        parts = [e["mask"]]
        for k in ("old", "new", "dir"):
            if k in e:
                parts.append(f"{k}={e[k]['name']}")
        if "fid" in e:
            p = e["fid"].get("path", "")
            parts.append("file=" + p.replace(str(root) + "/", ""))
            if "id" in e["fid"]:
                parts.append(f"id={e['fid']['id']}")
        out.append(" ".join(parts))
    return out


def result(num, title, verdict, detail):
    results.append({"num": num, "title": title, "verdict": verdict,
                    "detail": detail})


# -------------------------------------------------------------- scenarios

@scenario(1, "mv within one filesystem is one FAN_RENAME carrying the moved file's handle")
def s_rename():
    d = fresh_dir("ext4", "s1")
    (d / "sub").mkdir()
    f = d / "a.txt"
    write(f)
    set_id(f, "S1")
    h = handle(f)
    EV.drain()
    t0 = now_ms()
    run(["mv", str(f), str(d / "sub" / "b.txt")], check=True)
    evs = EV.collect()
    ren = [e for e in evs if has(e, "RENAME")]
    lines = summarize(evs, MNT["ext4"])
    ok = (len(ren) == 1 and fid(ren[0]) == h
          and ren[0]["old"]["name"] == "a.txt" and ren[0]["new"]["name"] == "b.txt"
          and get_id(d / "sub" / "b.txt") == "S1")
    lat = ren[0]["t"] - t0 if ren else None
    return ("PASS" if ok else "FINDING",
            [f"{len(ren)} RENAME event(s); handle matches: {bool(ren) and fid(ren[0]) == h}",
             f"ID kept on the moved file: {get_id(d / 'sub' / 'b.txt')}",
             f"event latency after mv returned: about {lat} ms (includes mv process start)",
             "events:"] + lines)


def rename_over(tmp_dir_name):
    d = fresh_dir("ext4", "s2" if tmp_dir_name is None else "s3")
    f = d / "a.txt"
    write(f)
    set_id(f, "VICTIM")
    hv = handle(f)
    tdir = d if tmp_dir_name is None else fresh_dir("ext4", tmp_dir_name)
    tmp = tdir / ".a.txt.tmp"
    write(tmp, "new contents\n")
    ht = handle(tmp)
    EV.drain()
    os.rename(tmp, f)
    evs = EV.collect()
    ren = [e for e in evs if has(e, "RENAME")]
    victim = [e for e in evs if fid(e) == hv]
    ok = (len(ren) == 1 and fid(ren[0]) == ht and ren[0]["new"]["name"] == "a.txt"
          and "id" not in ren[0].get("fid", {}))
    detail = [
        f"RENAME reports the incoming file's handle: {bool(ren) and fid(ren[0]) == ht}",
        f"incoming file carries no ID, so the rule 'no ID → same object, new contents' can decide: {get_id(f) is None}",
        f"events about the replaced inode itself: {[e['mask'] for e in victim] or 'none'}",
        "events:"] + summarize(evs, MNT["ext4"])
    return ("PASS" if ok else "FINDING", detail)


@scenario(2, "Atomic save, temp file in the same folder, renamed over the original")
def s_rename_over_same():
    return rename_over(None)


@scenario(3, "Atomic save, temp file in another folder, renamed over the original")
def s_rename_over_other():
    return rename_over("s3tmp")


def vi_save(d, mode, binary="vi"):
    f = d / f"{binary}-{mode}.txt"
    write(f, "line x\n")
    set_id(f, "VIM-" + mode)
    h_before, i_before = handle(f), inode(f)
    EV.drain()
    cmd = [binary, "-es", "-N", "-u", "NONE", "-i", "NONE"]
    if mode != "default":
        cmd += ["-c", f"set backupcopy={mode}"]
    cmd += ["-c", "%s/x/y/", "-c", "wq", str(f)]
    p = run(cmd)
    evs = EV.collect()
    same_inode = inode(f) == i_before
    return f, same_inode, get_id(f), h_before, evs, p.returncode


def vim_modes(dirname, binary):
    d = fresh_dir("ext4", dirname)
    detail, verdict = [], "PASS"
    for mode in ("default", "yes", "no"):
        f, same, idv, hb, evs, rc = vi_save(d, mode, binary)
        detail.append(f"backupcopy={mode}: exit {rc}; same inode: {same}; ID after save: {idv}")
        detail += ["    " + l for l in summarize(evs, MNT["ext4"])]
        if not same and idv is None:
            # The design must be able to tell this was a save.
            ren_over = any(has(e, "RENAME") and e["new"]["name"] == f.name for e in evs)
            ren_away = any(has(e, "RENAME") and e["old"]["name"] == f.name for e in evs)
            if ren_away and not ren_over:
                verdict = "FINDING"
                detail.append(f"    -> original renamed AWAY (to a backup name) and a new file created at {f.name}: "
                              "not covered by the rename-over rule; needs the displacement rule")
    return verdict, detail


@scenario(4, "vi (vim-minimal) saving a file that has a WPS ID, three backupcopy modes")
def s_vim():
    return vim_modes("s4", "vi")


@scenario(5, "sed -i on a file that has a WPS ID")
def s_sed():
    d = fresh_dir("ext4", "s5")
    f = d / "a.txt"
    write(f, "line x\n")
    set_id(f, "SED")
    i0 = inode(f)
    EV.drain()
    run(["sed", "-i", "s/x/y/", str(f)], check=True)
    evs = EV.collect()
    ren_over = [e for e in evs if has(e, "RENAME") and e["new"]["name"] == "a.txt"]
    ok = bool(ren_over) and get_id(f) is None
    return ("PASS" if ok or get_id(f) == "SED" else "FINDING",
            [f"same inode: {inode(f) == i0}; ID after: {get_id(f)}",
             f"temp file renamed over the original (rule applies): {bool(ren_over)}",
             "events:"] + summarize(evs, MNT["ext4"]))


@scenario(6, "Which tools keep user.wps.id")
def s_tools():
    e, b, ft = fresh_dir("ext4", "s6"), fresh_dir("btrfs", "s6"), fresh_dir("fat", "s6")
    rows = []

    def case(name, src_fs_dir, make_cmd, dest):
        src = src_fs_dir / f"src-{len(rows)}.txt"
        write(src)
        set_id(src, f"T{len(rows)}")
        p = run(make_cmd(src, dest), check=False)
        kept = get_id(dest) if dest.exists() else "(no file)"
        rows.append((name, kept is not None and kept != "(no file)", kept,
                     p.stderr.strip().splitlines()[-1] if p.stderr.strip() else ""))

    case("mv, same filesystem", e, lambda s, d: ["mv", str(s), str(d)], e / "mv-same")
    case("mv, ext4 → btrfs", e, lambda s, d: ["mv", str(s), str(d)], b / "mv-cross")
    case("mv, ext4 → FAT", e, lambda s, d: ["mv", str(s), str(d)], ft / "mv-fat")
    case("cp", e, lambda s, d: ["cp", str(s), str(d)], e / "cp")
    case("cp -a", e, lambda s, d: ["cp", "-a", str(s), str(d)], e / "cp-a")
    case("cp --preserve=xattr", e, lambda s, d: ["cp", "--preserve=xattr", str(s), str(d)], e / "cp-x")
    case("cp -a, ext4 → btrfs", e, lambda s, d: ["cp", "-a", str(s), str(d)], b / "cp-a-cross")
    case("rsync -a", e, lambda s, d: ["rsync", "-a", str(s), str(d)], e / "rsync-a")
    case("rsync -aX", e, lambda s, d: ["rsync", "-aX", str(s), str(d)], e / "rsync-aX")
    for flags, label in (([], "tar (no --xattrs)"), (["--xattrs", "--xattrs-include=user.*"], "tar --xattrs")):
        src = e / f"tarsrc-{len(rows)}.txt"
        write(src)
        set_id(src, f"T{len(rows)}")
        out = fresh_dir("ext4", f"s6tar{len(rows)}")
        subprocess.run(f"tar {' '.join(flags)} -C {e} -cf - {src.name} | tar {' '.join(flags)} -C {out} -xf -",
                       shell=True, capture_output=True)
        rows.append((label, get_id(out / src.name) is not None, get_id(out / src.name), ""))
    cross = next(r for r in rows if r[0] == "mv, ext4 → btrfs")
    verdict = "PASS" if cross[1] else "FINDING"
    return verdict, [f"{n:24} {'kept' if k else 'LOST':5} {v or ''} {('  stderr: ' + w) if w else ''}"
                     for n, k, v, w in rows]


@scenario(7, "mv across filesystems: does the copy carry the ID before the original is deleted?")
def s_cross_events():
    e, b = fresh_dir("ext4", "s7"), fresh_dir("btrfs", "s7")
    f = e / "big.bin"
    with open(f, "wb") as out:
        out.write(os.urandom(1 << 20) * 64)  # 64 MB
    set_id(f, "S7")
    hs = handle(f)
    EV.drain()
    t0 = now_ms()
    run(["mv", str(f), str(b / "big.bin")], check=True)
    t1 = now_ms()
    evs = EV.collect(quiet=1.0)
    dest = [x for x in evs if x.get("fid", {}).get("path", "").startswith(str(b))]
    src_del = [x for x in evs if has(x, "DELETE") and x.get("dir", {}).get("name") == "big.bin"
               and str(e) in x["dir"]["d"].get("path", "")]
    attrib = [x for x in dest if has(x, "ATTRIB")]
    create = [x for x in dest if has(x, "CREATE")]
    order_ok = bool(attrib and src_del and attrib[0]["t"] <= src_del[0]["t"])
    # Event read times are only approximate ordering; use log order too.
    idx = {id(x): i for i, x in enumerate(evs)}
    log_order_ok = bool(attrib and src_del and idx[id(attrib[0])] < idx[id(src_del[0])])
    return ("PASS" if log_order_ok and get_id(b / "big.bin") == "S7" else "FINDING",
            [f"mv took {t1 - t0} ms for 64 MB",
             f"ID on the destination afterwards: {get_id(b / 'big.bin')}",
             f"destination CREATE seen: {bool(create)}; ATTRIB seen: {len(attrib)}; source DELETE seen: {bool(src_del)}",
             f"destination ATTRIB (ID written) arrives before source DELETE: {log_order_ok}",
             "events:"] + summarize(evs, WORK / "mnt"))


@scenario(8, "user.* attributes are refused on symlinks")
def s_symlink_xattr():
    d = fresh_dir("ext4", "s8")
    write(d / "t")
    os.symlink(d / "t", d / "link")
    try:
        os.setxattr(d / "link", "user.x", b"1", follow_symlinks=False)
        return "FINDING", ["setxattr on the symlink succeeded"]
    except OSError as ex:
        return "PASS", [f"setxattr on the symlink: {ex.strerror} (errno {ex.errno})"]


def _racer(paths, barrier, q, tag):
    wins = 0
    for p in paths:
        barrier.wait()
        try:
            os.setxattr(p, ID, tag.encode(), os.XATTR_CREATE)
            wins += 1
        except OSError:
            pass
    q.put((tag, wins))


@scenario(9, "XATTR_CREATE lets exactly one of two simultaneous ID assignments win")
def s_xattr_race():
    d = fresh_dir("ext4", "s9")
    paths = []
    for i in range(300):
        p = d / f"f{i}"
        write(p)
        paths.append(str(p))
    barrier, q = Barrier(2), Queue()
    procs = [Process(target=_racer, args=(paths, barrier, q, t)) for t in ("A", "B")]
    for p in procs:
        p.start()
    for p in procs:
        p.join()
    wins = dict(q.get() for _ in procs)
    single = sum(1 for p in paths if get_id(p) in ("A", "B"))
    ok = wins["A"] + wins["B"] == len(paths) and single == len(paths)
    return ("PASS" if ok else "FINDING",
            [f"300 files, two processes released together for each: A won {wins['A']}, B won {wins['B']}",
             f"files with exactly one ID: {single}; double wins: {wins['A'] + wins['B'] - len(paths)}"])


@scenario(10, "Who may write user.* attributes in shared folders")
def s_perms():
    root = MNT["ext4"] / "shared"
    detail = []
    run(["sudo", "rm", "-rf", str(root / "s10")])
    run(["mkdir", str(root / "s10")], user=60001, check=True)
    base = root / "s10"
    py = "import os,sys; p=sys.argv[1]\ntry:\n os.setxattr(p,'user.wps.t',b'1'); print('ok')\nexcept OSError as e: print(e.strerror)"
    for mode in ("0644", "0664"):
        f = base / f"file{mode}"
        run(["sh", "-c", f"echo x > {f} && chmod {mode} {f}"], user=60001, check=True)
        r = run(["python3", "-c", py, str(f)], user=60002).stdout.strip()
        detail.append(f"user 60002 sets an attribute on 60001's file, mode {mode}, same group: {r}")
    grp = os.stat(base / "file0644").st_gid
    detail.append(f"new file in the setgid shared folder gets group {grp} (expected 60000)")
    sticky = base / "sticky"
    run(["sh", "-c", f"mkdir {sticky} && chmod 1777 {sticky} && echo x > {sticky}/f && chmod 666 {sticky}/f"],
        user=60001, check=True)
    r1 = run(["python3", "-c", py, str(sticky)], user=60002).stdout.strip()
    r2 = run(["python3", "-c", py, str(sticky / "f")], user=60002).stdout.strip()
    detail.append(f"user 60002 sets an attribute on 60001's sticky world-writable FOLDER: {r1}")
    detail.append(f"user 60002 sets an attribute on 60001's mode-666 file inside it: {r2}")
    ok = detail[0].endswith("Permission denied") and detail[1].endswith("ok") and grp == 60000
    return ("PASS" if ok else "FINDING", detail)


@scenario(11, "File handles change when an inode number is reused (ext4)")
def s_reuse():
    d = fresh_dir("ext4", "s11")
    f = d / "orig"
    write(f)
    h1, i1 = handle(f), inode(f)
    os.unlink(f)
    for n in range(2000):
        g = d / f"n{n}"
        write(g)
        if inode(g) == i1:
            h2 = handle(g)
            old = run(["sudo", str(BIN / "fhtool"), "open", str(MNT["ext4"]), h1]).stdout.strip()
            ok = h1 != h2 and old.startswith("error")
            return ("PASS" if ok else "FINDING",
                    [f"inode {i1} reused after {n + 1} creates",
                     f"old handle {h1}", f"new handle {h2}",
                     f"opening the old handle: {old}"])
    return "INFO", [f"inode {i1} not reused within 2000 creates; reuse not observed"]


@scenario(12, "open_by_handle_at needs privilege")
def s_obh_priv():
    d = fresh_dir("ext4", "s12")
    write(d / "f")
    h = handle(d / "f")
    me = run([str(BIN / "fhtool"), "open", str(MNT["ext4"]), h]).stdout.strip()
    su = run(["sudo", str(BIN / "fhtool"), "open", str(MNT["ext4"]), h]).stdout.strip()
    ok = me.startswith("error") and su.startswith("ok")
    return ("PASS" if ok else "FINDING", [f"as user: {me}", f"as root: {su}"])


@scenario(13, "RENAME_NOREPLACE refuses to overwrite on each filesystem")
def s_noreplace():
    detail, ok = [], True
    for fs in ("ext4", "btrfs", "fat"):
        d = fresh_dir(fs, "s13")
        write(d / "a")
        write(d / "b")
        r = run([str(BIN / "fhtool"), "rename-noreplace", str(d / "a"), str(d / "b")]).stdout.strip()
        detail.append(f"{fs:6} rename a → existing b: {r}")
        ok &= "File exists" in r
    return ("PASS" if ok else "FINDING", detail)


@scenario(14, "FAT: fanotify filesystem marks and file handles")
def s_fat():
    marks = []
    with open(LOG) as f:
        for line in f:
            j = json.loads(line)
            if "mark" in j:
                marks.append(j)
    fat = next((m for m in marks if m["mark"].endswith("/fat")), {})
    d = fresh_dir("fat", "s14")
    (d / "sub").mkdir()
    write(d / "f")
    h = handle(d / "f")
    os.rename(d / "f", d / "sub" / "g")
    h2 = handle(d / "sub" / "g")
    reopen = run(["sudo", str(BIN / "fhtool"), "open", str(MNT["fat"]), h]).stdout.strip()
    return ("INFO",
            [f"fanotify filesystem mark on FAT: {'ok' if fat.get('ok') else fat.get('error')}",
             f"name_to_handle_at on FAT: {h or 'refused'}",
             f"handle after moving the file to another folder: {h2} (same: {h == h2})",
             f"opening the old handle after the move: {reopen}",
             "(the design already treats FAT as path-keyed with weaker tracking)"])


@scenario(15, "btrfs find-new lists files changed since a generation")
def s_findnew():
    m = str(MNT["btrfs"])
    out = run(["sudo", "btrfs", "subvolume", "find-new", m, "99999999"]).stdout
    gen = out.strip().split()[-1]
    d = fresh_dir("btrfs", "s15")
    write(d / "changed.txt")
    run(["sync"])
    out2 = run(["sudo", "btrfs", "subvolume", "find-new", m, gen]).stdout
    ok = "changed.txt" in out2
    return ("PASS" if ok else "FINDING",
            [f"generation before: {gen}", f"listed after a change: {'changed.txt' if ok else 'not listed'}",
             f"output lines: {len(out2.splitlines())}"])


@scenario(16, "A filesystem-wide watcher sees other users' private activity")
def s_privacy():
    p = MNT["ext4"] / "private60001"
    EV.drain()
    run(["sh", "-c", f"echo secret > {p}/salary-2027.txt && mv {p}/salary-2027.txt {p}/salary-final.txt"],
        user=60001, check=True)
    evs = EV.collect()
    names = sorted({e[k]["name"] for e in evs for k in ("dir", "old", "new") if k in e})
    seen = any("salary" in n for n in names)
    return ("INFO",
            [f"root watcher saw names from user 60001's mode-700 folder: {seen} {names}",
             f"event pids reported: {sorted({e['pid'] for e in evs})}",
             "→ the helper must filter per user before forwarding (design: Concurrency, per-user filtering)"])


@scenario(17, "Event flood: 50,000 file creates against the default queue")
def s_flood():
    d = fresh_dir("ext4", "s17")
    EV.drain()
    t0 = time.time()
    for i in range(50000):
        open(d / f"f{i:05}", "w").close()
    t1 = time.time()
    evs = EV.collect(quiet=1.5, limit=120)
    creates = sum(1 for e in evs if has(e, "CREATE"))
    closes = sum(1 for e in evs if has(e, "CLOSE_WRITE"))
    overflow = sum(1 for e in evs if has(e, "Q_OVERFLOW"))
    qmax = Path("/proc/sys/fs/fanotify/max_queued_events").read_text().strip()
    run(["rm", "-rf", str(d)])
    EV.collect(quiet=1.5, limit=120)  # swallow the delete events
    return ("INFO",
            [f"created 50,000 files in {t1 - t0:.1f} s",
             f"events received: {len(evs)} (CREATE {creates}, CLOSE_WRITE {closes}), overflow events: {overflow}",
             f"kernel queue limit max_queued_events = {qmax}",
             "→ overflow means a rescan; whether it happens depends on how fast the reader drains"])


@scenario(18, "Atomic save through one name of a hard-linked file")
def s_hardlink():
    d = fresh_dir("ext4", "s18")
    a = d / "a.txt"
    write(a)
    set_id(a, "HL")
    os.link(a, d / "b.txt")
    ha = handle(a)
    tmp = d / ".a.tmp"
    write(tmp, "changed\n")
    EV.drain()
    os.rename(tmp, a)
    evs = EV.collect()
    victim_events = [e["mask"] for e in evs if fid(e) == ha]
    return ("INFO",
            [f"after the save: a.txt ID {get_id(a)}, b.txt ID {get_id(d / 'b.txt')}, "
             f"b.txt links now {os.stat(d / 'b.txt').st_nlink}",
             f"events about the original inode: {victim_events or 'none'}",
             "→ the split is only visible by knowing the replaced inode had other links (from the database)",
             "events:"] + summarize(evs, MNT["ext4"]))


def save_pattern(evs, name):
    if any(has(e, "RENAME") and e["new"]["name"] == name for e in evs):
        return "temp file renamed over the original"
    if any(has(e, "RENAME") and e["old"]["name"] == name for e in evs):
        return "original renamed away, new file created"
    if any(has(e, "CREATE") and e.get("dir", {}).get("name") == name for e in evs):
        return "original deleted, new file created"
    return "written in place"


def editor_save(fs, name, content, argv):
    """Run a save script that prints 'loaded', waits for a line, then saves."""
    d = fresh_dir(fs, name)
    f = d / content[0]
    if content[1] is not None:
        write(f, content[1])
    set_id(f, name.upper())
    i0 = inode(f)
    p = subprocess.Popen(argv(f), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True)
    first = p.stdout.readline().strip()
    if first != "loaded":
        p.kill()
        return "ERROR", [f"save script did not load the file: {first!r}"]
    EV.drain()
    p.stdin.write("\n")
    p.stdin.flush()
    done = p.stdout.readline().strip()
    evs = EV.collect(quiet=1.0)
    p.wait(timeout=60)
    if done != "saved":
        return "ERROR", [f"the file was not saved: {done}"]
    same = inode(f) == i0
    idv = get_id(f)
    pattern = save_pattern(evs, f.name)
    covered = idv is not None or pattern == "temp file renamed over the original"
    detail = [f"save: {done}; same inode: {same}; ID after save: {idv}",
              f"save pattern: {pattern}",
              "events:"] + summarize(evs, MNT[fs])
    return ("PASS" if covered else "FINDING"), detail


@scenario(19, "GNOME Text Editor's save code (GtkSourceView 5 FileSaver)")
def s_gtksource():
    return editor_save("ext4", "s19", ("notes.txt", "line x\n"),
                       lambda f: ["python3", str(HERE / "gtksource_save.py"), str(f)])


@scenario(20, "LibreOffice Writer saving an .odt document")
def s_libreoffice():
    profile = WORK / "lo-profile"
    d = fresh_dir("ext4", "s20")
    write(d / "report.txt", "Quarterly report\n")
    run(["soffice", "--headless", f"-env:UserInstallation=file://{profile}",
         "--convert-to", "odt", "--outdir", str(d), str(d / "report.txt")], check=True)
    (d / "report.txt").unlink()
    f = d / "report.odt"
    set_id(f, "S20")
    i0 = inode(f)
    p = subprocess.Popen(["python3", str(HERE / "lo_save.py"), str(f), str(profile)],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    first = p.stdout.readline().strip()
    if first != "loaded":
        p.kill()
        return "ERROR", [f"LibreOffice did not load the document: {first!r}"]
    EV.drain()
    p.stdin.write("\n")
    p.stdin.flush()
    done = p.stdout.readline().strip()
    evs = EV.collect(quiet=1.5)
    p.wait(timeout=90)
    idv = get_id(f)
    pattern = save_pattern(evs, f.name)
    covered = idv is not None or pattern == "temp file renamed over the original"
    return ("PASS" if covered else "FINDING",
            [f"save: {done}; same inode: {inode(f) == i0}; ID after save: {idv}",
             f"save pattern: {pattern}",
             "events (lock files included):"] + summarize(evs, MNT["ext4"]))


@scenario(21, "gio save (GLib's file replace, used by most GNOME apps)")
def s_gio():
    d = fresh_dir("ext4", "s21")
    f = d / "a.txt"
    write(f, "line x\n")
    set_id(f, "S21")
    i0 = inode(f)
    EV.drain()
    run(["gio", "save", str(f)], input="replaced\n", check=True)
    evs = EV.collect()
    idv = get_id(f)
    pattern = save_pattern(evs, f.name)
    covered = idv is not None or pattern == "temp file renamed over the original"
    return ("PASS" if covered else "FINDING",
            [f"same inode: {inode(f) == i0}; ID after save: {idv}", f"save pattern: {pattern}",
             "events:"] + summarize(evs, MNT["ext4"]))


@scenario(22, "Full vim (vim-enhanced, built with +xattr), three backupcopy modes")
def s_vim_full():
    if not shutil.which("vim"):
        return "INFO", ["vim-enhanced is not installed"]
    return vim_modes("s22", "vim")


@scenario(23, "gedit's save code (libgedit-gtksourceview, GtkSource 300)")
def s_gedit_lib():
    return editor_save("ext4", "s23", ("notes.txt", "line x\n"),
                       lambda f: ["python3", str(HERE / "gtksource_save.py"), str(f), "300"])


def gui_editor(num, app):
    if not (os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY")) or not shutil.which(app):
        return "INFO", [f"skipped: needs a desktop session and {app}"]
    return editor_save("ext4", f"s{num}", ("notes.txt", "line x\n"),
                       lambda f: ["python3", str(HERE / "gui_save.py"), app, str(f)])


@scenario(24, "The real gedit window, edited and saved through the accessibility bus")
def s_gedit_gui():
    return gui_editor(24, "gedit")


@scenario(25, "The real GNOME Text Editor window, edited and saved through the accessibility bus")
def s_gte_gui():
    return gui_editor(25, "gnome-text-editor")


# ----------------------------------------------------------------- driver

def main():
    want = {int(a) for a in sys.argv[1:]}
    for fs, m in MNT.items():
        if not os.path.ismount(m):
            sys.exit(f"{m} is not mounted; run proto/m0/testbed.sh up first")
    BIN.mkdir(parents=True, exist_ok=True)
    if LOG.exists():
        LOG.unlink()
    watcher = subprocess.Popen(["sudo", str(BIN / "fanwatch"), "-o", str(LOG)] +
                               [str(m) for m in MNT.values()])
    for _ in range(100):
        if LOG.exists() and '"ready"' in LOG.read_text():
            break
        time.sleep(0.05)
    EV.drain()
    try:
        for num, title, fn in sorted(scenarios):
            if want and num not in want:
                continue
            try:
                verdict, detail = fn()
            except Exception as ex:  # report and continue
                verdict, detail = "ERROR", [repr(ex)]
            result(num, title, verdict, detail)
            print(f"[{verdict:7}] {num:2}. {title}")
            for line in detail:
                print("            " + line)
    finally:
        run(["sudo", "pkill", "-TERM", "-x", "fanwatch"])
        watcher.wait(timeout=10)
    (BIN / "report.json").write_text(json.dumps(results, indent=1))
    counts = {}
    for r in results:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("\nsummary:", ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))


if __name__ == "__main__":
    main()
