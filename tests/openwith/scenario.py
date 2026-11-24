"""Open With (run by run.sh): a data file's Open cascade lists its type's
programs (the default first, checked; added and declared ones; its parent
types'), Other program... last; opening with one of them; Always use for
this type writes mimeapps.list; every application for the picker."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

T = os.environ["T"]
A = T + "/share/applications"

if len(sys.argv) > 1 and sys.argv[1] == "setup":
    with open(T + "/fakeapp", "w") as f:
        f.write('#!/bin/sh\necho "$@ @$(pwd)" >> "%s/ran.log"\n' % T)
    os.chmod(T + "/fakeapp", 0o755)
    with open(T + "/faketerm", "w") as f:   # a terminal: what it was asked to run
        f.write('#!/bin/sh\necho "$@" >> "%s/term.log"\n' % T)
    os.chmod(T + "/faketerm", 0o755)
    os.mkdir(T + "/work")

    def entry(name, mimes, extra=""):
        with open(A + "/" + name + ".desktop", "w") as f:
            f.write("[Desktop Entry]\nType=Application\nName=%s\nExec=%s/fakeapp %s %%f\nMimeType=%s\n%s"
                    % (name.capitalize(), T, name, mimes, extra))
    entry("editor", "text/plain;", "Name[ko]=편집기\n")
    entry("viewer", "text/plain;")
    entry("ccode", "text/x-csrc;")
    entry("quiet", "text/plain;", "NoDisplay=true\n")
    entry("gone", "text/plain;", "Hidden=true\n")
    entry("other", "image/png;")
    with open(A + "/mimeinfo.cache", "w") as f:
        f.write("[MIME Cache]\ntext/plain=editor.desktop;viewer.desktop;quiet.desktop;gone.desktop;\n"
                "text/x-csrc=ccode.desktop;\nimage/png=other.desktop;\n")
    with open(T + "/share/mime/subclasses", "w") as f:
        f.write("text/x-csrc text/plain\n")
    with open(T + "/xdg/config/mimeapps.list", "w") as f:
        f.write("[Default Applications]\ntext/plain=viewer.desktop\n")
    for n in ("doc.txt", "prog.c", "pic.png"):
        with open(T + "/f/" + n, "w") as f:
            f.write("hello\n")
    sys.exit(0)

import harness  # noqa: E402

fails = 0


def check(what, ok, more=""):
    global fails
    print("%s %s%s" % ("ok  " if ok else "FAIL", what, "" if ok else "  (%s)" % (more,)))
    fails += not ok


class Svc:
    env = {"OSD_RUNTIME_DIR": os.environ["OSD_RUNTIME_DIR"]}


c = harness.Conn(Svc())


def cascade(name):
    m = c.request(op="menu", target=T + "/f/" + name).get("result") or []
    op = [x for x in m if x.get("id") == 1]
    return [(x["label"], x.get("checked")) for x in (op[0].get("items") if op else [])]


def ran():
    try:
        with open(T + "/ran.log") as f:
            return f.read().split("\n")
    except OSError:
        return []


def wait_ran(what):
    t = time.time()
    while time.time() - t < 3:
        if any(what in x for x in ran()):
            return True
        time.sleep(0.1)
    return False


m = cascade("doc.txt")
check("Open: the default first, checked; the others; Other program... last",
      m == [("Viewer", True), ("편집기", False), ("Quiet", False), ("Other program...", False)], m)
m = cascade("prog.c")
check("a C file: its own program (the default: the first declared), then text/plain's",
      m and m[0] == ("Ccode", True) and ("Viewer", False) in m and ("편집기", False) in m and m[-1][0] == "Other program...", m)
r = c.request(op="open", target=T + "/f/doc.txt", app_index=1)
check("open with the second: the editor", r.get("ok") and wait_ran("editor " + T + "/f/doc.txt"), (r, ran()))
r = c.request(op="open", target=T + "/f/doc.txt", app="ccode.desktop", always=True)
check("Other program, Always use for this type", r.get("ok") and wait_ran("ccode " + T + "/f/doc.txt"), (r, ran()))
with open(T + "/xdg/config/mimeapps.list") as f:
    ml = f.read()
check("mimeapps.list: the default, and first among the added", "text/plain=ccode.desktop\n" in ml
      and "[Added Associations]\ntext/plain=ccode.desktop;" in ml and ml.count("[Default Applications]") == 1, ml)
m = cascade("doc.txt")
check("Open: the new default first", m and m[0] == ("Ccode", True), m)
r = c.request(op="open", target=T + "/f/doc.txt")
check("a double-click opens with it too", r.get("ok") and len([x for x in ran() if x.startswith("ccode")]) >= 1 and wait_ran("ccode"), r)
r = c.request(op="open", target=T + "/f/doc.txt", app="../evil.desktop")
check("an unknown program: refused", not r.get("ok"), r)
apps = [a["name"] for a in c.request(op="apps").get("result") or []]
check("every application for the picker (not hidden ones), by name",
      "편집기" in apps and "Viewer" in apps and "Other" in apps and "Quiet" not in apps and "Gone" not in apps, apps)


# ---- program settings (v0.3-design 8)
ED = A + "/editor.desktop"


def pages(target):
    st = c.request(op="settings", target=target).get("result") or []
    return {p["title"]: {f["key"]: f.get("value") for f in p.get("fields", [])} for p in st}


pg = pages(ED)
check("the Program page: type, close on exit, working directory", pg.get("Program", {}).get("program_type") == 0
      and pg["Program"].get("close_on_exit") is True and pg["Program"].get("cwd") == "", pg.get("Program"))
check("the Association page: the types it opens", pg.get("Association", {}).get("mime_types") == "text/plain;", pg.get("Association"))
r = c.request(op="settings_set", target=ED, values={"mime_types": "text/plain;image/png"})
mine = os.environ["XDG_DATA_HOME"] + "/applications/editor.desktop"
check("types changed: the user's copy of the entry", r.get("ok") and os.path.exists(mine)
      and "MimeType=text/plain;image/png;\n" in open(mine).read(), r)
check("... and Open on a picture offers it", ("편집기", False) in cascade("pic.png"), cascade("pic.png"))
check("... the system's entry untouched", "MimeType=text/plain;\n" in open(ED).read())

setup = c.request(op="setup").get("result") or {}
sysaddr = ((setup.get("objects") or {}).get("OsdSystem") or {}).get("address")
r = c.request(op="settings_set", target=sysaddr, values={"terminal_command": T + "/faketerm --run"})
check("the System object: a terminal command", r.get("ok"), (sysaddr, r))
r = c.request(op="settings_set", target=ED, values={"program_type": 2, "close_on_exit": False, "cwd": T + "/work"})
check("set: text mode, kept open, a working directory", r.get("ok"), r)
r = c.request(op="open", target=ED)
t0 = time.time()
while time.time() - t0 < 3 and not os.path.exists(T + "/term.log"):
    time.sleep(0.1)
tl = open(T + "/term.log").read() if os.path.exists(T + "/term.log") else ""
check("opened in that terminal, held open after it ends", r.get("ok") and tl.startswith("--run sh -c ")
      and "Press Enter" in tl and (T + "/fakeapp editor") in tl, (r, tl))
r = c.request(op="settings_set", target=ED, values={"program_type": 1})
os.remove(T + "/term.log")
r = c.request(op="open", target=ED)
check("a window program: no terminal, in its working directory", r.get("ok") and wait_ran("@" + T + "/work")
      and not os.path.exists(T + "/term.log"), (r, ran()))

print("%d failed" % fails)
sys.exit(1 if fails else 0)
