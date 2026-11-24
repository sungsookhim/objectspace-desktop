"""The SDK's sample plug-in (sdk/sample/OsdImageFile) in a private osdd (run
by tests/plugins/run.sh): a class for a file type, its declared property,
its own menu item handled in its host."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import harness  # noqa: E402

T = os.environ["T"]
DESK = T + "/desk"
fails = 0


def check(what, ok, more=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + (("  (" + str(more) + ")") if not ok and more != "" else ""))
    fails += not ok


class Svc:
    env = {"OSD_RUNTIME_DIR": os.environ["OSD_RUNTIME_DIR"]}


c = harness.Conn(Svc())
for _ in range(50):   # the host starts
    st = (c.request(op="stats").get("result") or {}).get("plugins") or []
    if any(p.get("ready") for p in st if isinstance(p, dict)):
        break
    time.sleep(0.1)
check("the sample's host is up", any(p.get("ready") for p in st if isinstance(p, dict)), st)

png = DESK + "/photo.png"
open(png, "wb").write(b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR")
r = c.request(op="resolve", target=png).get("result") or {}
check("a PNG is an Image (OsdImageFile: its types)", r.get("class") == "OsdImageFile" and r.get("class_title") == "Image", r)
txt = DESK + "/notes.txt"
open(txt, "w").write("hi")
check("a text file is not", (c.request(op="resolve", target=txt).get("result") or {}).get("class") == "OsdDataFile")

pages = c.request(op="settings", target=png).get("result") or []
img = [p for p in pages if p["title"] == "Image"]
check("its Image page, from its declared property", img and img[0]["fields"][0]["key"] == "rating", [p["title"] for p in pages])
r = c.request(op="set", target=png, setup="rating=3")
check("rating=3 (a setup string)", r.get("ok") and c.request(op="get", target=png, name="rating").get("result") == "3", r)

menu = c.request(op="menu", target=png).get("result") or []
items = [m.get("label") or m.get("title") for m in menu] if isinstance(menu, list) else []
check("its menu item, from its host", "Rate it 5 stars" in items, items)
r = c.request(op="menu_select", target=png, item=7001)
time.sleep(0.3)
check("chosen: its host handles it (Rating 5)", r.get("ok") and c.request(op="get", target=png, name="rating").get("result") == "5", r)

r = c.request(op="set", target="class:OsdImageFile", setup="rating=4")
png2 = DESK + "/other.png"
open(png2, "wb").write(b"\x89PNG\r\n\x1a\n")
check("all Images: rating 4 (class:OsdImageFile)", r.get("ok") and c.request(op="get", target=png2, name="rating").get("result") == "4", r)

print("%d failed" % fails)
sys.exit(1 if fails else 0)
