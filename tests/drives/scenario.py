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
    env = {"WPS_RUNTIME_DIR": os.environ["WPS_RUNTIME_DIR"]}


c = harness.Conn(Svc())
fake = dbus.Interface(dbus.bus.BusConnection(os.environ["FAKE_BUS"]).get_object("org.freedesktop.UDisks2", "/org/wps/Fake"),
                      "org.wps.FakeUDisks")


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

sys.exit(1 if fails else 0)
