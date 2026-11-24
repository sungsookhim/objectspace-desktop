"""Drives: the udisks2 client against tests/fake_udisks.py (run by run.sh)."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import dbus  # noqa: E402
import harness  # noqa: E402

T = os.environ["T"]
fails = 0


def check(what, ok, more=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + (("  (" + str(more) + ")") if not ok and more != "" else ""))
    fails += not ok


class Svc:
    env = {"OSD_RUNTIME_DIR": os.environ["OSD_RUNTIME_DIR"]}


c = harness.Conn(Svc())
fake = dbus.Interface(dbus.bus.BusConnection(os.environ["FAKE_BUS"]).get_object("org.freedesktop.UDisks2", "/org/objectspace/Fake"),
                      "io.github.sungsookhim.Objectspace.FakeUDisks")


def drives():
    return {d["uuid"]: d for d in c.request(op="drives")["result"]}


def wait_for(cond, secs=3):
    t = time.time()
    while time.time() - t < secs:
        d = drives()
        if cond(d):
            return d
        time.sleep(0.1)
    return drives()


d = wait_for(lambda d: "AAAA-0001" in d)
check("udisks2 reached", c.request(op="drives").get("udisks") is True)
check("listed: root, stick, LUKS; not EFI, not swap", sorted(d) == ["AAAA-0001", "ROOT-0000", "luks-1111"], sorted(d))
check("root: kind root, mounted at /", d.get("ROOT-0000", {}).get("kind") == "root" and d["ROOT-0000"]["mount"] == "/")
s = d.get("AAAA-0001", {})
check("stick: usb, removable, titled by its label", s.get("kind") == "usb" and s.get("removable") and s.get("title") == "STICK", s)
check("LUKS: encrypted, locked, titled by size", d.get("luks-1111", {}).get("encrypted") and not d["luks-1111"]["unlocked"]
      and d["luks-1111"]["title"] == "1.1 GB Encrypted Volume", d.get("luks-1111"))

r = c.request(op="drive", uuid="AAAA-0001", action="mount")
check("mount: its mount point", r.get("ok") and r.get("result") == T + "/mnt/STICK", r)
d = wait_for(lambda d: d["AAAA-0001"]["mount"])
check("mounted: the list follows", d["AAAA-0001"]["mount"] == T + "/mnt/STICK")

fake.SetBusy(True)
r = c.request(op="drive", uuid="AAAA-0001", action="unmount")
check("unmount while busy: refused as busy", not r.get("ok") and r.get("error") == "busy", r)
fake.SetBusy(False)

r = c.request(op="drive", uuid="AAAA-0001", action="eject")
check("eject (unmounts first)", r.get("ok"), r)
d = wait_for(lambda d: "AAAA-0001" not in d)
check("ejected: gone from the list", "AAAA-0001" not in d)
fake.Plug("stick")
d = wait_for(lambda d: "AAAA-0001" in d)
check("plugged again: back", "AAAA-0001" in d and not d["AAAA-0001"]["mount"])

r = c.request(op="drive", uuid="luks-1111", action="mount")
check("mount a locked volume: refused", not r.get("ok") and r.get("error") == "locked", r)
r = c.request(op="drive", uuid="luks-1111", action="unlock", **{"pass": "wrong"})
check("unlock, wrong passphrase: refused", not r.get("ok"), r)
r = c.request(op="drive", uuid="luks-1111", action="unlock", **{"pass": "secret"})
check("unlock", r.get("ok"), r)
d = wait_for(lambda d: d["luks-1111"]["unlocked"])
check("unlocked: shown as the container, its label", d["luks-1111"]["unlocked"] and d["luks-1111"]["title"] == "Secret"
      and "clear-2222" not in d, d.get("luks-1111"))
r = c.request(op="drive", uuid="luks-1111", action="mount")
check("mount the unlocked volume", r.get("ok") and r.get("result") == T + "/mnt/Secret", r)
r = c.request(op="drive", uuid="luks-1111", action="lock")
check("lock (unmounts first)", r.get("ok"), r)
d = wait_for(lambda d: not d["luks-1111"]["unlocked"])
check("locked again", not d["luks-1111"]["unlocked"] and not d["luks-1111"]["mount"])

fake.SetAuth(False)
r = c.request(op="drive", uuid="AAAA-0001", action="mount")
check("mount not authorized: said so", not r.get("ok") and r.get("error") == "not authorized", r)
fake.SetAuth(True)


# ---- the objects: Computer and Drives (first start), kept in step
SYS = T + "/xdg/data/objectspace/System"
c.request(op="first_start", desktop=T + "/desk")


def listing(folder):
    r = c.request(op="list", target=folder, offset=0, count=100, details=True).get("result") or {}
    return {x.get("title") or x["name"]: x for x in r.get("items", [])}


def wait_list(folder, cond, secs=4):
    t = time.time()
    while time.time() - t < secs:
        it = listing(folder)
        if cond(it):
            return it
        time.sleep(0.15)
    return listing(folder)


it = wait_list(SYS, lambda it: "My Storages" in it)
check("System holds My Storages", "My Storages" in it, sorted(it))
check("My Storages has its own icon", (it.get("My Storages", {}).get("icon") or "") == "theme:computer", it.get("My Storages"))
r = c.request(op="resolve", target=SYS + "/Storages")["result"]
check("Computer is a OsdComputer", r.get("class") == "OsdComputer", r.get("class"))
r = c.request(op="resolve", target=SYS + "/Storages/Drives")["result"]
check("Drives is a OsdDrives", r.get("class") == "OsdDrives", r.get("class"))
it = wait_list(SYS + "/Storages", lambda it: "Root" in it)
check("Computer: Drives and Root", "Drives" in it and "Root" in it, sorted(it))
check("Root's icon: the root drive, lit", (it.get("Root", {}).get("icon") or "") == "theme:drive-root", it.get("Root"))
it = wait_list(SYS + "/Storages/Drives", lambda it: "STICK" in it and "1.1 GB Encrypted Volume" in it)
check("Drives: the stick and the LUKS volume (not root)", sorted(it) == ["1.1 GB Encrypted Volume", "STICK"], sorted(it))
stick = it.get("STICK", {})
stick_addr = stick.get("name", "")[1:]
check("the stick's icon: usb, not lit", (stick.get("icon") or "") == "theme:drive-usb-off", stick.get("icon"))

m = c.request(op="menu", target=stick_addr).get("result") or []
labels = [x.get("label") for x in m]
check("stick's menu: Mount, Eject, Safely remove; no Delete", "Mount" in labels and "Eject" in labels
      and "Safely remove" in labels and "Delete..." not in labels, labels)
st = c.request(op="settings", target=stick_addr).get("result") or []
pg = {p["title"]: {f["key"]: f.get("value") for f in p.get("fields", [])} for p in st}
check("stick's Drive page: device, size, UUID", pg.get("Drive", {}).get("device") == "/dev/sdc1"
      and pg["Drive"].get("size") == "16 GB" and pg["Drive"].get("uuid") == "AAAA-0001", pg.get("Drive"))

r = c.request(op="open", target=stick_addr)
check("open the stick: mounted, its folder", (r.get("result") or {}).get("folder") == T + "/mnt/STICK", r)
it = wait_list(SYS + "/Storages/Drives", lambda it: (it.get("STICK", {}).get("icon") or "") == "theme:drive-usb")
check("mounted: its icon lit", (it.get("STICK", {}).get("icon") or "") == "theme:drive-usb", it.get("STICK", {}).get("icon"))
m = c.request(op="menu", target=stick_addr).get("result") or []
check("mounted: Unmount in its menu", "Unmount" in [x.get("label") for x in m])
r = c.request(op="drive", target=stick_addr, action="unmount")
check("unmount by its object", r.get("ok"), r)

fake.Pull("stick")
it = wait_list(SYS + "/Storages/Drives", lambda it: "STICK" not in it)
check("pulled out: gone from Drives", "STICK" not in it, sorted(it))
fake.Plug("stick")
it = wait_list(SYS + "/Storages/Drives", lambda it: "STICK" in it)
check("plugged in again: back, the same object", it.get("STICK", {}).get("name", "")[1:] == stick_addr, it.get("STICK"))

r = c.request(op="open", target=it["1.1 GB Encrypted Volume"]["name"][1:])
check("open a locked volume: says locked", r.get("error") == "locked", r)

sys.exit(1 if fails else 0)
