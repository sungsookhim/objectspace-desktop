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
    env = {"WPS_RUNTIME_DIR": os.environ["WPS_RUNTIME_DIR"]}


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
print("%d failed" % fails)
sys.exit(1 if fails else 0)
