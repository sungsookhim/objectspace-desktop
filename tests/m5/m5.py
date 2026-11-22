#!/usr/bin/env python3
"""M5 scenarios: events, menus and settings as data, programs, templates,
file types, root sessions, the plug-in host.

usage: tests/m5/m5.py [scenario ...]      (needs the test bed; uses sudo)
"""
import os
import shutil
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
    ok = fl == ["Open", "Properties", "Create another", "Delete...", "Arrange"] and dl == ["Open", "Properties", "Create another", "Delete..."] and sel.get("ok")
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
        nobj.get("title") in ("Letter to the bank.odt", "Letter to the bank") and nobj.get("id") != tid and tpl is False and \
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
    ok = o.get("class") == "TestFolder" and m == ["Open", "Properties", "Create another", "Delete...", "Arrange", "From the plug-in"] and \
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
    ok = m == ["Open", "Properties", "Create another", "Delete...", "Arrange"] and 1.8 < took < 4 and p99 is not None and p99 < 20
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
    ok = res.get("menu") == ["Open", "Properties", "Create another", "Delete...", "Arrange"] and res.get("took", 9) < 1.5 and tf.get("ready") and \
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


# ----------------------------------------------------------- central objects

def wait_for(fn, timeout=5.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        r = fn()
        if r:
            return r
        time.sleep(step)
    return fn()


@scenario("C1", "Central objects: shadows kept centrally by default, listed with the folder, moved, copied, placed")
def sc1():
    me = Service("me").start()
    d = fresh("ext4", "c1")
    for sub in ("docs", "desk", "other", "keepfiles"):
        (d / sub).mkdir()
    (d / "docs" / "report.txt").write_text("x")
    me.call(d / "docs" / "report.txt", "wpSetTitle", "Report")
    r = me.ctl("shadow", d / "docs" / "report.txt", d / "desk")
    addr = r.get("result", "")
    files_in_desk = sorted(os.listdir(d / "desk"))
    page = me.ctl("list", d / "desk", 0, 10, "details").get("result", {}).get("items", [])
    sh = obj(me, addr)
    mv = me.ctl("vmove", addr, d / "other").get("result", "")
    in_desk = me.ctl("list", d / "desk", 0, 10).get("result", {}).get("items", [])
    in_other = me.ctl("list", d / "other", 0, 10).get("result", {}).get("items", [])
    cp = me.ctl("vmove", mv, d / "desk", "copy").get("result", "")
    me.ctl("place", d / "desk", cp, 120, 80)
    me.ctl("flush")
    lay = {i["name"]: (i["x"], i["y"]) for i in me.ctl("layout", d / "desk").get("result", {}).get("items", [])}
    # a folder set to keep files gets a .wpobj file
    me.call(d / "keepfiles", "wpSetLocalObjects", "true")
    me.ctl("flush")
    lr = me.ctl("shadow", d / "docs" / "report.txt", d / "keepfiles").get("result", "")
    # deleting the target removes its central shadows
    os.unlink(d / "docs" / "report.txt")
    gone = wait_for(lambda: not me.ctl("list", d / "other", 0, 10).get("result", {}).get("items", []) and
                    not me.ctl("list", d / "desk", 0, 10).get("result", {}).get("items", []), timeout=8)
    me.stop()
    entry = page[0] if page else {}
    ok = addr.startswith("vobj:") and files_in_desk == [] and entry.get("name", "").startswith("\x01vobj:") and \
        entry.get("shadow") is True and entry.get("title") == "Report" and sh.get("class") == "WPShadow" and \
        (sh.get("target") or {}).get("title") == "Report" and mv == addr and in_desk == [] and len(in_other) == 1 and \
        cp.startswith("vobj:") and cp != mv and lay.get("\x01" + cp) == (120, 80) and lr.endswith(".wpobj") and bool(gone)
    return ok, [f"shadow made centrally: {addr[:13]}..., files in desk/: {files_in_desk}",
                f"listed with the folder: shadow={entry.get('shadow')}, title {entry.get('title')!r}; resolves to {(sh.get('target') or {}).get('title')!r}",
                f"moved to other/ (same ID {mv == addr}): desk lists {len(in_desk)}, other lists {len(in_other)}",
                f"copied back to desk/ with its own ID; placed at {lay.get(chr(1) + cp)}",
                f"in a folder set to keep files: {Path(lr).name if lr else None}",
                f"target deleted: its central shadows gone: {bool(gone)}"]


@scenario("C2", "Dropped copies land where they were dropped, after any renaming")
def sc2():
    me = Service("me").start()
    d = fresh("ext4", "c2")
    (d / "src").mkdir()
    (d / "dst").mkdir()
    (d / "src" / "a.txt").write_text("a")
    (d / "dst" / "a.txt").write_text("taken")
    c = Conn(me)
    sel = c.request(op="select", folder=str(d / "src"), all=False, items=["a.txt"]).get("result")
    job = c.request(op="copy", selection=sel, dest=str(d / "dst"), positions=[{"name": "a.txt", "x": 333, "y": 44}]).get("result")
    def state():
        st = c.request(op="job", job=job).get("result") or {}
        if st.get("state") == "needs-decision":
            c.request(op="job_decide", job=job, decision="rename", all=False)
        return st.get("state")
    wait_for(lambda: state() == "done", timeout=10)
    c.close()
    me.ctl("flush")
    lay = {i["name"]: (i["x"], i["y"]) for i in me.ctl("layout", d / "dst").get("result", {}).get("items", [])}
    me.stop()
    ok = lay.get("a (2).txt") == (333, 44) and "a.txt" not in lay
    return ok, [f"copied under a clash, renamed: positions {lay}"]


# ------------------------------------------------------------------- in use

@scenario("U1", "In use: files opened from WPS show as open while their program runs; lock files mark files opened elsewhere")
def su1():
    d = fresh("ext4", "u1")
    xdg = WORK / "u1-xdg"
    shutil.rmtree(xdg, ignore_errors=True)
    apps = xdg / "data" / "applications"
    apps.mkdir(parents=True)
    (xdg / "config").mkdir()
    (xdg / "sys").mkdir()
    log = xdg / "args"
    hold, fork = xdg / "hold.sh", xdg / "fork.sh"
    # One viewer holds the file for 2 s; one forks a child that does, and exits at once.
    hold.write_text(f'#!/bin/sh\necho "$@" >> {log}\nsleep 2\n')
    fork.write_text('#!/bin/sh\n( sleep 2 ) &\nexit 0\n')
    for f in (hold, fork):
        f.chmod(0o755)
    (apps / "wps-hold.desktop").write_text(f"[Desktop Entry]\nType=Application\nName=Hold\nExec={hold} --view %f\n")
    (apps / "wps-fork.desktop").write_text(f"[Desktop Entry]\nType=Application\nName=Fork\nExec={fork} %U\n")
    (xdg / "config" / "mimeapps.list").write_text(
        "[Default Applications]\ntext/plain=missing.desktop;wps-hold.desktop;\nimage/png=wps-fork.desktop\n")
    me = Service("me", extra_env={"XDG_CONFIG_HOME": str(xdg / "config"), "XDG_DATA_HOME": str(xdg / "data"),
                                  "XDG_CONFIG_DIRS": str(xdg / "sys")}).start()
    for n in ("doc.txt", "pic.png", "sheet.ods"):
        (d / n).write_text("x")
    c = Conn(me)
    c.request(op="subscribe", folders=[str(d)])

    def in_use(name):
        items = c.request(op="list", target=str(d), offset=0, count=100, details=True).get("result", {}).get("items", [])
        return {i["name"]: i.get("in_use", False) for i in items}.get(name)

    def ev_for(name, value, timeout):
        evs = c.events(timeout, until=lambda e: e["event"] == "in_use" and e["path"].endswith("/" + name) and e["in_use"] == value)
        return any(e["event"] == "in_use" and e["path"].endswith("/" + name) and e["in_use"] == value for e in evs)

    out = []
    r = c.request(op="open", target=str(d / "doc.txt")).get("result", {})
    on = ev_for("doc.txt", True, 2)
    listed_on = in_use("doc.txt")
    os.rename(d / "doc.txt", d / "moved.txt")              # renamed while open: still open under the new name
    moved_on = wait_for(lambda: in_use("moved.txt"), timeout=2)
    off = ev_for("moved.txt", False, 4)
    listed_off = in_use("moved.txt")
    args = log.read_text().split() if log.exists() else []
    out += [f"doc.txt opened with {r.get('command')!r}, args {args}", f"in use: event {on}, listed {listed_on}; "
            f"after renaming, moved.txt listed {moved_on}", f"program ended: event {off}, listed {listed_off}"]
    ok = r.get("command") == str(hold) and args == ["--view", str(d / "doc.txt")] and on and listed_on and moved_on and \
        off and listed_off is False

    t0 = time.time()
    c.request(op="open", target=str(d / "pic.png"))
    fon = ev_for("pic.png", True, 2)
    still = (time.sleep(0.8), in_use("pic.png"))[1]           # the opener exited; its child holds it
    foff = ev_for("pic.png", False, 4)
    held = time.time() - t0
    out.append(f"a program that forks and exits: in use {fon}, after its parent exited {still}, "
               f"released after {held:.1f} s ({foff})")
    ok = ok and fon and still and foff and held >= 1.8

    (d / ".~lock.sheet.ods#").write_text("x")
    locked = in_use("sheet.ods")
    (d / ".~lock.sheet.ods#").unlink()
    unlocked = in_use("sheet.ods")
    (d / ".doc2.txt.swp").write_text("x")
    (d / "doc2.txt").write_text("x")
    vim = in_use("doc2.txt")
    os.symlink("user@host.1234", d / ".#notes.org")
    (d / "notes.org").write_text("x")
    emacs = in_use("notes.org")
    out.append(f"lock files: LibreOffice {locked} (gone: {unlocked}), vim {vim}, Emacs {emacs}")
    ok = ok and locked and unlocked is False and vim and emacs
    c.close()
    me.stop()
    return ok, out


@scenario("U2", "Opening: links show and open as what they point to; executables run in their folder (text-mode ones in a terminal); untyped text opens as text")
def su2():
    d = fresh("ext4", "u2", user=None)
    run(["sudo", "chmod", "1777", str(d)], check=True)
    xdg = WORK / "u2-xdg"
    shutil.rmtree(xdg, ignore_errors=True)
    (xdg / "data" / "applications").mkdir(parents=True)
    (xdg / "config").mkdir()
    (xdg / "sys").mkdir()
    log = xdg / "args"
    hold = xdg / "hold.sh"
    hold.write_text(f'#!/bin/sh\necho "$@" >> {log}\n')
    hold.chmod(0o755)
    (xdg / "data" / "applications" / "wps-hold.desktop").write_text(f"[Desktop Entry]\nType=Application\nName=Hold\nExec={hold} %f\n")
    (xdg / "config" / "mimeapps.list").write_text("[Default Applications]\ntext/plain=wps-hold.desktop\n")
    term, tlog = xdg / "term.sh", xdg / "term-args"
    term.write_text(f'#!/bin/sh\nprintf "%s\\n" "$*" >> {tlog}\nshift\nexec "$@"\n')   # a stand-in terminal: logs, then runs
    term.chmod(0o755)
    me = Service("me", extra_env={"XDG_CONFIG_HOME": str(xdg / "config"), "XDG_DATA_HOME": str(xdg / "data"),
                                  "XDG_CONFIG_DIRS": str(xdg / "sys"), "WPS_TERMINAL": f"{term} -e"}).start()
    (d / "sub").mkdir()
    (d / "doc.txt").write_text("x")
    os.symlink(d / "sub", d / "to-sub")
    os.symlink(d / "doc.txt", d / "to-doc")
    os.symlink(d / "gone", d / "dangling")
    where = d / "ran-in"
    (d / "tool.sh").write_text(f"#!/bin/sh\npwd > {where}\n")
    (d / "tool.sh").chmod(0o755)
    (d / "README").write_text("plain words, no extension\n")
    write_as(60001, d / "theirs.sh", "#!/bin/sh\ntouch /tmp/never", mode="0755")
    tui = d / "top.desktop"
    tui.write_text(f"[Desktop Entry]\nType=Application\nName=Top\nExec=sh -c \"echo tui > {d}/tui-ran\"\nTerminal=true\n")
    c = Conn(me)
    items = c.request(op="list", target=str(d), offset=0, count=100, details=True).get("result", {}).get("items", [])
    links = {i["name"]: (i.get("target"), i.get("target_path")) for i in items if i["type"] == "link"}
    folder = c.request(op="open", target=str(d / "to-sub")).get("result", {})
    ran = c.request(op="open", target=str(d / "tool.sh")).get("result", {})
    asked = c.request(op="open", target=str(d / "theirs.sh")).get("result", {})
    text = c.request(op="open", target=str(d / "README")).get("result", {})
    via = c.request(op="open", target=str(d / "to-doc")).get("result", {})
    c.request(op="open", target=str(tui))
    c.close()
    time.sleep(0.8)
    me.stop()
    tl = tlog.read_text().splitlines() if tlog.exists() else []
    in_term = [l for l in tl if "tool.sh" in l] == [f"-e {d / 'tool.sh'}"] and any("tui-ran" in l for l in tl) and \
        (d / "tui-ran").exists()
    args = log.read_text().split() if log.exists() else []
    ranin = where.read_text().strip() if where.exists() else None
    ok = links == {"to-sub": ("folder", str(d / "sub")), "to-doc": ("file", str(d / "doc.txt")), "dangling": ("missing", "")} and \
        folder.get("folder") == str(d / "sub") and ran.get("command") == str(d / "tool.sh") and ranin == str(d) and \
        asked.get("needs_confirmation") is True and text.get("command") == str(hold) and via.get("command") == str(hold) and \
        args == [str(d / "README"), str(d / "doc.txt")] and in_term
    return ok, [f"in the terminal: {tl}", f"links listed as: {links}", f"link to a folder opens it: {folder.get('folder')}",
                f"tool.sh ran: {ran.get('command')!r}, in {ranin}", f"another user's script asks first: {asked.get('needs_confirmation')}",
                f"README and a link to doc.txt opened with the text default: {text.get('command') == str(hold)}, {via.get('command') == str(hold)}, args {args}"]


# ------------------------------------------------------------------- delete

@scenario("D1", "Delete: items go to the freedesktop trash of their filesystem, keeping their identity; for good from the trash; central objects deleted")
def sd1():
    d, b = fresh("ext4", "d1"), fresh("btrfs", "d1")
    home = MNT["ext4"] / "d1-home"
    run(["sudo", "rm", "-rf", str(home)])
    home.mkdir()
    uid = os.getuid()
    btrash = MNT["btrfs"] / f".Trash-{uid}"
    for n in ("info/c.txt.trashinfo", "info/c.txt.2.trashinfo", "files/c.txt", "files/c.txt.2"):
        run(["rm", "-rf", str(btrash / n)])
    me = Service("me", extra_env={"XDG_DATA_HOME": str(home)}).start()
    (d / "a.txt").write_text("a")
    (d / "sub").mkdir()
    (d / "sub" / "inner.txt").write_text("i")
    me.call(d / "a.txt", "wpSetTitle", "Letter A")
    me.ctl("flush")
    aid = obj(me, d / "a.txt").get("id")
    (b / "c.txt").write_text("c")
    c = Conn(me)

    def job(kind, folder, names):
        sel = c.request(op="select", folder=str(folder), all=False, items=names)["result"]
        jid = c.request(op=kind, selection=sel).get("result")
        st = {}
        def done():
            nonlocal st
            st = c.request(op="job", job=jid).get("result") or {}
            return st.get("state") in ("done", "failed", "cancelled")
        wait_for(done, timeout=10)
        return st

    st1 = job("trash", d, ["a.txt", "sub"])
    ht = home / "Trash"
    info_a = ini(ht / "info" / "a.txt.trashinfo", "Path") if (ht / "info" / "a.txt.trashinfo").exists() else None
    moved = (ht / "files" / "a.txt").exists() and (ht / "files" / "sub" / "inner.txt").exists() and not (d / "a.txt").exists()
    me.ctl("flush")
    same = obj(me, ht / "files" / "a.txt")
    (b / "c.txt.new").write_text("c2")
    st2 = job("trash", b, ["c.txt"])
    os.rename(b / "c.txt.new", b / "c.txt")
    st3 = job("trash", b, ["c.txt"])                       # a second c.txt: c.txt.2
    info_c = ini(btrash / "info" / "c.txt.trashinfo", "Path") if (btrash / "info" / "c.txt.trashinfo").exists() else None
    two = (btrash / "files" / "c.txt").exists() and (btrash / "files" / "c.txt.2").exists()
    st4 = job("trash", btrash / "files", ["c.txt.2"])     # already in the trash: no trash takes it
    st5 = job("delete", btrash / "files", ["c.txt.2"])    # for good, with its info file
    gone = not (btrash / "files" / "c.txt.2").exists() and not (btrash / "info" / "c.txt.2.trashinfo").exists()
    # a central shadow is deleted outright; its original stays
    (d / "orig.txt").write_text("o")
    addr = me.ctl("shadow", d / "orig.txt", d).get("result", "")
    vd = c.request(op="vdelete", target=addr)
    left = [i["name"] for i in c.request(op="list", target=str(d), offset=0, count=50).get("result", {}).get("items", [])]
    c.close()
    me.stop()
    ok = st1.get("state") == "done" and moved and info_a == str(d / "a.txt") and same.get("id") == aid and \
        same.get("title") == "Letter A" and same.get("trashed") is True and \
        st2.get("state") == st3.get("state") == "done" and two and info_c == "d1/c.txt" and \
        st4.get("not_trashed") == ["c.txt.2"] and st5.get("state") == "done" and gone and \
        vd.get("ok") and addr.startswith("vobj:") and left == ["orig.txt"]
    return ok, [f"home filesystem: moved {moved}, Path {info_a!r}; same object {same.get('id') == aid}, "
                f"title {same.get('title')!r}, trashed {same.get('trashed')}",
                f"other filesystem: {btrash.name}, Path {info_c!r}, a second c.txt kept as c.txt.2: {two}",
                f"in the trash already: not_trashed {st4.get('not_trashed')}; deleted for good with its info: {gone}",
                f"central shadow deleted: {vd.get('ok')}, folder now {left}"]


# ---------------------------------------------------------------- templates

@scenario("TP1", "Templates: built-in Folder, Data File and Program; anything in the templates folder; new objects from each")
def stp1():
    d = fresh("ext4", "tp1")
    tdir, dest = d / "Templates", d / "dest"
    tdir.mkdir()
    dest.mkdir()
    (tdir / "Letter.odt").write_text("Dear ...\n")
    (tdir / "Project").mkdir()
    (tdir / "Project" / "README").write_text("about\n")
    (tdir / "Project" / "src").mkdir()
    (d / "plain.txt").write_text("x")
    env = {"WPS_TEMPLATES_DIR": str(tdir)}
    me = Service("me", extra_env=env).start()
    c = Conn(me)

    def listing(folder):
        return c.request(op="list", target=str(folder), offset=0, count=100, details=True).get("result", {}).get("items", [])

    items = wait_for(lambda: [i for i in listing(tdir) if i.get("makes")] and listing(tdir), timeout=5) or []
    builtins = {i["title"]: (i["makes"], i.get("template")) for i in items if i.get("makes")}
    files = {i["name"]: i.get("template") for i in items if not i.get("makes")}
    made = {}
    for i in items:
        target = i["name"][1:] if i["name"].startswith("\x01") else str(tdir / i["name"])
        r = c.request(op="from_template", target=target, folder=str(dest))
        made[i.get("title") or i["name"]] = r.get("ok")
    refused = c.request(op="from_template", target=str(d / "plain.txt"), folder=str(dest))
    me.ctl("flush")
    got = sorted(os.listdir(dest))
    proj_ok = (dest / "Project" / "README").read_text() == "about\n" and (dest / "Project" / "src").is_dir()
    tid, cid = obj(me, tdir / "Project").get("id"), obj(me, dest / "Project").get("id")
    ids = not cid or cid != tid   # never the template's ID (IDs come lazily: none yet is fine)
    kinds = {"Folder": (dest / "Folder").is_dir(), "Data File": (dest / "Data File").is_file(),
             "Program": any(n.startswith("Program") and n.endswith(".desktop") for n in got)}
    # once made, never again: delete one, restart
    folder_tpl = next((i["name"][1:] for i in items if i.get("makes") == "WPFolder"), "")
    c.request(op="vdelete", target=folder_tpl)
    c.close()
    me.stop()
    me = Service("me", extra_env=env).start()
    c = Conn(me)
    time.sleep(1)
    again = sorted(i["title"] for i in listing(tdir) if i.get("makes"))
    c.close()
    me.stop()
    ok = builtins == {"Folder": ("WPFolder", True), "Data File": ("WPDataFile", True), "Program": ("WPProgram", True)} and \
        files == {"Letter.odt": True, "Project": True} and all(made.values()) and len(made) == 5 and \
        not refused.get("ok") and all(kinds.values()) and "Letter.odt" in got and proj_ok and ids and \
        again == ["Data File", "Program"]
    return ok, [f"built-in templates: {builtins}", f"in the templates folder: {files}",
                f"made from each: {made}; in dest: {got}", f"kinds right: {kinds}; folder template copied with its contents: {proj_ok}, own ID: {ids}",
                f"an ordinary file is not a template: {not refused.get('ok')}", f"after deleting Folder and restarting: {again}"]


# ------------------------------------------------------------- big folders

@scenario("B1", "A big folder follows changes entry by entry: versions step by one, nothing read again, no half-copied items listed")
def sb1():
    d = fresh("ext4", "b1")
    big = d / "big"
    big.mkdir()
    for i in range(20000):
        (big / f"f{i:05}").write_bytes(b"")
    src = d / "src"
    src.mkdir()
    for i in range(200):
        (src / f"c{i:03}").write_bytes(b"x" * 65536)
    me = Service("me").start()
    c = Conn(me)
    def page(off, n=10):
        return c.request(op="list", target=str(big), offset=off, count=n).get("result", {})
    first = wait_for(lambda: (lambda r: r if r.get("complete") else None)(page(0, 1)), timeout=20) or {}
    total0, v0 = first.get("total"), first.get("version")
    c.request(op="subscribe", folders=[str(big)])
    out, ok = [], True

    def event(kind):
        evs = c.events(3, until=lambda e: e["event"] == kind)
        e = [x for x in evs if x["event"] == kind]
        return e[-1] if e else {}

    t0 = time.time()
    (big / "new-one").write_bytes(b"")
    a = event("content_added")
    tail = page(total0, 5)
    t_add = time.time() - t0
    names = [i["name"] for i in page(0, total0 + 1).get("items", [])]
    idx = names.index("f00010")   # directory order, not sorted
    os.rename(big / "f00010", big / "renamed")
    m = event("object_moved")
    at10 = page(idx, 1).get("items", [{}])[0].get("name")
    os.unlink(big / "f00020")
    r = event("content_removed")
    after = page(0, 1)
    ok = v0 and a.get("version") == v0 + 1 and a.get("type") == "file" and [i["name"] for i in tail.get("items", [])] == ["new-one"] and \
        tail.get("version") == v0 + 1 and m.get("version") == v0 + 2 and at10 == "renamed" and \
        r.get("version") == v0 + 3 and after.get("total") == total0 and after.get("version") == v0 + 3
    out.append(f"{total0} entries, version {v0}; added: event version {a.get('version')} type {a.get('type')}, "
               f"listed at the end {[i['name'] for i in tail.get('items', [])]} after {t_add * 1000:.0f} ms")
    out.append(f"renamed: event version {m.get('version')}, still at index {idx} as {at10!r}; "
               f"removed: version {r.get('version')}, total {after.get('total')}")
    # a copy into the folder: its temporary names are never listed
    seen_tmp = False
    sel = c.request(op="select", folder=str(src), all=True, **{"except": []})["result"]
    jid = c.request(op="copy", selection=sel, dest=str(big))["result"]
    end = time.time() + 20
    while time.time() < end:
        res = page(total0, 400)
        seen_tmp |= any(i["name"].startswith(".wps-tmp-") for i in res.get("items", []))
        st = c.request(op="job", job=jid).get("result") or {}
        if st.get("state") == "done":
            break
    final = page(0, 1)
    c.close()
    me.stop()
    ok = ok and not seen_tmp and final.get("total") == total0 + 200
    out.append(f"copy of 200 items in: half-copied names listed {seen_tmp}; total now {final.get('total')} (version {final.get('version')})")
    return ok, out


# ----------------------------------------------------------- settings objects

@scenario("G3", "Settings objects: System, Sound, Mouse, Scheme Palette made once; their pages; changes kept; the GUI reads them all")
def sg3():
    d = fresh("ext4", "g3")
    env = {"WPS_SETUP_DIR": str(d / "System Setup")}
    me = Service("me", extra_env=env).start()
    c = Conn(me)
    setup = wait_for(lambda: (lambda r: r if len(r.get("objects", {})) == 4 else None)(c.request(op="setup").get("result", {})), timeout=5) or {}
    objs = setup.get("objects", {})
    listed = sorted(i.get("title", "") for i in c.request(op="list", target=str(d / "System Setup"), offset=0, count=10, details=True).get("result", {}).get("items", []))
    sysaddr = objs.get("WPSystem", {}).get("address", "")
    pages = [p["title"] for p in c.request(op="settings", target=sysaddr).get("result", [])]
    r1 = c.request(op="settings_set", target=sysaddr, values={"corner_radius": 3, "confirm_delete": False})
    r2 = c.request(op="settings_set", target=objs.get("WPSound", {}).get("address", ""), values={"window.open": "bell", "volume": "40"})
    bad = c.request(op="settings_set", target=sysaddr, values={"buttons": 9})
    after = c.request(op="setup").get("result", {}).get("objects", {})
    c.close()
    me.stop()
    me = Service("me", extra_env=env).start()          # kept, and not made twice
    c = Conn(me)
    again = c.request(op="setup").get("result", {}).get("objects", {})
    n_listed = len(c.request(op="list", target=str(d / "System Setup"), offset=0, count=10).get("result", {}).get("items", []))
    c.close()
    me.stop()
    sysv, sndv = after.get("WPSystem", {}).get("settings", {}), after.get("WPSound", {}).get("settings", {})
    ok = sorted(objs) == ["WPMouse", "WPSchemePalette", "WPSound", "WPSystem"] and \
        listed == ["Mouse", "Scheme Palette", "Sound", "System"] and pages == ["General", "Window", "Confirmations"] and \
        r1.get("ok") and r2.get("ok") and not bad.get("ok") and sysv.get("corner_radius") == "8" and \
        sysv.get("confirm_delete") == "0" and sndv.get("window.open") == "bell" and sndv.get("volume") == "40" and \
        objs.get("WPMouse", {}).get("settings", {}).get("open") == "button1 double" and \
        again.get("WPSystem", {}).get("settings", {}).get("corner_radius") == "8" and n_listed == 4 and \
        again.get("WPSystem", {}).get("address") == sysaddr
    return ok, [f"objects {sorted(objs)}; in the System Setup folder: {listed}", f"System's pages: {pages}",
                f"after changes: corner_radius {sysv.get('corner_radius')!r} (choice 3), confirm_delete {sysv.get('confirm_delete')!r}, "
                f"window.open {sndv.get('window.open')!r}, volume {sndv.get('volume')!r}; a bad choice refused {not bad.get('ok')}",
                f"after a restart: corner_radius {again.get('WPSystem', {}).get('settings', {}).get('corner_radius')!r}, "
                f"{n_listed} objects (not made again), same address {again.get('WPSystem', {}).get('address') == sysaddr}"]


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
