#!/usr/bin/env python3
"""M4 scenarios: shadows, trash, abstract objects as files, scale, jobs.

usage: tests/m4/m4.py [--mode inotify|watchd] [scenario ...]
Needs the test bed (proto/m0/testbed.sh up); uses sudo.
"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
os.environ.setdefault("WPS_SUITE", "m4")
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
    svc.call(path, "wpSetTitle", title)
    svc.ctl("flush")
    return obj(svc, path).get("id")


def link_of(path):
    try:
        return os.readlink(path)
    except OSError:
        return None


# ------------------------------------------------------------------ shadows

@scenario(3, "A shadow follows its target to another folder; reading through it works")
def s3():
    me = Service("me").start()
    d = fresh("ext4", "s3")
    for sub in ("docs", "desk", "archive"):
        (d / sub).mkdir()
    (d / "docs" / "report.txt").write_text("quarterly numbers\n")
    tracked(me, d / "docs" / "report.txt", "Report")
    r = me.ctl("shadow", d / "docs" / "report.txt", d / "desk")
    link = Path(r.get("result", ""))
    os.rename(d / "docs" / "report.txt", d / "archive" / "report.txt")
    ok_link = wait_for(lambda: link_of(link) == str(d / "archive" / "report.txt"))
    content = link.read_text() if ok_link else None
    sh = obj(me, link)
    me.stop()
    ok = r.get("ok") and bool(ok_link) and content == "quarterly numbers\n" and \
        sh.get("class") == "WPShadow" and (sh.get("target") or {}).get("title") == "Report"
    return ok, [f"shadow created at {link.name!r} in desk/", f"after the move it points to {link_of(link)}",
                f"reading through it: {content!r}", f"resolves as {sh.get('class')} of {(sh.get('target') or {}).get('title')!r}"]


@scenario("4s", "A target moved while the service was stopped: its shadow is fixed at the next start")
def s4s():
    me = Service("me").start()
    d = fresh("ext4", "s4s")
    (d / "a").mkdir()
    (d / "b").mkdir()
    (d / "a" / "f.txt").write_text("x")
    tracked(me, d / "a" / "f.txt", "F")
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
    link = Path(a.ctl("shadow", d / "docs" / "spec.txt", d / "team").get("result", ""))
    a.ctl("flush")
    a.stop()                                   # A logs out
    obj(b, d / "team")                         # B has the team folder (and its records) in view
    obj(b, d / "docs" / "spec.txt")
    run(["mv", f"{d}/docs/spec.txt", f"{d}/done/spec.txt"], user=60002, check=True)
    fixed = wait_for(lambda: link_of(link) == str(d / "done" / "spec.txt"))
    b.stop()
    a.start()                                  # A logs in again
    works = run(["cat", str(link)], user=60001).stdout == "spec\n"
    a.stop()
    return bool(fixed) and works, [f"B rewrote A's shadow: {bool(fixed)} -> {link_of(link)}",
                                   f"A reads through it after logging in: {works}"]


@scenario("T1", "Trash: the object keeps its identity; its shadow follows and comes back on restore")
def st1():
    me = Service("me").start()
    d = fresh("ext4", "t1")
    trash = MNT["ext4"] / f".Trash-{os.getuid()}" / "files"
    trash.mkdir(parents=True, exist_ok=True)
    (d / "old.txt").write_text("x")
    oid = tracked(me, d / "old.txt", "Old notes")
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
    r = me.ctl("create", d, "WPProgram", "Text Editor", "gnome-text-editor %U")
    p = Path(r.get("result", {}).get("path", ""))
    me.ctl("flush")
    me.call(p, "wpSetTitle", "Editor")
    me.call(p, "wpSetCommand", "gedit %U")
    me.ctl("flush")
    name, exe = ini(p, "Name"), ini(p, "Exec")
    v = run(["desktop-file-validate", str(p)]) if shutil.which("desktop-file-validate") else None
    me.stop()
    me.wipe_db()
    me.start()
    again = obj(me, p)
    cmd = me.call(p, "wpQueryCommand").get("result")
    me.stop()
    ok = r.get("ok") and p.suffix == ".desktop" and name == "Editor" and exe == "gedit %U" and \
        again.get("class") == "WPProgram" and again.get("title") == "Editor" and cmd == "gedit %U" and \
        (v is None or v.returncode == 0)
    return ok, [f"created {p.name!r}; Name={name!r} Exec={exe!r}",
                f"desktop-file-validate: {'not installed' if v is None else 'ok' if v.returncode == 0 else v.stdout.strip()}",
                f"after deleting the database: {again.get('class')} {again.get('title')!r}, command {cmd!r}"]


@scenario("A2", "Other abstract objects are .wpobj files; a class not installed shows as a plain object")
def sa2():
    me = Service("me").start()
    d = fresh("ext4", "a2")
    r = me.ctl("create", d, "WPAbstract", "Team notes")
    p = Path(r.get("result", {}).get("path", ""))
    me.ctl("flush")
    content = p.read_text() if p.exists() else ""
    (d / "printer.wpobj").write_text("[WPS Object]\nVersion=1\nClass=WPPrinter\n")
    pr = obj(me, d / "printer.wpobj")
    me.stop()
    me.start()
    again = obj(me, p)
    me.stop()
    ok = r.get("ok") and p.suffix == ".wpobj" and "Class=WPAbstract" in content and \
        again.get("title") == "Team notes" and pr.get("class") == "WPAbstract" and pr.get("class_not_installed") == "WPPrinter"
    return ok, [f"created {p.name!r} containing Class=WPAbstract: {'Class=WPAbstract' in content}",
                f"after restart: {again.get('class')} {again.get('title')!r}",
                f"printer.wpobj (WPPrinter not installed): {pr.get('class')}, class_not_installed={pr.get('class_not_installed')!r}"]


@scenario("A3", "A shadow of a program object; creating never overwrites")
def sa3():
    me = Service("me").start()
    d = fresh("ext4", "a3")
    (d / "menu").mkdir()
    p1 = Path(me.ctl("create", d, "WPProgram", "Terminal", "ptyxis").get("result", {}).get("path", ""))
    p2 = Path(me.ctl("create", d, "WPProgram", "Terminal", "xterm").get("result", {}).get("path", ""))
    me.ctl("flush")
    link = Path(me.ctl("shadow", p1, d / "menu").get("result", ""))
    sh = obj(me, link)
    me.stop()
    ok = p1.name == "Terminal.desktop" and p2.name == "Terminal (2).desktop" and ini(p1, "Exec") == "ptyxis" and \
        sh.get("class") == "WPShadow" and (sh.get("target") or {}).get("class") == "WPProgram"
    return ok, [f"first: {p1.name!r}, second: {p2.name!r} (Exec of the first still {ini(p1, 'Exec')!r})",
                f"shadow in menu/ resolves to a {(sh.get('target') or {}).get('class')}"]


# -------------------------------------------------------------------- scale

def percentile(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * p))] if xs else None


@scenario(28, "A folder of 1,000,000 files: first page fast; other requests stay fast while it loads")
def s28():
    big = big_volume()
    d = big / "million"
    if not (d / "f0999999").exists():
        run(["rm", "-rf", str(d)])
        d.mkdir()
        for i in range(1000000):
            os.close(os.open(d / f"f{i:07}", os.O_CREAT | os.O_WRONLY, 0o644))
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
    first = c.request(op="list", target=str(d), offset=0, count=50, details=True)
    t_first = time.time() - t0
    r = c.request(op="list", target=str(d), offset=999950, count=50)   # answered when that far is read
    t_all = time.time() - t0
    stop[0] = True
    th.join()
    total = r.get("result", {}).get("total")
    last = [x["name"] for x in r.get("result", {}).get("items", [])]
    c.close()
    me.stop()
    fr = first.get("result", {})
    ok = fr.get("items") and len(fr["items"]) == 50 and fr["items"][0].get("size") == 0 and t_first < 0.15 and \
        total == 1000000 and len(last) == 50 and percentile(pings, 0.99) < 20
    return ok, [f"first page of 50 (with details) after {t_first * 1000:.0f} ms; complete=false then: {fr.get('complete') is False}",
                f"whole folder read in {t_all:.1f} s; total {total}; last page has {len(last)} entries",
                f"pings while loading: {len(pings)}, 99th percentile {percentile(pings, 0.99):.2f} ms, worst {max(pings):.2f} ms"]


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
        helper = subprocess.Popen(["sudo", "env", f"WPS_TRACE={os.environ.get('WPS_TRACE', '')}", str(BIN / "wps-watchd"), "-s", str(sock)],
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
