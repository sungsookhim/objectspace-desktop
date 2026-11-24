#!/usr/bin/env python3
"""M4 scenarios: shadows, trash, abstract objects as files, scale, jobs.

usage: tests/m4/m4.py [--mode inotify|watchd] [scenario ...]
Needs the test bed (proto/m0/testbed.sh up); uses sudo.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
os.environ.setdefault("OSD_SUITE", "m4")
import harness  # noqa: E402
from harness import *  # noqa: E402,F401,F403


def wait_for(fn, timeout=5.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


def obj(svc, target):
    r = svc.ctl("resolve", target)
    return r.get("result", {}) if r.get("ok") else {}


def tracked(svc, path, title):
    svc.call(path, "osdSetTitle", title)
    svc.ctl("flush")
    return obj(svc, path).get("id")


def link_of(path):
    """Where a shadow points: a shadow file's TargetPath (shadows are WPS
    object files, decided 2026-11-18), or a plain symlink's target."""
    try:
        if os.path.islink(path):
            return os.readlink(path)
        for line in Path(path).read_text().splitlines():
            if line.startswith("TargetPath="):
                return line[len("TargetPath="):]
    except OSError:
        pass
    return None


# ------------------------------------------------------------------ shadows

def keep_files(svc, folder):
    """Shadow files are for folders set to keep objects as files (shadows are
    central by default, decided 2026-11-18); these scenarios test the files."""
    svc.call(folder, "osdSetLocalObjects", "true")
    svc.ctl("flush")

@scenario(3, "A shadow follows its target to another folder, and stays a shadow when it is moved itself")
def s3():
    me = Service("me").start()
    d = fresh("ext4", "s3")
    for sub in ("docs", "desk", "archive"):
        (d / sub).mkdir()
    (d / "docs" / "report.txt").write_text("quarterly numbers\n")
    tracked(me, d / "docs" / "report.txt", "Report")
    keep_files(me, d / "desk")
    r = me.ctl("shadow", d / "docs" / "report.txt", d / "desk")
    link = Path(r.get("result", ""))
    os.rename(d / "docs" / "report.txt", d / "archive" / "report.txt")
    ok_link = wait_for(lambda: link_of(link) == str(d / "archive" / "report.txt"))
    hint = link_of(link)
    sh = obj(me, link)
    # The shadow itself moved, by another program: still a shadow of the same object.
    (d / "elsewhere").mkdir()
    moved = d / "elsewhere" / link.name
    os.rename(link, moved)
    sh2 = obj(me, moved)
    me.stop()
    t, t2 = sh.get("target") or {}, sh2.get("target") or {}
    ok = r.get("ok") and link.name.endswith(".wpobj") and bool(ok_link) and sh.get("class") == "OsdShadow" and \
        t.get("title") == "Report" and t.get("path") == str(d / "archive" / "report.txt") and \
        sh2.get("class") == "OsdShadow" and t2.get("path") == str(d / "archive" / "report.txt")
    return ok, [f"shadow created as {link.name!r} in desk/ (a WPS object file)", f"after the target moved its hint is {hint}",
                f"resolves as {sh.get('class')} of {t.get('title')!r} at {t.get('path')}",
                f"moved by mv into elsewhere/: still {sh2.get('class')} of {t2.get('title')!r}"]


@scenario("4s", "A target moved while the service was stopped: its shadow is fixed at the next start")
def s4s():
    me = Service("me").start()
    d = fresh("ext4", "s4s")
    (d / "a").mkdir()
    (d / "b").mkdir()
    (d / "a" / "f.txt").write_text("x")
    tracked(me, d / "a" / "f.txt", "F")
    keep_files(me, d)
    link = Path(me.ctl("shadow", d / "a" / "f.txt", d).get("result", ""))
    me.stop()
    os.rename(d / "a" / "f.txt", d / "b" / "f.txt")
    me.start()
    ok = wait_for(lambda: link_of(link) == str(d / "b" / "f.txt"), timeout=10)
    me.stop()
    return bool(ok), [f"shadow now points to {link_of(link)}"]


@scenario("7s", "rm the target: its managed shadow is removed; a user-made symlink is left")
def s7s():
    me = Service("me").start()
    d = fresh("ext4", "s7s")
    (d / "t.txt").write_text("x")
    tracked(me, d / "t.txt", "T")
    keep_files(me, d)
    link = Path(me.ctl("shadow", d / "t.txt", d).get("result", ""))
    os.symlink(d / "t.txt", d / "my-own-link")
    obj(me, d / "my-own-link")
    os.unlink(d / "t.txt")
    gone = wait_for(lambda: not os.path.lexists(link))
    user_left = os.path.lexists(d / "my-own-link")
    me.stop()
    return bool(gone) and user_left, [f"managed shadow {link.name!r} removed: {bool(gone)}",
                                      f"user-made symlink left alone: {user_left}"]


@scenario(12, "Shadows into an unmounted volume show unavailable, then work again")
def s12():
    me = Service("me").start()
    d, t = fresh("ext4", "s12"), fresh("btrfs", "s12")
    (t / "data.bin").write_text("on btrfs\n")
    tracked(me, t / "data.bin", "Data")
    keep_files(me, d)
    link = Path(me.ctl("shadow", t / "data.bin", d).get("result", ""))
    me.stop()
    run(["sudo", "umount", str(MNT["btrfs"])], check=True)
    me.start()
    away = obj(me, link)
    me.stop()
    run(["sudo", "mount", "-o", "loop", str(WORK / "img" / "btrfs.img"), str(MNT["btrfs"])], check=True)
    me.start()
    back = obj(me, link)
    me.stop()
    ok = away.get("available") is False and link_of(link) == str(t / "data.bin") and back.get("available") is True
    return ok, [f"volume unmounted: available={away.get('available')}, symlink untouched: {link_of(link) == str(t / 'data.bin')}",
                f"mounted again: available={back.get('available')}"]


@scenario(35, "A makes a shadow in a shared folder and logs out; B moves the target; B's service fixes it")
def s35():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "s35", user=60001)
    run(["mkdir", "-m", "2775", f"{d}/team", f"{d}/docs", f"{d}/done"], user=60001, check=True)
    write_as(60001, d / "docs" / "spec.txt", "spec\n", mode="0664")
    tracked(a, d / "docs" / "spec.txt", "Spec")
    keep_files(a, d / "team")   # a team folder shares its shadows as files
    link = Path(a.ctl("shadow", d / "docs" / "spec.txt", d / "team").get("result", ""))
    a.ctl("flush")
    a.stop()                                   # A logs out
    obj(b, d / "team")                         # B has the team folder (and its records) in view
    obj(b, d / "docs" / "spec.txt")
    run(["mv", f"{d}/docs/spec.txt", f"{d}/done/spec.txt"], user=60002, check=True)
    fixed = wait_for(lambda: link_of(link) == str(d / "done" / "spec.txt"))
    b.stop()
    a.start()                                  # A logs in again
    r = a.ctl("resolve", link)
    t = (r.get("result") or {}).get("target") or {}
    works = t.get("path") == str(d / "done" / "spec.txt")
    a.stop()
    return bool(fixed) and works, [f"B updated A's shadow: {bool(fixed)} -> {link_of(link)}",
                                   f"A's shadow resolves to it after logging in: {t.get('path')}" +
                                   ("" if works else f" (A answered {json.dumps(r)[:400]})")]


@scenario("T1", "Trash: the object keeps its identity; its shadow follows and comes back on restore")
def st1():
    me = Service("me").start()
    d = fresh("ext4", "t1")
    trash = MNT["ext4"] / f".Trash-{os.getuid()}" / "files"
    trash.mkdir(parents=True, exist_ok=True)
    (d / "old.txt").write_text("x")
    oid = tracked(me, d / "old.txt", "Old notes")
    keep_files(me, d)
    link = Path(me.ctl("shadow", d / "old.txt", d).get("result", ""))
    os.rename(d / "old.txt", trash / "old.txt")
    in_trash = wait_for(lambda: link_of(link) == str(trash / "old.txt"))
    t = obj(me, f"id:{oid}")
    os.rename(trash / "old.txt", d / "old.txt")
    back = wait_for(lambda: link_of(link) == str(d / "old.txt"))
    t2 = obj(me, f"id:{oid}")
    me.stop()
    ok = bool(in_trash) and t.get("trashed") is True and t.get("title") == "Old notes" and bool(back) and \
        t2.get("trashed") is False
    return ok, [f"in the trash: shadow follows {bool(in_trash)}, trashed={t.get('trashed')}, title {t.get('title')!r}",
                f"restored: shadow back {bool(back)}, trashed={t2.get('trashed')}"]


@scenario("T2", "Emptying the trash is the real deletion: the shadow goes")
def st2():
    me = Service("me").start()
    d = fresh("ext4", "t2")
    trash = MNT["ext4"] / f".Trash-{os.getuid()}" / "files"
    trash.mkdir(parents=True, exist_ok=True)
    (d / "junk.txt").write_text("x")
    tracked(me, d / "junk.txt", "Junk")
    keep_files(me, d)
    link = Path(me.ctl("shadow", d / "junk.txt", d).get("result", ""))
    os.rename(d / "junk.txt", trash / "junk.txt")
    wait_for(lambda: link_of(link) == str(trash / "junk.txt"))
    os.unlink(trash / "junk.txt")
    gone = wait_for(lambda: not os.path.lexists(link))
    me.stop()
    return bool(gone), [f"shadow removed after emptying the trash: {bool(gone)}"]


# ------------------------------------------------- abstract objects are files

def ini(path, key):
    for line in Path(path).read_text().splitlines():
        if line.startswith(key + "="):
            return line[len(key) + 1:]
    return None


@scenario("A1", "A program object is a .desktop file that other desktops can read; it survives a lost database")
def sa1():
    me = Service("me").start()
    d = fresh("ext4", "a1")
    r = me.ctl("create", d, "OsdProgram", "Text Editor", "gnome-text-editor %U")
    p = Path(r.get("result", {}).get("path", ""))
    me.ctl("flush")
    me.call(p, "osdSetTitle", "Editor")
    me.call(p, "osdSetCommand", "gedit %U")
    me.ctl("flush")
    name, exe = ini(p, "Name"), ini(p, "Exec")
    v = run(["desktop-file-validate", str(p)]) if shutil.which("desktop-file-validate") else None
    me.stop()
    me.wipe_db()
    me.start()
    again = obj(me, p)
    cmd = me.call(p, "osdQueryCommand").get("result")
    me.stop()
    ok = r.get("ok") and p.suffix == ".desktop" and name == "Editor" and exe == "gedit %U" and \
        again.get("class") == "OsdProgram" and again.get("title") == "Editor" and cmd == "gedit %U" and \
        (v is None or v.returncode == 0)
    return ok, [f"created {p.name!r}; Name={name!r} Exec={exe!r}",
                f"desktop-file-validate: {'not installed' if v is None else 'ok' if v.returncode == 0 else v.stdout.strip()}",
                f"after deleting the database: {again.get('class')} {again.get('title')!r}, command {cmd!r}"]


@scenario("A2", "Other abstract objects are .wpobj files; a class not installed shows as a plain object")
def sa2():
    me = Service("me").start()
    d = fresh("ext4", "a2")
    r = me.ctl("create", d, "OsdAbstract", "Team notes")
    p = Path(r.get("result", {}).get("path", ""))
    me.ctl("flush")
    content = p.read_text() if p.exists() else ""
    (d / "printer.wpobj").write_text("[WPS Object]\nVersion=1\nClass=OsdPrinter\n")
    pr = obj(me, d / "printer.wpobj")
    me.stop()
    me.start()
    again = obj(me, p)
    me.stop()
    ok = r.get("ok") and p.suffix == ".wpobj" and "Class=OsdAbstract" in content and \
        again.get("title") == "Team notes" and pr.get("class") == "OsdAbstract" and pr.get("class_not_installed") == "OsdPrinter"
    return ok, [f"created {p.name!r} containing Class=OsdAbstract: {'Class=OsdAbstract' in content}",
                f"after restart: {again.get('class')} {again.get('title')!r}",
                f"printer.wpobj (OsdPrinter not installed): {pr.get('class')}, class_not_installed={pr.get('class_not_installed')!r}"]


@scenario("A3", "A shadow of a program object; creating never overwrites")
def sa3():
    me = Service("me").start()
    d = fresh("ext4", "a3")
    (d / "menu").mkdir()
    p1 = Path(me.ctl("create", d, "OsdProgram", "Terminal", "ptyxis").get("result", {}).get("path", ""))
    p2 = Path(me.ctl("create", d, "OsdProgram", "Terminal", "xterm").get("result", {}).get("path", ""))
    me.ctl("flush")
    keep_files(me, d / "menu")
    link = Path(me.ctl("shadow", p1, d / "menu").get("result", ""))
    sh = obj(me, link)
    me.stop()
    ok = p1.name == "Terminal.desktop" and p2.name == "Terminal (2).desktop" and ini(p1, "Exec") == "ptyxis" and \
        sh.get("class") == "OsdShadow" and (sh.get("target") or {}).get("class") == "OsdProgram" and link.name.endswith(".wpobj")
    return ok, [f"first: {p1.name!r}, second: {p2.name!r} (Exec of the first still {ini(p1, 'Exec')!r})",
                f"shadow in menu/ resolves to a {(sh.get('target') or {}).get('class')}"]


# -------------------------------------------------------------------- scale

def percentile(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * p))] if xs else None


def million():
    """<big>/million with exactly 1,000,000 files, whatever earlier runs left."""
    big = big_volume()
    d, moved = big / "million", big / "million-moved"
    def full(p):
        return p.is_dir() and sum(1 for _ in os.scandir(p)) == 1000000
    if not full(d):
        if full(moved):
            run(["rm", "-rf", str(d)])
            os.rename(moved, d)
        else:
            run(["rm", "-rf", str(d), str(moved)])
            d.mkdir()
            for i in range(1000000):
                os.close(os.open(d / f"f{i:07}", os.O_CREAT | os.O_WRONLY, 0o644))
    run(["rm", "-rf", str(moved)])
    return d


@scenario(28, "A folder of 1,000,000 files: first page fast; other requests stay fast while it loads")
def s28():
    d = million()
    run(["sudo", "sh", "-c", "sync; echo 3 > /proc/sys/vm/drop_caches"])   # a cold start
    me = Service("me").start()
    c = Conn(me)
    pings = []
    stop = [False]

    def pinger():   # interactive requests all the while the folder is read
        p2 = Conn(me)
        while not stop[0]:
            p0 = time.time()
            p2.request(op="ping")
            pings.append((time.time() - p0) * 1000)
        p2.close()

    import threading
    th = threading.Thread(target=pinger)
    t0 = time.time()
    th.start()
    # The first page is records only (design: no stat, no attribute reads);
    # details for the visible rows follow in a second request.
    first = c.request(op="list", target=str(d), offset=0, count=50)
    t_first = time.time() - t0
    t1 = time.time()
    det = c.request(op="list", target=str(d), offset=0, count=50, details=True)
    t_det = time.time() - t1
    r = c.request(op="list", target=str(d), offset=999950, count=50)   # answered when that far is read
    t_all = time.time() - t0
    stop[0] = True
    th.join()
    total = r.get("result", {}).get("total")
    last = [x["name"] for x in r.get("result", {}).get("items", [])]
    c.close()
    me.stop()
    fr = first.get("result", {})
    di = det.get("result", {}).get("items", [])
    ok = fr.get("items") and len(fr["items"]) == 50 and t_first < 0.15 and len(di) == 50 and di[0].get("size") == 0 and \
        total == 1000000 and len(last) == 50 and percentile(pings, 0.99) < 20
    return ok, [f"first page of 50 records after {t_first * 1000:.0f} ms (cold cache); complete=false then: {fr.get('complete') is False}",
                f"details for those 50 (sizes, times, IDs) {t_det * 1000:.0f} ms later: random inode reads, disk-bound",
                f"whole folder read in {t_all:.1f} s; total {total}; last page has {len(last)} entries",
                f"pings while loading: {len(pings)}, 99th percentile {percentile(pings, 0.99):.2f} ms, worst {max(pings):.2f} ms"]


# --------------------------------------------------------------------- jobs

FINAL = ("done", "failed", "cancelled", "interrupted", "paused-for-logout")


def job(c, jid):
    return c.request(op="job", job=jid).get("result", {})


def wait_job(c, jid, states=FINAL, timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        st = job(c, jid)
        if st.get("state") in states:
            return st
        time.sleep(0.05)
    return job(c, jid)


def start(c, kind, folder, dest, items=None):
    sel = c.request(op="select", folder=str(folder), all=items is None, **({"items": items} if items else {"except": []}))
    return c.request(op=kind, selection=sel["result"], dest=str(dest))["result"]


def pinger(svc, out, stop):
    import threading
    def run_():
        p = Conn(svc)
        while not stop[0]:
            t = time.time()
            p.request(op="ping")
            out.append((time.time() - t) * 1000)
        p.close()
    th = threading.Thread(target=run_)
    th.start()
    return th


def make_files(d, n, size=4096):
    d.mkdir(parents=True, exist_ok=True)
    blob = os.urandom(size)
    for i in range(n):
        with open(d / f"f{i:05}", "wb") as fh:
            fh.write(blob)


@scenario(29, "Drag-move 1,000,000 files within a filesystem: constant-time start, quick finish, loop stays fast")
def s29():
    src = million()
    dst = src.parent / "million-moved"
    dst.mkdir()
    me = Service("me").start()
    c = Conn(me)
    pings, stop = [], [False]
    th = pinger(me, pings, stop)
    t0 = time.time()
    jid = start(c, "move", src, dst)
    t_start = time.time() - t0
    st = wait_job(c, jid, timeout=300)
    took = time.time() - t0
    stop[0] = True
    th.join()
    c.close()
    me.stop()
    moved = sum(1 for _ in os.scandir(dst))
    ok = t_start < 0.05 and st.get("state") == "done" and moved == 1000000 and percentile(pings, 0.99) < 20
    return ok, [f"select + move answered in {t_start * 1000:.1f} ms; job {st.get('state')} after {took:.1f} s",
                f"entries in the destination: {moved}",
                f"pings during the move: {len(pings)}, 99th percentile {percentile(pings, 0.99):.2f} ms"]


@scenario(30, "Copy a 200 MB file across filesystems: progress arrives; cancel stops within 1 s and leaves nothing")
def s30():
    big = big_volume()
    src = fresh("ext4", "s30")
    with open(src / "video.bin", "wb") as fh:
        for _ in range(200):
            fh.write(os.urandom(1 << 20))
    dst = big / "s30"
    run(["rm", "-rf", str(dst)])
    dst.mkdir()
    me = Service("me").start()
    c = Conn(me)
    jid = start(c, "copy", src, dst)
    seen = set()
    while True:
        st = job(c, jid)
        seen.add(st.get("bytes_done"))
        if st.get("state") in FINAL:
            break
        time.sleep(0.01)
    copied_ok = st.get("state") == "done" and (dst / "video.bin").stat().st_size == 200 << 20
    (dst / "video.bin").unlink()
    run(["sh", "-c", "sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null"])
    jid2 = start(c, "copy", src, dst)
    wait_job(c, jid2, states=("running",), timeout=5)
    while job(c, jid2).get("bytes_done", 0) == 0:
        time.sleep(0.005)
    t0 = time.time()
    c.request(op="job_cancel", job=jid2)
    st2 = wait_job(c, jid2, timeout=5)
    took = time.time() - t0
    left = sorted(p.name for p in dst.iterdir())
    c.close()
    me.stop()
    ok = copied_ok and len(seen) >= 3 and st2.get("state") == "cancelled" and took < 1.0 and not left
    run(["rm", "-rf", str(dst), str(src)])
    return ok, [f"first copy {st.get('state')}, progress values seen: {len(seen)}",
                f"second copy cancelled after {took * 1000:.0f} ms: {st2.get('state')}; left in the destination: {left or 'nothing'}"]


@scenario(31, "wpsd killed during a copy of 10,000 files: reported interrupted after restart; resume completes it")
def s31():
    big = big_volume()
    src = fresh("ext4", "s31")
    make_files(src / "files", 10000)
    dst = big / "s31"
    run(["rm", "-rf", str(dst)])
    dst.mkdir()
    me = Service("me").start()
    c = Conn(me)
    jid = start(c, "copy", src / "files", dst)
    while job(c, jid).get("items_done", 0) < 1500:
        time.sleep(0.01)
    me.proc.kill()                      # a crash, not a clean stop
    me.proc.wait()
    me.proc = None
    on_disk = sum(1 for p in dst.iterdir() if not p.name.startswith(".wps-tmp-"))
    me.start()
    c = Conn(me)
    st = job(c, jid)
    c.request(op="job_resume", job=jid)
    done = wait_job(c, jid, timeout=120)
    files = [p for p in dst.iterdir()]
    good = sum(1 for p in files if not p.name.startswith(".wps-tmp-") and p.stat().st_size == 4096)
    c.close()
    me.stop()
    run(["rm", "-rf", str(dst)])
    ok = st.get("state") == "interrupted" and st.get("items_done", 0) <= on_disk + 1 and \
        done.get("state") == "done" and good == 10000 and len(files) == 10000
    return ok, [f"after the crash: {on_disk} files on disk; the job reports {st.get('state')} with {st.get('items_done')} done",
                f"after resume: {done.get('state')}; complete files {good}, entries {len(files)} (no leftovers)"]


@scenario(32, "Requests stay fast during a parallel build and a large copy")
def s32():
    big = big_volume()
    src = fresh("ext4", "s32src")
    with open(src / "video.bin", "wb") as fh:
        for _ in range(100):
            fh.write(os.urandom(1 << 20))
    dst = big / "s32"
    run(["rm", "-rf", str(dst)])
    dst.mkdir()
    build = fresh("ext4", "s32build")
    me = Service("me").start()
    obj(me, build)                      # the build folder is in view: its events arrive
    c = Conn(me)
    pings, stop = [], [False]
    th = pinger(me, pings, stop)
    jid = start(c, "copy", src, dst)
    for i in range(20000):
        os.close(os.open(build / f"o{i}.o", os.O_CREAT | os.O_WRONLY, 0o644))
    wait_job(c, jid, timeout=120)
    stop[0] = True
    th.join()
    c.close()
    me.stop()
    p99 = percentile(pings, 0.99)
    run(["rm", "-rf", str(src), str(dst), str(build)])
    return p99 < 20, [f"pings during the build and the copy: {len(pings)}, 99th percentile {p99:.2f} ms, worst {max(pings):.1f} ms"]


@scenario(34, "A and B drop a file of the same name into one folder at the same moment, 20 times")
def s34():
    import threading
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    base = fresh("ext4", "s34", user=60001)
    for u, who in ((60001, "a"), (60002, "b")):
        run(["mkdir", "-p", f"{base}/{who}"], user=u, check=True)
        run(["sh", "-c", f"printf 'from {who}\\n' > {base}/{who}/report.txt"], user=u, check=True)
    results = []
    for k in range(20):
        dest = base / f"drop{k}"
        run(["mkdir", "-m", "2777", str(dest)], user=60001, check=True)
        jids = {}
        def go(svc, who):
            sel = svc.ctl("select", base / who, "report.txt")["result"]
            jids[who] = svc.ctl("copy", sel, dest)["result"]
        t1 = threading.Thread(target=go, args=(a, "a")); t2 = threading.Thread(target=go, args=(b, "b"))
        t1.start(); t2.start(); t1.join(); t2.join()
        states = {}
        for who, svc in (("a", a), ("b", b)):
            for _ in range(100):
                st = svc.ctl("job", jids[who]).get("result", {})
                if st.get("state") in ("done", "needs-decision"):
                    break
                time.sleep(0.05)
            states[who] = st.get("state")
            if st.get("state") == "needs-decision":
                svc.ctl("job_decide", jids[who], "skip")
        try:
            content = (dest / "report.txt").read_text()
        except OSError:
            content = None
        if content is None or sorted(states.values()) != ["done", "needs-decision"]:
            print(f"                 round {k} detail: dest={sorted(p.name for p in dest.iterdir())} "
                  f"a={a.ctl('job', jids['a']).get('result', {}).get('error')} b={b.ctl('job', jids['b']).get('result', {}).get('error')}")
        if content is None and k == 0:
            print("                 debug:", a.ctl("job", jids["a"]), b.ctl("job", jids["b"]))
        results.append((sorted(states.values()), content))
    a.stop(); b.stop()
    for k, (st, content) in enumerate(results):
        if not (st == ["done", "needs-decision"] and content):
            print(f"                 round {k}: states {st} content {content!r}")
    good = sum(1 for st, content in results if st == ["done", "needs-decision"] and content in ("from a\n", "from b\n"))
    return good == 20, [f"rounds with exactly one copy placed and one clash: {good}/20",
                        f"the file is always one user's whole copy, never overwritten: {all(c in ('from a\n', 'from b\n') for _, c in results)}"]


@scenario(38, "A copy is running at logout: it pauses, and resumes at the next login")
def s38():
    big = big_volume()
    src = MNT["ext4"] / "s31" / "files"
    dst = big / "s38"
    run(["rm", "-rf", str(dst)])
    dst.mkdir()
    me = Service("me").start()
    c = Conn(me)
    jid = start(c, "copy", src, dst)
    while job(c, jid).get("items_done", 0) < 1000:
        time.sleep(0.01)
    c.close()
    me.stop()                           # logout
    me.start()
    c = Conn(me)
    resumed = job(c, jid).get("state")
    done = wait_job(c, jid, timeout=120)
    n = sum(1 for p in dst.iterdir() if not p.name.startswith(".wps-tmp-"))
    c.close()
    me.stop()
    ok = resumed in ("queued", "running", "done") and done.get("state") == "done" and n == 10000
    run(["rm", "-rf", str(dst), str(MNT["ext4"] / "s31")])
    return ok, [f"after logging in again the job was {resumed!r}, then {done.get('state')}; files copied {n}"]


@scenario(40, "Part of the source is moved away during a copy: reported missing, the rest copied")
def s40():
    big = big_volume()
    src = fresh("ext4", "s40")
    for sub in ("sub1", "sub2", "sub3", "sub4"):
        make_files(src / "tree" / sub, 800)
    dst = big / "s40"
    run(["rm", "-rf", str(dst)])
    dst.mkdir()
    me = Service("me").start()
    c = Conn(me)
    jid = start(c, "copy", src / "tree", dst, items=["sub1", "sub2", "sub3", "sub4"])
    while job(c, jid).get("current") not in ("sub1", "sub2"):
        time.sleep(0.002)
    os.rename(src / "tree" / "sub4", src / "away")
    st = wait_job(c, jid, timeout=120)
    c.close()
    me.stop()
    copied = sorted(p.name for p in dst.iterdir())
    run(["rm", "-rf", str(dst), str(src)])
    ok = st.get("state") == "done" and st.get("missing") == ["sub4"] and copied == ["sub1", "sub2", "sub3"]
    return ok, [f"job {st.get('state')}; missing {st.get('missing')}; copied {copied}"]


# ------------------------------------------------------------------- driver

def main():
    args = sys.argv[1:]
    mode = "inotify"
    if args[:1] == ["--mode"]:
        mode, args = args[1], args[2:]
    prepare()
    for f in OUT.glob("*.log"):
        f.unlink()
    reset_services()
    helper = None
    if mode == "watchd":
        sock = WORK / "watchd.sock"
        helper = subprocess.Popen(["sudo", "env", f"OSD_TRACE={os.environ.get('OSD_TRACE', '')}", str(BIN / "wps-watchd"), "-s", str(sock)],
                                  stdout=open(OUT / "wps-watchd.log", "w"), stderr=subprocess.STDOUT)
        time.sleep(0.5)
        harness.WATCHD["socket"] = str(sock)
    print(f"=== mode: {mode}")
    run_scenarios(set(args))
    if helper:
        run(["sudo", "pkill", "-TERM", "-x", "wps-watchd"])
        helper.wait(timeout=10)
    finish()


if __name__ == "__main__":
    main()
