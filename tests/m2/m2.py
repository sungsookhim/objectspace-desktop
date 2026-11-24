#!/usr/bin/env python3
"""M2 scenarios: persistence, identity, concurrency, users, travelling volumes.

Runs real osdd services against the loopback test bed (proto/m0/testbed.sh
up): ext4, btrfs and FAT. Services run as the current user, and as test
users 60001 (A) and 60002 (B, both in group 60000) through setpriv. A second
"computer" is simulated by a second osdd with its own database and host ID
but the same Objectspace identity file.

usage: tests/m2/m2.py [scenario ...]     e.g. m2.py 1 25a
Writes build/m2/report.json.
"""
import os
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
os.environ.setdefault("OSD_SUITE", "m2")
from harness import *  # noqa: E402,F401,F403


# ------------------------------------------------------------------ scenarios

@scenario(1, "Settings survive a restart (ext4 attribute, and FAT through the database)")
def s1():
    me = Service("me").start()
    d, f = fresh("ext4", "s1"), fresh("fat", "s1")
    (d / "notes.txt").write_text("x")
    (f / "notes.txt").write_text("x")
    me.call(d / "notes.txt", "osdSetTitle", "Notes on ext4")
    me.call(f / "notes.txt", "osdSetTitle", "Notes on FAT")
    me.ctl("flush")
    stored = attr(d / "notes.txt") is not None
    me.stop()
    me.start()
    t1, t2 = me.title(d / "notes.txt"), me.title(f / "notes.txt")
    me.stop()
    ok = t1 == "Notes on ext4" and t2 == "Notes on FAT" and stored
    return ok, [f"ext4 title after restart: {t1!r} (stored in the file's attribute: {stored})",
                f"FAT title after restart: {t2!r} (FAT has no attributes: kept in the database)"]


@scenario(2, "Database deleted: file-stored state survives; database-only state is gone")
def s2():
    me = Service("me")
    d, f = MNT["ext4"] / "s1", MNT["fat"] / "s1"   # left by scenario 1
    me.wipe_db()
    me.start()
    t1, t2 = me.title(d / "notes.txt"), me.title(f / "notes.txt")
    me.stop()
    ok = t1 == "Notes on ext4" and t2 == "notes.txt"
    return ok, [f"ext4 title with a new database: {t1!r} (rebuilt from the file)",
                f"FAT title with a new database: {t2!r} (expected: lost, as the design says)"]


@scenario(10, "Hard links: one object ID, a separate position in each folder")
def s10():
    me = Service("me").start()
    d = fresh("ext4", "s10")
    (d / "a").mkdir()
    (d / "b").mkdir()
    (d / "a" / "doc.txt").write_text("x")
    os.link(d / "a" / "doc.txt", d / "b" / "doc.txt")
    me.call(d / "a", "osdSetIconPos", f"@{d}/a/doc.txt", 10, 10)
    me.call(d / "b", "osdSetIconPos", f"@{d}/b/doc.txt", 90, 90)
    me.ctl("flush")
    ida = me.ctl("resolve", d / "a" / "doc.txt")["result"]["id"]
    idb = me.ctl("resolve", d / "b" / "doc.txt")["result"]["id"]
    pa = me.call(d / "a", "osdQueryIconPos", f"@{d}/a/doc.txt").get("result")
    pb = me.call(d / "b", "osdQueryIconPos", f"@{d}/b/doc.txt").get("result")
    me.stop()
    ok = ida and ida == idb and pa == "10,10" and pb == "90,90"
    return ok, [f"IDs through the two names: {ida} / {idb}", f"positions: folder a {pa}, folder b {pb}"]


@scenario(21, "Two clients change different settings of one object, 200 times each")
def s21():
    me = Service("me").start()
    d = fresh("ext4", "s21")
    (d / "folder").mkdir()
    target = d / "folder"
    def client(method, values):
        for v in values:
            me.call(target, method, v)
    t1 = threading.Thread(target=client, args=("osdSetTitle", [f"title {i}" for i in range(200)]))
    t2 = threading.Thread(target=client, args=("osdSetSortOrder", [i % 16 for i in range(200)]))
    t1.start(); t2.start(); t1.join(); t2.join()
    me.ctl("flush")
    me.stop()
    me.start()
    title = me.title(target)
    sort = me.call(target, "osdQuerySortOrder").get("result")
    me.stop()
    ok = title == "title 199" and sort == 199 % 16
    return ok, [f"after both clients and a restart: title {title!r}, sort order {sort}"]


@scenario(22, "Two clients change the same setting at once: one wins, one gets a conflict")
def s22():
    me = Service("me").start()
    d = fresh("ext4", "s22")
    (d / "f.txt").write_text("x")
    seen = me.ctl("resolve", d / "f.txt")["result"]["changes"]
    r1 = me.call(d / "f.txt", "osdSetTitle", "from client 1", expect=seen)
    r2 = me.call(d / "f.txt", "osdSetTitle", "from client 2", expect=seen)
    me.stop()
    ok = r1.get("ok") and not r2.get("ok") and r2.get("error") == "conflict" and \
        r2.get("object", {}).get("title") == "from client 1"
    return ok, [f"first: ok={r1.get('ok')}; second: {r2.get('error')}, current title {r2.get('object', {}).get('title')!r}"]


@scenario(23, "Users A and B write settings on a shared, writable folder")
def s23():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "s23", user=60001)
    run(["mkdir", f"{d}/project"], user=60001, check=True)
    run(["chmod", "2775", f"{d}/project"], user=60001, check=True)
    a.call(d / "project", "osdSetTitle", "Project X")
    b.call(d / "project", "osdSetSortOrder", 5)
    a.ctl("flush"); b.ctl("flush")
    ta, sa = a.title(d / "project"), a.call(d / "project", "osdQuerySortOrder").get("result")
    tb, sb = b.title(d / "project"), b.call(d / "project", "osdQuerySortOrder").get("result")
    a.stop(); b.stop()
    ok = ta == tb == "Project X" and sa == sb == 5
    return ok, [f"A sees title {ta!r}, sort {sa}", f"B sees title {tb!r}, sort {sb}",
                "both changes are in the folder's shared attributes"]


@scenario(24, "User B, without write permission, retitles A's file")
def s24():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "s24", user=60001)
    write_as(60001, d / "report.txt", mode="0644")
    a.call(d / "report.txt", "osdSetTitle", "Report")
    a.ctl("flush")
    rb = b.call(d / "report.txt", "osdSetTitle", "My copy of the report")
    b.ctl("flush")
    tb, ta = b.title(d / "report.txt"), a.title(d / "report.txt")
    b.stop(); b.start()
    tb2 = b.title(d / "report.txt")
    a.stop(); b.stop()
    raw = attr(d / "report.txt") or ""
    ok = rb.get("ok") and tb == tb2 == "My copy of the report" and ta == "Report" and "My copy" not in raw
    return ok, [f"B sees {tb!r} (and {tb2!r} after restarting B)", f"A sees {ta!r}",
                f"file attribute unchanged by B: {'My copy' not in raw}"]


@scenario(25, "Users A and B arrange a shared folder differently")
def s25():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "s25", user=60001)
    write_as(60001, d / "plan.txt", mode="0664")
    a.call(d, "osdSetIconPos", f"@{d}/plan.txt", 40, 40)
    b.call(d, "osdSetIconPos", f"@{d}/plan.txt", 300, 200)
    a.ctl("flush"); b.ctl("flush")
    a.stop(); b.stop(); a.start(); b.start()
    pa = a.call(d, "osdQueryIconPos", f"@{d}/plan.txt").get("result")
    pb = b.call(d, "osdQueryIconPos", f"@{d}/plan.txt").get("result")
    a.stop(); b.stop()
    ok = pa == "40,40" and pb == "300,200"
    return ok, [f"A (owner, stored on the folder): {pa}", f"B (stored in B's own database): {pb}"]


def portable(svc, fs):
    svc.config_file("portable", fs_uuid(fs) + "\n")


@scenario("25a", "A FAT disk carries the state to another computer (same Objectspace identity)")
def s25a():
    home = Service("home")
    home.start(); home.stop()                     # creates the identity
    work = Service("work", identity_from=home)
    portable(home, "fat"); portable(work, "fat")
    d = fresh("fat", "s25a")
    (d / "slides.odp").write_text("x")
    home.start()
    home.call(d / "slides.odp", "osdSetTitle", "Conference slides")
    home.ctl("flush")
    home.stop()
    remount("fat", "rw")                          # the disk is unplugged and taken to work
    work.start()
    t = work.title(d / "slides.odp")
    work.stop()
    voldb = list((MNT["fat"] / ".osd").glob("*.db"))
    ok = t == "Conference slides" and len(voldb) == 1
    return ok, [f"title on the other computer: {t!r}", f"volume databases on the disk: {[p.name for p in voldb]}"]


@scenario("25b", "Both computers change the same title while apart; read-only changes wait")
def s25b():
    home, work = Service("home"), Service("work")
    d = MNT["fat"] / "s25a"
    remount("fat", "ro")                          # at home, mounted read-only
    home.start()
    r = home.call(d / "slides.odp", "osdSetTitle", "Home version")
    home.ctl("flush")
    home.stop()
    remount("fat", "rw")                          # taken to work, writable there
    work.start()
    work.call(d / "slides.odp", "osdSetTitle", "Work version")
    work.ctl("flush")
    work.stop()
    remount("fat", "rw")                          # back home, writable now: sync
    home.start()
    sync = home.ctl("sync").get("result", "")
    th = home.title(d / "slides.odp")
    stats = home.ctl("stats").get("result", {})
    home.stop()
    work.start()
    work.ctl("sync")
    tw = work.title(d / "slides.odp")
    work.stop()
    ok = r.get("ok") and th == tw and th in ("Home version", "Work version") and stats.get("conflicts", 0) >= 1
    return ok, [f"home changed it while read-only: ok={r.get('ok')}", f"sync at home: {sync}",
                f"home now shows {th!r}, work shows {tw!r} (they agree)",
                f"conflicts recorded at home: {stats.get('conflicts')}"]


@scenario("25d", "Two users share a portable disk: separate volume databases")
def s25d():
    a = Service("A2", user=60001)
    b = Service("B2", user=60002)
    for s in (a, b):
        portable(s, "ext4")
    # The disk's .osd folder (mode 1777, like /tmp) is made by the first user
    # who can, or by an administrator; here the disk root belongs to neither.
    run(["sudo", "mkdir", "-p", str(MNT["ext4"] / ".osd")], check=True)
    run(["sudo", "chmod", "1777", str(MNT["ext4"] / ".osd")], check=True)
    d = fresh("ext4", "s25d", user=60001)
    write_as(60001, d / "shared.txt", mode="0644")
    a.start(); b.start()
    a.call(d, "osdSetIconPos", f"@{d}/shared.txt", 1, 1)
    b.call(d / "shared.txt", "osdSetTitle", "B's private name")   # no permission: B's overlay
    a.ctl("flush"); b.ctl("flush")
    a.ctl("sync"); b.ctl("sync")
    ta = a.title(d / "shared.txt")
    a.stop(); b.stop()
    dbs = sorted((MNT["ext4"] / ".osd").glob("*.db"))
    modes = {p.name: oct(p.stat().st_mode & 0o777) for p in dbs}
    readable = [run(["test", "-r", str(p)], user=60001).returncode == 0 for p in dbs]
    ok = len(dbs) == 2 and all(m == "0o600" for m in modes.values()) and ta == "shared.txt" and readable.count(True) == 1
    return ok, [f"volume databases: {modes}", f"A sees the file as {ta!r} (B's name stays B's)",
                f"A can read exactly one of them (its own): {readable.count(True) == 1}"]


# ------------------------------------------------------------------- driver

def main():
    prepare()
    for f in OUT.glob("osdd-*.log"):
        f.unlink()
    reset_services()
    run_scenarios(set(sys.argv[1:]))
    finish()


if __name__ == "__main__":
    main()
