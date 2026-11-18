#!/usr/bin/env python3
"""M5 scenarios: events, menus and settings as data, programs, templates,
file types, root sessions, the plug-in host.

usage: tests/m5/m5.py [scenario ...]      (needs the test bed; uses sudo)
"""
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
os.environ.setdefault("WPS_SUITE", "m5")
import harness  # noqa: E402
from harness import *  # noqa: E402,F401,F403


def obj(svc, target):
    r = svc.ctl("resolve", target)
    return r.get("result", {}) if r.get("ok") else {}


def ini(path, key):
    for line in Path(path).read_text().splitlines():
        if line.startswith(key + "="):
            return line[len(key) + 1:]
    return None


# ------------------------------------------------------------------- events

@scenario("E1", "Folder events: created, renamed and deleted files arrive within a second")
def se1():
    me = Service("me").start()
    d = fresh("ext4", "e1")
    c = Conn(me)
    c.request(op="subscribe", folders=[str(d)])
    t0 = time.time()
    (d / "new.txt").write_text("x")
    added = c.events(2, until=lambda e: e["event"] == "content_added")
    t_add = time.time() - t0
    os.rename(d / "new.txt", d / "renamed.txt")
    moved = c.events(2, until=lambda e: e["event"] == "object_moved")
    os.unlink(d / "renamed.txt")
    removed = c.events(2, until=lambda e: e["event"] == "content_removed")
    c.close()
    me.stop()
    a = [e for e in added if e["event"] == "content_added"]
    m = [e for e in moved if e["event"] == "object_moved"]
    r = [e for e in removed if e["event"] == "content_removed"]
    ok = a and a[-1]["name"] == "new.txt" and m and m[-1]["from"] == "new.txt" and m[-1]["to"] == "renamed.txt" and \
        r and r[-1]["name"] == "renamed.txt" and t_add < 1.0
    return ok, [f"content_added {a[-1]['name'] if a else None} after {t_add * 1000:.0f} ms",
                f"object_moved {m[-1]['from'] if m else None} -> {m[-1]['to'] if m else None}",
                f"content_removed {r[-1]['name'] if r else None}"]


@scenario("E2", "Object events: a change made by another client is pushed with the new values")
def se2():
    me = Service("me").start()
    d = fresh("ext4", "e2")
    (d / "doc.txt").write_text("x")
    me.call(d / "doc.txt", "wpSetTitle", "First")
    me.ctl("flush")
    oid = obj(me, d / "doc.txt")["id"]
    c = Conn(me)
    c.request(op="subscribe", objects=[f"id:{oid}"])
    me.call(d / "doc.txt", "wpSetTitle", "Second")        # another client
    ev = c.events(2, until=lambda e: e["event"] == "object_changed")
    c.close()
    me.stop()
    ch = [e for e in ev if e["event"] == "object_changed"]
    ok = ch and ch[-1]["object"]["title"] == "Second"
    return ok, [f"object_changed pushed: {bool(ch)}, title {ch[-1]['object']['title'] if ch else None!r}"]


@scenario("E3", "Job events: progress and completion are pushed")
def se3():
    me = Service("me").start()
    src, dst = fresh("ext4", "e3src"), fresh("btrfs", "e3dst")
    for i in range(3000):
        (src / f"f{i:04}").write_bytes(b"x" * 1024)
    c = Conn(me)
    c.request(op="subscribe", jobs=True)
    sel = c.request(op="select", folder=str(src), all=True, **{"except": []})["result"]
    jid = c.request(op="copy", selection=sel, dest=str(dst))["result"]
    ev = c.events(30, until=lambda e: e["event"] == "job" and e["status"]["state"] == "done")
    c.close()
    me.stop()
    jobs = [e["status"] for e in ev if e["event"] == "job" and e["status"]["job"] == jid]
    states = [j["state"] for j in jobs]
    progress = sorted({j["items_done"] for j in jobs})
    ok = states and states[-1] == "done" and len(progress) >= 2
    return ok, [f"job events: {len(jobs)}, states {sorted(set(states))}, distinct progress values {len(progress)}"]


# ------------------------------------------------------- menus and settings

@scenario("S1", "Menus as data: each class adds its items; selecting one is handled")
def ss1():
    me = Service("me").start()
    d = fresh("ext4", "s1m")
    (d / "f.txt").write_text("x")
    folder = me.ctl("menu", d).get("result", [])
    data = me.ctl("menu", d / "f.txt").get("result", [])
    sel = me.ctl("menu_select", d / "f.txt", 2)
    me.stop()
    fl = [i["label"] for i in folder]
    dl = [i["label"] for i in data]
    ok = fl == ["Open", "Properties", "Arrange"] and dl == ["Open", "Properties", "Open With"] and sel.get("ok")
    return ok, [f"folder menu: {fl}", f"data file menu: {dl}", f"selecting Properties: ok={sel.get('ok')}"]


@scenario("S2", "Settings pages as data; changing a value goes through the class's setter")
def ss2():
    me = Service("me").start()
    d = fresh("ext4", "s2m")
    p = Path(me.ctl("create", d, "WPProgram", "Viewer", "eog").get("result", {}).get("path", ""))
    pages = me.ctl("settings", p).get("result", [])
    changes = obj(me, p).get("changes")
    r = me.ctl("settings_set", p, "command=loupe", "title=Image Viewer")
    me.ctl("flush")
    stale = harness.run([str(BIN / "wpsctl"), "call", "--expect", str(changes), str(p), "wpSetTitle", "x"],
                        env=me.env).stdout
    me.stop()
    titles = [pg["title"] for pg in pages]
    fields = {f["key"]: f["value"] for pg in pages for f in pg["fields"]}
    ok = titles == ["General", "Program"] and fields.get("command") == "eog" and r.get("ok") and \
        ini(p, "Exec") == "loupe" and ini(p, "Name") == "Image Viewer" and '"conflict"' in stale
    return ok, [f"pages {titles}, fields {fields}", f"after settings_set: Exec={ini(p, 'Exec')!r} Name={ini(p, 'Name')!r}",
                f"a write based on an old view is refused: {'conflict' in stale}"]


# ------------------------------------------------------------------- driver

def main():
    prepare()
    for f in OUT.glob("*.log"):
        f.unlink()
    reset_services()
    run_scenarios(set(sys.argv[1:]))
    finish()


if __name__ == "__main__":
    main()
