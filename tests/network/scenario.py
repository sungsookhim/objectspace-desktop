# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""Network shares (run by run.sh): the Network folder in Storages; a share
(a gvfs folder) as a share object, titled, its icon lit, its address on its
notebook; opening it opens its folder; a file made on it behind a window's
back told to the window; a title set on a file there (no attributes: the
database, by the share's address) kept when osdd starts again with the
share at another place; the share gone: its object gone from Network, back
(with its title) when the share is; Disconnect: gio's unmount."""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import harness  # noqa: E402

T, REPO = os.environ["T"], os.environ["REPO"]
SHARE = "smb-share:server=FileServer,share=projects"
fails = 0


def check(what, ok, more=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + (("  (" + str(more) + ")") if not ok and more != "" else ""))
    fails += not ok


class Svc:
    env = {"OSD_RUNTIME_DIR": os.environ["OSD_RUNTIME_DIR"]}


def start(gvfs):
    env = dict(os.environ, OSD_GVFS_DIR=gvfs)
    p = subprocess.Popen([REPO + "/build/osdd"], env=env, stderr=open(T + "/osdd.log", "a"))
    for _ in range(50):
        if subprocess.run([REPO + "/build/osdctl", "ping"], env=env, capture_output=True).returncode == 0:
            break
        time.sleep(0.1)
    return p, harness.Conn(Svc())


def stop(p):
    subprocess.run([REPO + "/build/osdctl", "shutdown"], capture_output=True)
    p.wait(timeout=10)


def listing(c, folder):
    r = c.request(op="list", target=folder, offset=0, count=100, details=True).get("result") or {}
    return {x.get("title") or x["name"]: x for x in r.get("items", [])}


def wait_list(c, folder, cond, secs=6):
    t = time.time()
    while time.time() - t < secs:
        it = listing(c, folder)
        if cond(it):
            return it
        time.sleep(0.2)
    return listing(c, folder)


os.makedirs(T + "/gvfs/" + SHARE + "/Reports")
with open(T + "/gvfs/" + SHARE + "/plan.txt", "w") as f:
    f.write("plan\n")
p, c = start(T + "/gvfs")
c.request(op="first_start", desktop=T + "/desk")
NET = T + "/xdg/data/objectspace/System/Storages/Network"
r = c.request(op="resolve", target=NET).get("result") or {}
check("Network in Storages, beside Drives (an OsdNetwork)", r.get("class") == "OsdNetwork", r)
st = wait_list(c, T + "/xdg/data/objectspace/System/Storages", lambda it: "Network" in it and "Drives" in it)
check("... its own icon (the globe's)", (st.get("Network", {}).get("icon") or "") == "theme:network", st.get("Network"))
it = wait_list(c, NET, lambda it: "projects on fileserver" in it)
sh = it.get("projects on fileserver", {})
check("a share (GNOME's smb) as a share object, titled by share and server", bool(sh), sorted(it))
check("... its icon a connected share's", sh.get("icon") == "theme:drive-net", sh.get("icon"))
st = c.request(op="settings", target=sh.get("name", "").lstrip("\x01")).get("result") or []
addr = [f.get("value") for pg in st for f in pg.get("fields", []) if f["key"] == "address"]
check("... its notebook: its address", addr == ["smb://fileserver/projects"], addr)
r = c.request(op="open", target=sh.get("name", "").lstrip("\x01"))
check("opening it opens its folder", r.get("ok") and r.get("result", {}).get("folder") == T + "/gvfs/" + SHARE, r)

# a window on the share: a file made behind its back is told within the polling
FOLDER = T + "/gvfs/" + SHARE
c.request(op="list", target=FOLDER, offset=0, count=100)
c.request(op="subscribe", objects=[], folders=[FOLDER], jobs=False)
time.sleep(1.5)   # (its first look: what is there)
with open(FOLDER + "/new.txt", "w") as f:
    f.write("made elsewhere\n")
ev = c.events(4, until=lambda m: m.get("event") == "content_added" and m.get("name") == "new.txt")
check("a file made on the share behind a window's back: told to it", any(m.get("name") == "new.txt" for m in ev),
      [m.get("event") for m in ev])
os.remove(FOLDER + "/new.txt")
ev = c.events(4, until=lambda m: m.get("event") == "content_removed" and m.get("name") == "new.txt")
check("... and when it goes", any(m.get("event") == "content_removed" for m in ev), [m.get("event") for m in ev])

# a title on a file there: no attributes, so the database, by the share's address
r = c.request(op="settings_set", target=FOLDER + "/plan.txt", values={"title": "The plan"})
time.sleep(0.6)
check("a title set on a file on the share", r.get("ok"), r)
try:
    os.getxattr(FOLDER + "/plan.txt", "user.osd.OsdObject")
    on_file = True
except OSError:
    on_file = False
check("... kept by Objectspace, not on the file (no attributes there)", not on_file)
stop(p)

# the share mounted elsewhere (another folder for GNOME's): the same share
os.rename(T + "/gvfs/" + SHARE, T + "/gvfs2/" + SHARE)
p, c = start(T + "/gvfs2")
it = wait_list(c, T + "/gvfs2/" + SHARE, lambda it: "The plan" in it)
check("mounted again at another place: the file's title still there", "The plan" in it, sorted(it))
it = wait_list(c, NET, lambda it: "projects on fileserver" in it)
check("... and one share object for it (not a second)", list(it).count("projects on fileserver") == 1 and len(it) == 1, sorted(it))

# the share gone: its object gone from Network; back with its title
c.request(op="settings_set", target=it["projects on fileserver"]["name"].lstrip("\x01"), values={"title": "Team projects"})
time.sleep(0.3)   # (its saving and the parking at once: the parking waits for it)
os.rename(T + "/gvfs2/" + SHARE, T + "/away")
it = wait_list(c, NET, lambda it: not it)
if it:
    import sqlite3
    print("DBG failing at", time.strftime("%T"))
    nm = list(it.values())[0]["name"].lstrip("\x01")
    for r in sqlite3.connect(T + "/state/objects.db").execute("select vol, replace(key, char(31), ' | '), value, rev from state where key like ?", (nm + "%",)):
        print("DBG row", r)
check("the share gone (unmounted): its object gone from Network", not it, sorted(it))
os.rename(T + "/away", T + "/gvfs2/" + SHARE)
it = wait_list(c, NET, lambda it: "Team projects" in it)
check("... mounted again: back, its own title kept", "Team projects" in it, sorted(it))

# Disconnect: GNOME's share through gio
sh = it.get("Team projects", {})
m = c.request(op="menu", target=sh.get("name", "").lstrip("\x01")).get("result") or []
check("its menu: Disconnect (not Delete)", any(x.get("label") == "Disconnect" for x in m) and not any(x.get("label") == "Delete..." for x in m),
      [x.get("label") for x in m])
r = c.request(op="share", target=sh.get("name", "").lstrip("\x01"), action="disconnect")
log = open(T + "/gio.log").read() if os.path.exists(T + "/gio.log") else ""
check("Disconnect: GNOME's unmount (gio mount -u its folder)", r.get("ok") and ("mount -u " + T + "/gvfs2/" + SHARE) in log, (r, log))
it = wait_list(c, NET, lambda it: not it)
check("... and it is gone from Network", not it, sorted(it))
stop(p)
osdd_log = open(T + "/osdd.log").read()
check("no sanitizer reports", "Sanitizer" not in osdd_log and "runtime error" not in osdd_log)
print("%d failed" % fails)
sys.exit(1 if fails else 0)
