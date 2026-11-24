"""Every object's settings pages and menu, as osdd gives them, written to
OUT/<n>-<title>.txt (run by tests/props/run.sh). For the refactor
(docs/object-properties-design.md 4): the pages made from declared
properties must read as the hand-written ones did."""
import json
import os
import time
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import harness  # noqa: E402

T = os.environ["T"]
OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
DESK = T + "/desk"


class Svc:
    env = {"OSD_RUNTIME_DIR": os.environ["OSD_RUNTIME_DIR"]}


c = harness.Conn(Svc())


def norm(s):
    s = s.replace(T, "<T>")
    s = re.sub(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}", "<ID>", s)
    s = re.sub(r"\b\d{4}-\d\d-\d\d[ T]\d\d:\d\d(:\d\d)?", "<DATE>", s)
    s = s.replace(os.uname().nodename, "<HOST>")
    s = s.replace(os.path.expanduser("~"), "<HOME>")
    return s


def items(folder):
    r = c.request(op="list", target=folder, offset=0, count=500, details=True).get("result") or {}
    out = []
    for it in r.get("items", []):
        name = it.get("name", "")
        target = name[1:] if name.startswith("\x01") else folder + "/" + name
        out.append((it.get("title") or name, target, it.get("target") or ""))
    return sorted(out)


def dump(n, title, target):
    s = c.request(op="settings", target=target)
    m = c.request(op="menu", target=target)
    text = "target: %s\n\n## settings\n%s\n\n## menu\n%s\n" % (
        target, json.dumps(s.get("result", s), indent=1, sort_keys=True, ensure_ascii=False),
        json.dumps(m.get("result", m), indent=1, sort_keys=True, ensure_ascii=False))
    safe = re.sub(r"[^A-Za-z0-9._-]+", "_", norm(title))[:60]
    with open("%s/%03d-%s.txt" % (OUT, n, safe), "w") as f:
        f.write(norm(text))


# a desktop as the first start makes it, and objects of the other kinds
os.makedirs(DESK + "/Projects", exist_ok=True)
open(DESK + "/notes.txt", "w").write("hi\n")
open(DESK + "/build.sh", "w").write("#!/bin/sh\n")
os.chmod(DESK + "/build.sh", 0o755)
open(DESK + "/editor.desktop", "w").write("[Desktop Entry]\nType=Application\nName=Editor\nExec=true %f\nMimeType=text/plain;\n")
c.request(op="first_start", desktop=DESK)
# (its last step, Prevent delete on WPS's own objects, comes a moment after the reply)
for _ in range(50):
    r = c.request(op="get", target=T + "/xdg/data/objectspace/Programs", name="no_delete")
    if str(r.get("result")).lower() in ("1", "true"):
        break
    time.sleep(0.1)
c.request(op="create", folder=DESK, **{"class": "OsdProgram"}, title="My program", command="true")
c.request(op="shadow", target=DESK + "/notes.txt", folder=DESK + "/Projects")
setup = c.request(op="setup").get("result") or {}

n = 0
seen = set()
# the desktop and Projects, then the folders their shadows stand for that
# WPS made (System, System Setup, My Storages, Templates, ...): two levels
queue = [(DESK, 0), (DESK + "/Projects", 0), (T + "/xdg/data/objectspace", 0), (T + "/templates", 1), (T + "/xdg/data/objectspace/System/System Setup/Schemes", 1)]
n += 1
dump(n, "the desktop", DESK)
seen.add(DESK)
done = set()
while queue:
    folder, depth = queue.pop(0)
    if folder in done:
        continue
    done.add(folder)
    for title, target, orig in items(folder):
        if target not in seen:
            seen.add(target)
            n += 1
            dump(n, title, target)
        for p in (orig, target):
            if depth < 2 and p.startswith(T + "/") and os.path.isdir(p) and p not in done and "/Programs" not in p:
                queue.append((p, depth + 1))
for cls, o in sorted((setup.get("objects") or {}).items()):
    if o.get("address") not in seen:
        seen.add(o["address"])
        n += 1
        dump(n, cls, o["address"])
print("%d objects (%s)" % (n, OUT))
