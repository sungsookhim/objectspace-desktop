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


# ------------------------------------------------- programs, templates, types

@scenario(37, "A program in a shared folder: another user is asked first, and it runs as whoever opens it")
def s37():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "s37", user=60001)
    out = d / "who-ran-it"
    run(["sh", "-c", f"touch {out} && chmod 666 {out}"], user=60001, check=True)
    p = Path(a.ctl("create", d, "WPProgram", "Who am I", f'sh -c "id -u >> {out}"').get("result", {}).get("path", ""))
    a.ctl("flush")
    asked = b.ctl("open", p).get("result", {})
    time.sleep(0.5)
    before = out.read_text().split()
    b.ctl("open", p, "confirm")
    a.ctl("open", p)                                   # its owner is not asked
    time.sleep(1.0)
    ran = out.read_text().split()
    a.stop(); b.stop()
    ok = asked.get("needs_confirmation") is True and "id -u" in asked.get("command", "") and before == [] and \
        sorted(ran) == ["60001", "60002"]
    return ok, [f"B is asked first: {asked.get('needs_confirmation')}, shown command {asked.get('command')!r}",
                f"nothing ran before confirming: {before == []}",
                f"after B confirms and A opens it: ran as UIDs {sorted(ran)}"]


@scenario("P2", "A system program: retitled for this user only; the system file is untouched")
def sp2():
    apps = sorted(Path("/usr/share/applications").glob("*.desktop"))
    target = next(p for p in apps if ini(p, "Type") == "Application" and ini(p, "Exec") and ini(p, "Name"))
    st0 = target.stat()
    a = Service("me").start()
    b = Service("B", user=60002).start()
    before = obj(a, target)
    r = a.call(target, "wpSetTitle", "My favourite tool")
    a.ctl("flush")
    mine, theirs = obj(a, target), obj(b, target)
    a.stop(); b.stop()
    st1 = target.stat()
    ok = before.get("class") == "WPProgram" and r.get("ok") and mine.get("title") == "My favourite tool" and \
        theirs.get("title") == ini(target, "Name") and st0.st_mtime == st1.st_mtime
    return ok, [f"{target.name}: {before.get('class')} {before.get('title')!r}",
                f"after retitling: mine {mine.get('title')!r}, another user's {theirs.get('title')!r}",
                f"system file unchanged: {st0.st_mtime == st1.st_mtime}"]


@scenario("P3", "Templates: a new object copies the settings, not the identity or the template mark")
def sp3():
    me = Service("me").start()
    d = fresh("ext4", "p3")
    (d / "letter.odt").write_text("Dear ...\n")
    me.call(d / "letter.odt", "wpSetTitle", "Letter")
    me.call(d / "letter.odt", "wpSetTemplate", "true")
    me.ctl("flush")
    tid = obj(me, d / "letter.odt")["id"]
    r = me.ctl("from_template", d / "letter.odt", d, "Letter to the bank")
    me.ctl("flush")
    n = r.get("result", {})
    newp = Path(n.get("path", ""))
    nobj = obj(me, newp)
    tpl = me.call(newp, "wpQueryTemplate").get("result")
    refused = me.ctl("from_template", newp, d, "x")
    me.stop()
    ok = r.get("ok") and newp.name == "Letter to the bank.odt" and newp.read_text() == "Dear ...\n" and \
        nobj.get("title") == "Letter to the bank" and nobj.get("id") and nobj.get("id") != tid and tpl is False and \
        not refused.get("ok")
    return ok, [f"new object {newp.name!r} with the template's contents", f"own ID: {nobj.get('id') != tid}, not a template: {tpl is False}",
                f"an ordinary object cannot be used as a template: {not refused.get('ok')}"]


@scenario("P4", "File types come from shared-mime-info")
def sp4():
    me = Service("me").start()
    d = fresh("ext4", "p4")
    want = {"photo.png": "image/png", "report.odt": "application/vnd.oasis.opendocument.text",
            "main.rs": "text/rust", "backup.tar.gz": "application/x-compressed-tar"}
    got = {}
    for name in want:
        (d / name).write_text("x")
        got[name] = me.call(d / name, "wpQueryMimeType").get("result")
    me.stop()
    return got == want, [f"{k}: {v}" for k, v in got.items()]


@scenario(41, "A root session warns, and does not follow other users' homes on its own")
def s41():
    r = Service("root", user=0).start()
    d = fresh("ext4", "s41")
    (d / "x.txt").write_text("x")
    obj(r, d / "x.txt")
    w0 = r.ctl("stats").get("result", {}).get("watched")
    home = Path(os.environ.get("HOME", "/home"))
    probe = next((p for p in home.iterdir() if p.is_file()), None)
    seen = obj(r, probe) if probe else {}
    w1 = r.ctl("stats").get("result", {}).get("watched")
    r.stop()
    log = r.log.read_text()
    ok = "running as root" in log and bool(seen) and w1 == w0
    return ok, [f"warning in the log: {'running as root' in log}",
                f"root can still open a file in a home explicitly: {bool(seen)}",
                f"but its folder is not watched: watched {w0} before, {w1} after"]


# ----------------------------------------------------------------- plug-ins

PLUG = {"WPS_PLUGIN_DIRS": f"{ROOT}/build/tests/m5plugins:{ROOT}/build/tests/m5future"}


def plugins_ready(svc, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        ps = svc.ctl("stats").get("result", {}).get("plugins", [])
        if any(p["ready"] for p in ps):
            return ps
        time.sleep(0.1)
    return svc.ctl("stats").get("result", {}).get("plugins", [])


def labels(menu):
    return [i["label"] for i in menu.get("result", [])] if menu.get("ok") else None


@scenario(17, "A plug-in replaces WPFolder: new folders are its class and get its menu item")
def s17():
    me = Service("me", extra_env=PLUG).start()
    ps = plugins_ready(me)
    d = fresh("ext4", "s17")
    (d / "projects").mkdir()
    o = obj(me, d / "projects")
    m = labels(me.ctl("menu", d / "projects"))
    st = me.call(d / "projects", "wpStamp")
    me.ctl("flush")
    me.stop()
    me.start()
    plugins_ready(me)
    t = obj(me, d / "projects").get("title")
    me.stop()
    ok = o.get("class") == "TestFolder" and m == ["Open", "Properties", "Arrange", "From the plug-in"] and \
        st.get("plugin") and t == "Stamped by the plug-in"
    return ok, [f"plug-ins: {[(p['name'], p['ready']) for p in ps]}", f"new folder is a {o.get('class')}; menu {m}",
                f"a setting changed in the host is saved by the service: {t!r}"]


@scenario(18, "A plug-in built against a newer WPFolder than installed is refused; the service carries on")
def s18():
    me = Service("me", extra_env=PLUG).start()
    ps = {p["name"]: p for p in plugins_ready(me)}
    pong = me.ctl("ping").get("ok")
    me.stop()
    fut = ps.get("future", {})
    ok = fut.get("ready") is False and "incompatible" in (fut.get("error") or "") and ps.get("testfolder", {}).get("ready") and pong
    return ok, [f"future.so: ready={fut.get('ready')}, error {fut.get('error')!r}",
                "(plug-ins built against older versions loading is covered by the runtime tests)",
                f"service still answers: {pong}"]


@scenario(19, "A plug-in hangs for 60 s in its menu code: others are unaffected; the parent's menu after the deadline")
def s19():
    me = Service("me", extra_env=PLUG).start()
    plugins_ready(me)
    d = fresh("ext4", "s19")
    (d / "slow").mkdir()
    me.call(d / "slow", "wpSetTitle", "SLOW")
    pings, stop = [], [False]
    import threading
    def pinger():
        c = Conn(me)
        while not stop[0]:
            t = time.time()
            c.request(op="ping")
            pings.append((time.time() - t) * 1000)
        c.close()
    th = threading.Thread(target=pinger)
    th.start()
    t0 = time.time()
    m = labels(me.ctl("menu", d / "slow"))
    took = time.time() - t0
    stop[0] = True
    th.join()
    pid = me.ctl("stats").get("result", {}).get("plugins", [{}])[0].get("pid")
    me.stop()
    pings.sort()
    p99 = pings[int(len(pings) * 0.99)] if pings else None
    ok = m == ["Open", "Properties", "Arrange"] and 1.8 < took < 4 and p99 is not None and p99 < 20
    return ok, [f"menu came back after {took:.1f} s with the parent's items: {m}",
                f"pings meanwhile: {len(pings)}, 99th percentile {p99:.2f} ms"]


@scenario(20, "The plug-in's host is killed mid-call: the parent's menu at once; the host restarts; wpsd keeps running")
def s20():
    me = Service("me", extra_env=PLUG).start()
    ps = plugins_ready(me)
    pid0 = next(p["pid"] for p in ps if p["name"] == "testfolder")
    d = fresh("ext4", "s20")
    (d / "slow").mkdir()
    me.call(d / "slow", "wpSetTitle", "SLOW")
    import threading
    res = {}
    def ask():
        t = time.time()
        res["menu"] = labels(me.ctl("menu", d / "slow"))
        res["took"] = time.time() - t
    th = threading.Thread(target=ask)
    th.start()
    time.sleep(0.5)
    run(["kill", "-9", str(pid0)])
    th.join()
    time.sleep(1.0)
    ps2 = {p["name"]: p for p in me.ctl("stats").get("result", {}).get("plugins", [])}
    tf = ps2.get("testfolder", {})
    (d / "fast").mkdir()
    again = labels(me.ctl("menu", d / "fast"))
    wpsd_alive = me.proc.poll() is None
    me.stop()
    ok = res.get("menu") == ["Open", "Properties", "Arrange"] and res.get("took", 9) < 1.5 and tf.get("ready") and \
        tf.get("pid") != pid0 and tf.get("restarts") == 1 and again and again[-1] == "From the plug-in" and wpsd_alive
    return ok, [f"menu during the crash: {res.get('menu')} after {res.get('took', 0):.2f} s",
                f"host restarted: pid {pid0} -> {tf.get('pid')}, restarts {tf.get('restarts')}",
                f"plug-in works again: {again}; wpsd never restarted: {wpsd_alive}"]


@scenario("SB", "The plug-in's sandbox: no home folder, no network, its data folder only")
def ssb():
    me = Service("me", extra_env=PLUG).start()
    ps = plugins_ready(me)
    d = fresh("ext4", "sb")
    (d / "x").mkdir()
    data = me.base / "state" / "plugins" / "testfolder"
    probe = me.call(d / "x", "wpSandboxProbe", str(data)).get("result", "")
    sb = next(p["sandbox"] for p in ps if p["name"] == "testfolder")
    me.stop()
    ok = "home=Permission denied" in probe and "net=Network is unreachable" in probe and "data=writable" in probe
    return ok, [f"sandbox: {sb}", f"probe from inside the plug-in: {probe}"]


# ------------------------------------------------------------------- rename

@scenario("R1", "Renaming a file's icon renames the file; without permission it becomes a private title")
def sr1():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "r1", user=60001)
    run(["sh", "-c", f"echo hi > {d}/draft.txt && echo x > {d}/taken.txt && chmod 755 {d}"], user=60001, check=True)
    a.call(d / "draft.txt", "wpSetTitle", "Old title")
    a.ctl("flush")
    id0 = obj(a, d / "draft.txt").get("id")
    r = a.ctl("rename", d / "draft.txt", "final.txt")
    a.ctl("flush")
    after = obj(a, d / "final.txt")
    clash = a.ctl("rename", d / "final.txt", "taken.txt")
    taken = (d / "taken.txt").read_text()
    # B cannot write A's folder: the new name stays B's title only
    rb = b.ctl("rename", d / "final.txt", "Bs name.txt")
    b.ctl("flush")
    seen_b, seen_a = obj(b, d / "final.txt").get("title"), obj(a, d / "final.txt").get("title")
    p = Path(a.ctl("create", d, "WPProgram", "Tool", "true").get("result", {}).get("path", ""))
    rp = a.ctl("rename", p, "My tool")
    a.ctl("flush")
    a.stop(); b.stop()
    ok = r.get("ok") and r.get("result") is True and not (d / "draft.txt").exists() and after.get("id") == id0 and \
        after.get("title") == "final.txt" and not clash.get("ok") and taken == "x\n" and \
        rb.get("ok") and rb.get("result") is False and (d / "final.txt").exists() and seen_b == "Bs name.txt" and \
        seen_a == "final.txt" and rp.get("ok") and p.exists() and rp.get("object", {}).get("title") == "My tool"
    return ok, [f"renamed: draft.txt -> final.txt, same ID {after.get('id') == id0}, title follows the name: {after.get('title')!r}",
                f"onto an existing name: refused ({clash.get('message')!r}), taken.txt untouched: {taken == 'x' + chr(10)}",
                f"B without permission: file kept its name; B sees {seen_b!r}, A sees {seen_a!r}",
                f"a program object: only its title changes ({rp.get('object', {}).get('title')!r}), file kept"]


# --------------------------------------------------------------- positions

@scenario("L1", "Icon positions: placed by ID, kept across restarts, personal in others' folders, cleared by Arrange")
def sl1():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "l1", user=60001)
    run(["sh", "-c", f"touch {d}/one.txt {d}/two.txt && chmod 755 {d}"], user=60001, check=True)
    pl = a.ctl("place", d, d / "one.txt", 40, 30, d / "two.txt", 200, 90)
    ids = pl.get("result") or []
    a.ctl("flush")
    a.stop(); a.start()
    lay_a = a.ctl("layout", d).get("result", {}).get("positions", {})
    # B cannot write A's folder: B's placement stays B's
    b.ctl("place", d, d / "one.txt", 500, 500)
    b.ctl("flush")
    lay_b = b.ctl("layout", d).get("result", {}).get("positions", {})
    lay_a2 = a.ctl("layout", d).get("result", {}).get("positions", {})
    named = {i["name"]: (i["x"], i["y"]) for i in a.ctl("layout", d).get("result", {}).get("items", [])}
    ar = a.ctl("arrange", d)
    a.ctl("flush")
    lay_a3 = a.ctl("layout", d).get("result", {}).get("positions", {})
    a.stop(); b.stop()
    ok = len(ids) == 2 and all(ids) and lay_a.get(ids[0]) == "40,30" and lay_a.get(ids[1]) == "200,90" and \
        lay_b.get(ids[0]) == "500,500" and lay_a2.get(ids[0]) == "40,30" and ar.get("result") == 2 and lay_a3 == {} and \
        named == {"one.txt": (40, 30), "two.txt": (200, 90)}
    return ok, [f"placed with IDs {ids}", f"after a restart A sees {lay_a}",
                f"B (not the folder's owner) sees one.txt at {lay_b.get(ids[0]) if ids else None}; A still at {lay_a2.get(ids[0]) if ids else None}",
                f"by name: {named}", f"Arrange cleared {ar.get('result')} positions: now {lay_a3}"]


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
