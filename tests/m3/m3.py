#!/usr/bin/env python3
"""M3 scenarios: change tracking (design: Change tracking, Edge cases,
Detecting saves), run twice: once with inotify only, once with osd-watchd.

usage: tests/m3/m3.py [--mode inotify|watchd|both] [scenario ...]
Needs the test bed (proto/m0/testbed.sh up); uses sudo.
"""
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
os.environ.setdefault("OSD_SUITE", "m3")
import harness  # noqa: E402
from harness import *  # noqa: E402,F401,F403

HERE = Path(__file__).resolve().parent
M0 = ROOT / "proto" / "m0"
MODE = {"name": "inotify"}


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
    """Give a file an object with a title; return its ID."""
    svc.call(path, "osdSetTitle", title)
    svc.ctl("flush")
    return obj(svc, path).get("id")


def where(svc, oid):
    return obj(svc, f"id:{oid}").get("path")


def stats(svc):
    return svc.ctl("stats").get("result", {})


# ------------------------------------------------------------------ scenarios

@scenario(4, "A file moved while the service was stopped is found by the catch-up scan")
def s4():
    me = Service("me").start()
    d = fresh("ext4", "s4")
    (d / "a").mkdir()
    (d / "b").mkdir()
    (d / "a" / "doc.txt").write_text("x")
    oid = tracked(me, d / "a" / "doc.txt", "Moved while away")
    me.stop()
    os.rename(d / "a" / "doc.txt", d / "b" / "renamed.txt")
    me.start()
    p = wait_for(lambda: where(me, oid) == str(d / "b" / "renamed.txt") and where(me, oid))
    t = obj(me, f"id:{oid}").get("title")
    me.stop()
    ok = p == str(d / "b" / "renamed.txt") and t == "Moved while away"
    return ok, [f"after restart the ID resolves to {p}", f"title {t!r}"]


def save_with(path, how):
    if how == "vi":
        subprocess.run(["vi", "-es", "-N", "-u", "NONE", "-i", "NONE", "-c", "s/x/y/", "-c", "wq", str(path)], check=True)
    elif how == "vim":
        subprocess.run(["vim", "-es", "-N", "-u", "NONE", "-i", "NONE", "-c", "s/x/y/", "-c", "wq", str(path)], check=True)
    elif how == "sed -i":
        subprocess.run(["sed", "-i", "s/x/y/", str(path)], check=True)
    elif how == "GNOME Text Editor (GtkSourceView 5)":
        subprocess.run(["python3", str(M0 / "gtksource_save.py"), str(path)], input="\n", text=True,
                       capture_output=True, check=True)
    elif how == "gio save":
        subprocess.run(["gio", "save", str(path)], input="replaced x\n", text=True, check=True)
    elif how == "temp file in another folder":
        other = path.parent.parent / "tmpdir"
        other.mkdir(exist_ok=True)
        (other / ".tmp").write_text("new x\n")
        os.rename(other / ".tmp", path)


@scenario(5, "Editors: the object keeps its ID and settings through each kind of save")
def s5():
    me = Service("me").start()
    d = fresh("ext4", "s5")
    (d / "docs").mkdir()
    rows, ok = [], True
    for i, how in enumerate(["vi", "vim", "sed -i", "GNOME Text Editor (GtkSourceView 5)", "gio save",
                             "temp file in another folder"]):
        f = d / "docs" / f"file{i}.txt"
        f.write_text("line x\n")
        oid = tracked(me, f, f"Title {i}")
        ino = f.stat().st_ino
        save_with(f, how)
        time.sleep(0.5)
        me.ctl("flush")
        got = wait_for(lambda: obj(me, f).get("id") == oid and obj(me, f))
        same = got and got.get("id") == oid and got.get("title") == f"Title {i}"
        new_inode = f.stat().st_ino != ino
        rows.append(f"{how:38} new inode: {str(new_inode):5}  ID kept: {bool(same)}  title: {got.get('title') if got else None!r}")
        ok &= bool(same)
    me.stop()
    return ok, rows


@scenario("5b", "vim backups kept (set backup): with backup-names on, the edited file keeps the object")
def s5b():
    me = Service("me")
    me.config_file("backup-names", "on\n")
    me.start()
    d = fresh("ext4", "s5b")
    f = d / "notes.txt"
    f.write_text("line x\n")
    oid = tracked(me, f, "With backups")
    subprocess.run(["vi", "-es", "-N", "-u", "NONE", "-i", "NONE", "-c", "set backup", "-c", "s/x/y/", "-c", "wq",
                    str(f)], check=True)
    time.sleep(0.5)
    got = wait_for(lambda: obj(me, f).get("id") == oid and obj(me, f))
    backup = (d / "notes.txt~").exists()
    me.stop()
    (me.base / "config" / "backup-names").unlink()
    ok = backup and got and got.get("id") == oid and got.get("title") == "With backups"
    return ok, [f"backup notes.txt~ kept: {backup}", f"notes.txt keeps ID: {bool(got and got.get('id') == oid)}"]


@scenario(6, "mv across filesystems (ext4 -> btrfs, 64 MB) keeps the object")
def s6():
    me = Service("me").start()
    a, b = fresh("ext4", "s6"), fresh("btrfs", "s6")
    f = a / "big.bin"
    with open(f, "wb") as out:
        out.write(os.urandom(1 << 20) * 64)
    oid = tracked(me, f, "Big file")
    obj(me, b)   # the destination folder is in view
    subprocess.run(["mv", str(f), str(b / "big.bin")], check=True)
    p = wait_for(lambda: where(me, oid) == str(b / "big.bin") and where(me, oid))
    t = obj(me, b / "big.bin").get("title")
    i2 = obj(me, b / "big.bin").get("id")
    me.stop()
    ok = p == str(b / "big.bin") and i2 == oid and t == "Big file"
    return ok, [f"ID now at {p}", f"title {t!r}, same ID: {i2 == oid}"]


@scenario(7, "rm: the object is forgotten")
def s7():
    me = Service("me").start()
    d = fresh("ext4", "s7")
    (d / "gone.txt").write_text("x")
    oid = tracked(me, d / "gone.txt", "Soon gone")
    os.unlink(d / "gone.txt")
    gone = wait_for(lambda: not me.ctl("resolve", f"id:{oid}").get("ok"))
    me.stop()
    return bool(gone), [f"resolving the ID after rm fails: {bool(gone)}"]


@scenario(8, "cp -a shares the ID until the copy changes, then the copy gets its own")
def s8():
    me = Service("me").start()
    d = fresh("ext4", "s8")
    (d / "orig.txt").write_text("x")
    oid = tracked(me, d / "orig.txt", "Original")
    subprocess.run(["cp", "-a", str(d / "orig.txt"), str(d / "copy.txt")], check=True)
    time.sleep(0.5)
    before = obj(me, d / "copy.txt").get("id")
    with open(d / "copy.txt", "a") as fh:
        fh.write("changed\n")
    after = wait_for(lambda: (obj(me, d / "copy.txt").get("id") or oid) != oid and obj(me, d / "copy.txt").get("id"))
    orig = obj(me, d / "orig.txt").get("id")
    title = obj(me, d / "copy.txt").get("title")
    me.stop()
    ok = before == oid and after and after != oid and orig == oid and title == "Original"
    return ok, [f"right after cp -a the copy has the same ID: {before == oid}",
                f"after changing the copy: its own ID {after != oid}, original keeps its ID {orig == oid}",
                f"the copy kept the settings: title {title!r}"]


@scenario(9, "mv draft.txt final.txt in one folder, both with IDs: final's object is replaced")
def s9():
    me = Service("me").start()
    d = fresh("ext4", "s9")
    (d / "draft.txt").write_text("x")
    (d / "final.txt").write_text("y")
    id_draft = tracked(me, d / "draft.txt", "Draft")
    id_final = tracked(me, d / "final.txt", "Final")
    os.rename(d / "draft.txt", d / "final.txt")
    time.sleep(0.5)
    now = obj(me, d / "final.txt")
    old = wait_for(lambda: not me.ctl("resolve", f"id:{id_final}").get("ok"))
    me.stop()
    ok = now.get("id") == id_draft and now.get("title") == "Draft" and bool(old)
    return ok, [f"final.txt is now the draft's object: {now.get('id') == id_draft} (title {now.get('title')!r})",
                f"the old final's ID no longer resolves: {bool(old)}"]


@scenario(11, "Atomic save through one name of a hard-linked file: the link splits")
def s11():
    me = Service("me").start()
    d = fresh("ext4", "s11")
    (d / "a.txt").write_text("x")
    os.link(d / "a.txt", d / "b.txt")
    oid = tracked(me, d / "a.txt", "Linked")
    tmp = d / ".a.tmp"
    tmp.write_text("changed\n")
    os.rename(tmp, d / "a.txt")
    time.sleep(0.6)
    na = wait_for(lambda: obj(me, d / "a.txt").get("id") not in (None, "", oid) and obj(me, d / "a.txt"))
    nb = obj(me, d / "b.txt")
    split = run(["getfattr", "--only-values", "-n", "user.osd.OsdObject", str(d / "a.txt")]).stdout
    me.stop()
    ok = na and na.get("id") != oid and nb.get("id") == oid and na.get("title") == "Linked" and "split" in split
    return ok, [f"b.txt keeps the original ID: {nb.get('id') == oid}",
                f"a.txt is its own object now: {bool(na) and na.get('id') != oid}, settings copied (title {na.get('title') if na else None!r})",
                f"marked as split: {'split' in split}"]


@scenario(13, "A symlink loop inside a scanned tree: the scan finishes")
def s13():
    me = Service("me").start()
    d = fresh("ext4", "s13")
    (d / "deep" / "er").mkdir(parents=True)
    os.symlink("..", d / "deep" / "er" / "loop")
    (d / "x.txt").write_text("x")
    oid = tracked(me, d / "x.txt", "Scanned")
    me.stop()
    os.rename(d / "x.txt", d / "deep" / "er" / "x.txt")
    me.start()
    t0 = time.time()
    rep = me.ctl("scan").get("result", "")
    took = time.time() - t0
    p = where(me, oid)
    me.stop()
    ok = p == str(d / "deep" / "er" / "x.txt") and took < 30
    return ok, [f"scan: {rep} ({took:.1f} s)", f"found at {p}"]


@scenario(14, "A flood of 100,000 renames: overflow, rescan, and the object ends up correct")
def s14():
    me = Service("me").start()
    d = fresh("ext4", "s14")
    names = [f"f{i:06}" for i in range(100000)]
    for n in names:
        open(d / n, "w").close()
    oid = tracked(me, d / "f000000", "Needle")
    t0 = time.time()
    for n in names:
        os.rename(d / n, d / (n + ".r"))
    took = time.time() - t0
    p = wait_for(lambda: where(me, oid) == str(d / "f000000.r") and where(me, oid), timeout=60)
    st = stats(me)
    me.stop()
    ok = p == str(d / "f000000.r")
    return ok, [f"100,000 renames in {took:.1f} s", f"overflows seen: {st.get('overflows')}, scans: {st.get('scans')}",
                f"the tracked object is at {p}"]


@scenario(15, "A build inside an excluded path (node_modules): no objects, no state")
def s15():
    me = Service("me").start()
    d = fresh("ext4", "s15")
    nm = d / "node_modules"
    nm.mkdir()
    obj(me, d)
    obj(me, nm)
    before = stats(me)
    for i in range(20000):
        open(nm / f"m{i}.js", "w").close()
    me.call(nm / "m1.js", "osdSetTitle", "should not stick")
    me.ctl("flush")
    has_id = run(["getfattr", "-n", "user.osd.id", str(nm / "m1.js")]).returncode == 0
    after = stats(me)
    me.stop()
    ok = not has_id and after.get("rows", 0) == before.get("rows", 0)
    return ok, [f"files created: 20,000; IDs given: {has_id}",
                f"database rows before/after: {before.get('rows')}/{after.get('rows')}",
                f"events counted: {after.get('events', 0) - before.get('events', 0)}"]


@scenario(26, "User B holds a lock on a shared file forever: A stays responsive; the change waits, then lands")
def s26():
    a = Service("A", user=60001).start()
    d = fresh("ext4", "s26", user=60001)
    write_as(60001, d / "shared.txt", mode="0664")
    a.call(d / "shared.txt", "osdSetTitle", "First")
    a.ctl("flush")
    holder = subprocess.Popen(["sudo", "setpriv", "--reuid=60002", "--regid=60000", "--groups=60000", "--",
                               "flock", "-x", str(d / "shared.txt"), "sleep", "6"])
    time.sleep(0.5)
    a.call(d / "shared.txt", "osdSetTitle", "Second")
    t0 = time.time()
    a.ctl("flush")
    pings = []
    for _ in range(5):
        p0 = time.time()
        a.ctl("ping")
        pings.append(time.time() - p0)
    seen = a.title(d / "shared.txt")
    shared_during = attr(d / "shared.txt") or ""
    holder.wait()
    landed = wait_for(lambda: "Second" in (attr(d / "shared.txt") or ""), timeout=20)
    a.stop()
    ok = seen == "Second" and "Second" not in shared_during and bool(landed) and max(pings) < 0.5
    return ok, [f"A sees {seen!r} at once; slowest ping while blocked {max(pings) * 1000:.0f} ms",
                f"shared attribute while locked has it: {'Second' in shared_during}",
                f"after the lock is released it lands in the shared attribute: {bool(landed)}"]


@scenario(27, "User B renames files in B's private folder: A's service learns nothing")
def s27():
    if MODE["name"] != "watchd":
        return True, ["(inotify mode: A cannot watch B's private folder at all)"]
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    priv = fresh("ext4", "s27", user=60002, mode="0700")
    write_as(60002, priv / "secret.txt", mode="0600")
    b.call(priv / "secret.txt", "osdSetTitle", "B's secret")
    b.ctl("flush")
    obj(a, MNT["ext4"])
    before = stats(a).get("events", 0)
    for i in range(20):
        run(["mv", f"{priv}/secret.txt", f"{priv}/secret{i}.txt"], user=60002)
        run(["mv", f"{priv}/secret{i}.txt", f"{priv}/secret.txt"], user=60002)
    time.sleep(0.5)
    after = stats(a).get("events", 0)
    a.stop(); b.stop()
    return after == before, [f"A's events before/after B's 40 renames: {before}/{after}"]


@scenario(33, "A and B both track a shared folder; A renames a file; B sees the new place within 1 s")
def s33():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "s33", user=60001)
    write_as(60001, d / "plan.txt", mode="0664")
    oid = tracked(a, d / "plan.txt", "Plan")
    obj(b, d / "plan.txt")           # B has it in view
    run(["mv", f"{d}/plan.txt", f"{d}/plan-v2.txt"], user=60001)
    t0 = time.time()
    p = wait_for(lambda: where(b, oid) == str(d / "plan-v2.txt") and where(b, oid), timeout=3, step=0.05)
    took = time.time() - t0
    a.stop(); b.stop()
    ok = p == str(d / "plan-v2.txt") and took < 1.0
    return ok, [f"B resolves the ID to {p} after {took * 1000:.0f} ms"]


@scenario(36, "A and B both see an atomic save of a shared file: one ID, settings carried once")
def s36():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "s36", user=60001)
    write_as(60001, d / "notes.txt", "x", mode="0664")
    oid = tracked(a, d / "notes.txt", "Shared notes")
    obj(b, d / "notes.txt")
    run(["sed", "-i", "s/x/y/", str(d / "notes.txt")], user=60001)
    ia = wait_for(lambda: obj(a, d / "notes.txt").get("id") == oid and oid)
    ib = wait_for(lambda: obj(b, d / "notes.txt").get("id") == oid and oid)
    t = obj(b, d / "notes.txt").get("title")
    raw = run(["getfattr", "-d", "-m", "user.wps", str(d / "notes.txt")]).stdout
    a.stop(); b.stop()
    ok = ia == ib == oid and t == "Shared notes"
    return ok, [f"A and B both see ID {oid}: {ia == ib == oid}", f"title on the new file {t!r}",
                f"attributes on the new inode: {raw.count('user.wps')} ({'id' if 'user.osd.id' in raw else 'no id'})"]


# ------------------------------------------------------------------- driver

def start_watchd():
    sock = WORK / "watchd.sock"
    log = OUT / "osd-watchd.log"
    p = subprocess.Popen(["sudo", str(BIN / "osd-watchd"), "-s", str(sock)], stdout=open(log, "w"), stderr=subprocess.STDOUT)
    for _ in range(50):
        if sock.exists():
            break
        time.sleep(0.1)
    harness.WATCHD["socket"] = str(sock)
    return p


def main():
    args = sys.argv[1:]
    mode = "both"
    if args[:1] == ["--mode"]:
        mode, args = args[1], args[2:]
    prepare()
    for f in OUT.glob("*.log"):
        f.unlink()
    modes = ["inotify", "watchd"] if mode == "both" else [mode]
    for m in modes:
        MODE["name"] = m
        reset_services()
        helper = None
        if m == "watchd":
            helper = start_watchd()
        else:
            harness.WATCHD["socket"] = "none"
        print(f"\n=== mode: {m}")
        run_scenarios(set(args), prefix=f"{m[0]}:")
        if helper:
            run(["sudo", "pkill", "-TERM", "-x", "osd-watchd"])
            helper.wait(timeout=10)
    finish()


if __name__ == "__main__":
    main()
