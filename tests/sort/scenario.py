"""A folder's sort order (run by run.sh): wpsd's listing in the folder's
order - folders first, then name, type, size or last write, ascending or
descending; additions and renames read in the order again; none: the
folder's own order."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import harness  # noqa: E402

T = os.environ["T"]
F = T + "/f"
fails = 0


def check(what, ok, more=""):
    global fails
    print("%s %s%s" % ("ok  " if ok else "FAIL", what, "" if ok else "  (%s)" % (more,)))
    fails += not ok


class Svc:
    env = {"OSD_RUNTIME_DIR": os.environ["OSD_RUNTIME_DIR"]}


c = harness.Conn(Svc())

# b.txt 300 bytes oldest, a.png 100 bytes, c.md 200 bytes newest; folders z/ and m/
for name, size, age in (("b.txt", 300, 300), ("a.png", 100, 200), ("c.md", 200, 100)):
    with open(F + "/" + name, "wb") as f:
        f.write(b"x" * size)
    t = time.time() - age
    os.utime(F + "/" + name, (t, t))
os.mkdir(F + "/z")
os.mkdir(F + "/m")


def names():
    return [it["name"] for it in (c.request(op="list", target=F, offset=0, count=50).get("result") or {}).get("items", [])]


def sort(key, desc=False):
    r1 = c.request(op="settings_set", target=F, values={"sort_key": key})
    r2 = c.request(op="settings_set", target=F, values={"sort_desc": desc})
    return r1.get("ok") and r2.get("ok")


check("by name", sort(1) and names() == ["m", "z", "a.png", "b.txt", "c.md"], names())
check("by name, descending (folders still first)", sort(1, True) and names() == ["z", "m", "c.md", "b.txt", "a.png"], names())
check("by type (extension)", sort(2) and names() == ["m", "z", "c.md", "a.png", "b.txt"], names())
check("by size", sort(3) and names() == ["m", "z", "a.png", "c.md", "b.txt"], names())
check("by last write", sort(4) and names() == ["m", "z", "b.txt", "a.png", "c.md"], names())
sort(1)
names()
os.rename(F + "/c.md", F + "/0.md")
time.sleep(0.5)
check("a rename: in the order", names() == ["m", "z", "0.md", "a.png", "b.txt"], names())
open(F + "/aa.txt", "w").close()
time.sleep(0.5)
check("an addition: in the order", names() == ["m", "z", "0.md", "a.png", "aa.txt", "b.txt"], names())
os.remove(F + "/a.png")
time.sleep(0.5)
check("a removal", names() == ["m", "z", "0.md", "aa.txt", "b.txt"], names())
check("none: the folder's own order (every entry)", sort(0) and sorted(names()) == sorted(["m", "z", "0.md", "aa.txt", "b.txt"]), names())
pages = c.request(op="settings", target=F).get("result") or []
fields = {x["key"]: x.get("value") for p in pages for x in p.get("fields", [])}
check("its pages: Sort by and Descending, the columns", fields.get("sort_key") == 0 and "sort_desc" in fields
      and all(fields.get(k) is True for k in ("col_name", "col_type", "col_size", "col_date")), fields)
r = c.request(op="settings_set", target=F, values={"col_type": False})
pages = c.request(op="settings", target=F).get("result") or []
fields = {x["key"]: x.get("value") for p in pages for x in p.get("fields", [])}
check("a column off", r.get("ok") and fields.get("col_type") is False and fields.get("col_size") is True, fields)
# a window's own listing: a filter (any case), without dot names, no lock files
open(F + "/.hidden", "w").close()
open(F + "/.~lock.b.txt#", "w").close()
open(F + "/Report.TXT", "w").close()


def names_v(flt="", hidden=True):
    msg = {"op": "list", "target": F, "offset": 0, "count": 50, "hidden": hidden}
    if flt:
        msg["filter"] = flt
    return sorted(it["name"] for it in (c.request(**msg).get("result") or {}).get("items", []))


time.sleep(0.3)
check("lock files are never listed; dot files are", ".~lock.b.txt#" not in names_v() and ".hidden" in names_v(), names_v())
check("hidden off: no dot files", ".hidden" not in names_v(hidden=False) and "b.txt" in names_v(hidden=False), names_v(hidden=False))
check("a filter, any case", names_v("txt") == ["Report.TXT", "aa.txt", "b.txt"], names_v("txt"))
check("a filter matching a folder", names_v("z") == ["z"], names_v("z"))
open(F + "/more.txt", "w").close()
time.sleep(0.5)
check("a filtered listing follows an addition", "more.txt" in names_v("txt"), names_v("txt"))
os.remove(F + "/aa.txt")
time.sleep(0.5)
check("and a removal", "aa.txt" not in names_v("txt"), names_v("txt"))
check("the plain listing too", "aa.txt" not in names_v() and "more.txt" in names_v(), names_v())


# ---- Lock position (0.3 step 6): kept with the position; Arrange keeps it
def layout():
    return {it["name"]: it for it in (c.request(op="layout", target=F).get("result") or {}).get("items", [])}


def general(target):
    st = c.request(op="settings", target=target).get("result") or []
    return {f["key"]: f.get("value") for p in st if p["title"] == "General" for f in p.get("fields", [])}


r = c.request(op="place", folder=F, items=[{"target": F + "/b.txt", "x": 40, "y": 50, "lock": True},
                                            {"target": F + "/more.txt", "x": 300, "y": 10}])
check("place, one locked", r.get("ok"), r)
L = layout()
check("layout: the lock with its position", L.get("b.txt", {}).get("locked") is True and L["b.txt"].get("x") == 40
      and "locked" not in L.get("more.txt", {}), L)
check("General page: Lock position on", general(F + "/b.txt").get("lock_pos") is True, general(F + "/b.txt"))
c.request(op="place", folder=F, items=[{"target": F + "/b.txt", "x": 60, "y": 70}])
check("placed again: still locked", layout().get("b.txt", {}).get("locked") is True, layout().get("b.txt"))
c.request(op="arrange", target=F)
L = layout()
check("Arrange forgets the others, keeps the locked one", "more.txt" not in L and L.get("b.txt", {}).get("x") == 60, L)
r = c.request(op="settings_set", target=F + "/b.txt", values={"lock_pos": False})
check("unlocked on its General page", r.get("ok") and "locked" not in layout().get("b.txt", {}), (r, layout().get("b.txt")))
r = c.request(op="settings_set", target=F + "/more.txt", values={"lock_pos": True})
L = layout()
check("locked with no position yet: locked, no x", r.get("ok") and L.get("more.txt", {}).get("locked") is True
      and "x" not in L.get("more.txt", {}), (r, L.get("more.txt")))
pages = [p["title"] for p in c.request(op="settings", target=F).get("result") or []]
check("a folder's Icon view page", "Icon view" in pages, pages)

print("%d failed" % fails)
sys.exit(1 if fails else 0)
