# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""First start, step 7: Templates in System (run by run.sh)."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import harness  # noqa: E402

T = os.environ["T"]
DESK, SYS, TPL = T + "/desk", T + "/xdg/data/objectspace/System", T + "/templates"
PART = sys.argv[1]
fails = 0


def check(what, ok, more=""):
    global fails
    print(("ok   " if ok else "FAIL ") + PART + ": " + what + (("  (" + str(more) + ")") if not ok and more != "" else ""))
    fails += not ok


class Svc:
    env = {"OSD_RUNTIME_DIR": os.environ["OSD_RUNTIME_DIR"]}


c = harness.Conn(Svc())


def tpl_shadows(folder):
    out = []
    for it in (c.request(op="list", target=folder, offset=0, count=200, details=True).get("result") or {}).get("items", []):
        if it.get("shadow") and it.get("target_path") == TPL:
            out.append(it)
    return out


def wait(fn, secs=3):
    end = time.time() + secs
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.1)
    return fn()


if PART == "startup":
    AUTO = T + "/xdg/config/autostart"
    os.makedirs(T + "/share/applications", exist_ok=True)
    with open(T + "/share/applications/clock.desktop", "w") as f:
        f.write("[Desktop Entry]\nType=Application\nName=Clock\nExec=true\n\n[Desktop Action new]\nName=New\nExec=true\n")
    with open(T + "/run me.sh", "w") as f:
        f.write("#!/bin/sh\ntrue\n")
    os.chmod(T + "/run me.sh", 0o755)
    os.makedirs(T + "/Work", exist_ok=True)
    r = c.request(op="startup_add", target=T + "/share/applications/clock.desktop")
    check("a program becomes an entry", r.get("result") == AUTO + "/clock.desktop", r)
    text = open(AUTO + "/clock.desktop").read()
    check("marked X-Objectspace-Startup, in [Desktop Entry]", text.index("X-Objectspace-Startup=true") < text.index("[Desktop Action"), text)
    r = c.request(op="startup_add", target=T + "/share/applications/clock.desktop")
    check("again: a second entry, nothing overwritten", r.get("result") == AUTO + "/clock-2.desktop", r)
    r = c.request(op="startup_add", target=T + "/run me.sh")
    check("a script becomes an entry", r.get("result") == AUTO + "/run me.desktop", r)
    text = open(AUTO + "/run me.desktop").read()
    check("running it", 'Exec="%s/run me.sh"' % T in text, text)
    r = c.request(op="startup_add", target=T + "/Work")
    check("a folder becomes a shadow", str(r.get("result", "")).startswith("vobj:"), r)
    items = (c.request(op="list", target=AUTO, offset=0, count=100, details=True).get("result") or {}).get("items", [])
    check("the shadow is in Startup", any(it.get("shadow") and it.get("target_path") == T + "/Work" for it in items), items)
    sh = c.request(op="shadow", target=T + "/share/applications/clock.desktop", folder=DESK).get("result")
    r = c.request(op="startup_add", target=sh)
    check("a program's shadow becomes the program's entry", r.get("result") == AUTO + "/clock-3.desktop", r)
    pages = c.request(op="settings", target=AUTO + "/clock.desktop").get("result") or []
    f = [x for p in pages for x in p.get("fields", []) if x.get("key") == "start_at_login"]
    check("an entry's notebook: Start at login, on", f and f[0]["value"] is True, f)
    r = c.request(op="settings_set", target=AUTO + "/clock.desktop", values={"start_at_login": False})
    text = open(AUTO + "/clock.desktop").read()
    check("off: Hidden=true", r.get("ok") and "Hidden=true" in text and "X-GNOME-Autostart-enabled=false" in text, (r, text))
    c.request(op="settings_set", target=AUTO + "/clock.desktop", values={"start_at_login": True})
    text = open(AUTO + "/clock.desktop").read()
    check("on again: the keys go", "Hidden" not in text and "Autostart-enabled" not in text, text)
    pages = c.request(op="settings", target=T + "/share/applications/clock.desktop").get("result") or []
    check("a program elsewhere has no Start at login", not [x for p in pages for x in p.get("fields", []) if x.get("key") == "start_at_login"])
    print("%d failed" % fails)
    sys.exit(1 if fails else 0)

def items(folder):
    return (c.request(op="list", target=folder, offset=0, count=200, details=True).get("result") or {}).get("items", [])


def shadows_of(folder, target):
    return [it for it in items(folder) if it.get("shadow") and it.get("target_path") == target]


def titled(folder, title):
    return [it for it in items(folder) if it.get("title") == title and not it.get("shadow")]


if PART == "restore":
    # (after "new": the desktop's defaults made; a second start notes them)
    PROG, TRASH = T + "/xdg/data/objectspace/Programs", T + "/xdg/data/Trash/files"
    c.request(op="first_start", desktop=DESK)
    check("Restore Desktop is in System", wait(lambda: len(titled(SYS, "Restore Desktop")) == 1), items(SYS))
    check("Command Prompt is in System", wait(lambda: len(titled(SYS, "Command Prompt")) == 1), items(SYS))
    time.sleep(0.5)

    def vobj(it):
        return it["name"][1:]

    # a shadow's menu is its original's (0.4 step 6): the folder's views
    # and Arrange for the desktop's Programs
    r = c.request(op="menu", target=vobj(shadows_of(DESK, PROG)[0]))
    m = {i["label"]: [x["label"] for x in i["items"]] for i in (r.get("result") or [])}
    check("a shadow's menu: its original's (Open's views, Arrange)", "Arrange" in m and "Icon view" in m.get("Open", []), r)

    # Prevent delete (step 11, 0.4): Objectspace's own objects refuse the Trash
    # Can and delete, as the desktop's shadows of them do; a file of the
    # user's with it on too, as a trash job and as a move into the Trash Can
    r = c.request(op="vtrash", target=vobj(shadows_of(DESK, SYS)[0]))
    check("prevent delete: System's shadow refuses the Trash Can", r.get("error") == "protected" and "My System" in r.get("message", ""), r)
    r = c.request(op="vdelete", target=vobj(titled(SYS, "Minimized Window Viewer")[0]))
    check("prevent delete: the viewer refuses delete", r.get("error") == "protected", r)
    with open(DESK + "/keep.txt", "w") as f:
        f.write("x")
    c.request(op="settings_set", target=DESK + "/keep.txt", values={"no_delete": True})
    sid = c.request(op="select", folder=DESK, items=["keep.txt"]).get("result")
    r = c.request(op="trash", selection=sid)
    check("prevent delete: a file's own, trash refused", r.get("error") == "protected" and "keep.txt" in r.get("message", ""), r)
    sid = c.request(op="select", folder=DESK, all=True, **{"except": []}).get("result")
    r = c.request(op="delete", selection=sid)
    check("prevent delete: all of the desktop, delete refused (nothing goes)", r.get("error") == "protected" and os.path.exists(DESK + "/keep.txt"), r)
    sid = c.request(op="select", folder=DESK, items=["keep.txt"]).get("result")
    r = c.request(op="move", selection=sid, dest=TRASH)
    check("prevent delete: moved into the Trash Can, refused", r.get("error") == "protected", r)
    c.request(op="settings_set", target=DESK + "/keep.txt", values={"no_delete": False})
    sid = c.request(op="select", folder=DESK, items=["keep.txt"]).get("result")
    r = c.request(op="trash", selection=sid)
    check("prevent delete off: trashed", r.get("ok"), r)
    # the user's mishaps (Prevent delete turned off first): Programs
    # dropped on itself, System thrown away, Startup and the Minimized
    # Window Viewer deleted, the Launcher object moved to the desktop
    for t in (vobj(shadows_of(DESK, SYS)[0]), vobj(shadows_of(SYS, T + "/xdg/config/autostart")[0]),
              vobj(titled(SYS, "Minimized Window Viewer")[0])):
        c.request(op="settings_set", target=t, values={"no_delete": False})
    r = c.request(op="vmove", target=vobj(shadows_of(DESK, PROG)[0]), folder=PROG)
    check("(Programs moved into Programs)", r.get("ok"), r)
    r = c.request(op="vtrash", target=vobj(shadows_of(DESK, SYS)[0]))
    check("(System into the Trash Can)", r.get("ok"), r)
    AUTO = T + "/xdg/config/autostart"
    r = c.request(op="vdelete", target=vobj(shadows_of(SYS, AUTO)[0]))
    check("(Startup deleted)", r.get("ok"), r)
    r = c.request(op="vdelete", target=vobj(titled(SYS, "Minimized Window Viewer")[0]))
    check("(the Minimized Window Viewer deleted)", r.get("ok"), r)
    r = c.request(op="vmove", target=vobj(titled(SYS, "Launcher")[0]), folder=DESK)
    check("(the Launcher object moved to the desktop)", r.get("ok"), r)
    r = c.request(op="restore_defaults")
    got = sorted((r.get("result") or {}).get("restored", []))
    check("restore: what it put back", got == sorted(["Programs", "My System", "Startup", "Minimized Window Viewer", "Launcher"]), r)
    check("Programs: back on the desktop, the same one", len(shadows_of(DESK, PROG)) == 1 and not shadows_of(PROG, PROG),
          (items(DESK), items(PROG)))
    check("System: back from the Trash Can", len(shadows_of(DESK, SYS)) == 1 and not shadows_of(TRASH, SYS), items(TRASH))
    check("Startup: made again", len(shadows_of(SYS, AUTO)) == 1, items(SYS))
    check("the viewer: made again", len(titled(SYS, "Minimized Window Viewer")) == 1, items(SYS))
    check("the Launcher object: back in System", len(titled(SYS, "Launcher")) == 1 and not titled(DESK, "Launcher"))
    r = c.request(op="vdelete", target=vobj(titled(SYS, "Minimized Window Viewer")[0]))
    check("restored: Prevent delete on again", r.get("error") == "protected", r)
    r = c.request(op="restore_defaults", desktop=DESK)
    check("again: nothing to do", r.get("ok") and (r.get("result") or {}).get("restored") == [], r)
    # renamed outside Objectspace (a shell), osdd running: moved back at once
    for own in (SYS, SYS + "/Storages", PROG):
        os.rename(own, own + "-renamed")
        check("%s renamed outside: moved back" % os.path.basename(own),
              wait(lambda: os.path.isdir(own) and not os.path.exists(own + "-renamed")), os.listdir(os.path.dirname(own)))
    check("the desktop's System still is System", wait(lambda: len(shadows_of(DESK, SYS)) == 1), items(DESK))
    r = c.request(op="restore_defaults", desktop=DESK)
    check("after them: nothing to restore", r.get("ok") and (r.get("result") or {}).get("restored") == [], r)
    print("%d failed" % fails)
    sys.exit(1 if fails else 0)

if PART == "old":
    os.makedirs(SYS, exist_ok=True)
    r = c.request(op="shadow", target=TPL, folder=DESK)
    check("a step-6 desktop has Templates", wait(lambda: len(tpl_shadows(DESK)) == 1), r)
r = c.request(op="first_start", desktop=DESK)
check("first start answers its step", r.get("result") == (6 if PART == "old" else 0), r)
check("Templates is in System", wait(lambda: len(tpl_shadows(SYS)) == 1), tpl_shadows(SYS))
check("and not on the desktop", tpl_shadows(DESK) == [], tpl_shadows(DESK))
with open(T + "/config/desktop-made") as f:
    check("the desktop is at step 14", "version 14" in f.read())
COMP = SYS + "/Storages"
if PART == "old":   # (run.sh made 0.2's Computer, with a file in it)
    check("0.2's Computer is moved to Storages, its contents with it", os.path.exists(COMP + "/kept.txt")
          and not os.path.exists(SYS + "/Computer"), os.listdir(SYS))
check("step 9: the home folder in Computer", wait(lambda: len(shadows_of(COMP, os.environ["HOME"])) == 1), items(COMP))


def title_of(target):
    pages = c.request(op="settings", target=target).get("result") or []
    return ([x.get("value") for p in pages for x in p.get("fields", []) if x.get("key") == "title"] or [None])[0]


check("step 10: System is titled My System - <host>", (title_of(SYS) or "").startswith("My System - "), title_of(SYS))
check("step 10: Computer is titled My Storages", title_of(COMP) == "My Storages", title_of(COMP))
TRAYS = T + "/xdg/data/objectspace/Trays"
check("step 13: Find is in System", wait(lambda: len(titled(SYS, "Find")) == 1), items(SYS))
check("step 13: the Trays folder is shown in System", wait(lambda: len(shadows_of(SYS, TRAYS)) == 1), items(SYS))
NTRAY = 3 if PART == "old" else 4   # (the step-6 desktop here has no Programs folder; a real one has)
check("step 13: the first tray, Default", wait(lambda: len(items(TRAYS + "/Default")) == NTRAY), items(TRAYS + "/Default"))
tray = items(TRAYS + "/Default")
titles = sorted(it.get("title") or "" for it in tray)
check("... My System, Programs, Command Prompt, Find, all shadows",
      all(it.get("shadow") for it in tray) and len(shadows_of(TRAYS + "/Default", SYS)) == 1
      and (PART == "old" or len(shadows_of(TRAYS + "/Default", T + "/xdg/data/objectspace/Programs")) == 1)
      and "Command Prompt" in titles and "Find" in titles, tray)
fobj = next((it for it in tray if it.get("title") == "Find"), None)
r = c.request(op="vdelete", target=fobj["name"][1:]) if fobj else {}   # (as TheBar's Remove from tray does)
check("a tray's shadow can be taken away (no Prevent delete)", r.get("ok") and wait(lambda: len(items(TRAYS + "/Default")) == NTRAY - 1), r)
check("... and Find itself stays in System", len(titled(SYS, "Find")) == 1, items(SYS))
r = c.request(op="rename", target=COMP, name="Storage")
check("renaming Objectspace's own folder: its title only", r.get("ok") and os.path.isdir(COMP) and title_of(COMP) == "Storage", r)
c.request(op="rename", target=COMP, name="My Storages")
for own in (DESK, TPL):   # XDG user folders: their titles only (user-dirs.dirs would not follow)
    r = c.request(op="rename", target=own, name="Renamed")
    check("renaming %s: its title only" % os.path.basename(own), r.get("ok") and os.path.isdir(own) and title_of(own) == "Renamed", r)
r = c.request(op="first_start", desktop=DESK)
time.sleep(0.5)
check("a second start changes nothing", r.get("result") == 14 and len(tpl_shadows(SYS)) == 1 and not tpl_shadows(DESK)
      and len(shadows_of(COMP, os.environ["HOME"])) == 1, r)
print("%d failed" % fails)
sys.exit(1 if fails else 0)
