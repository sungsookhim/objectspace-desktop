"""First start, step 7: Templates in System (run by run.sh)."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import harness  # noqa: E402

T = os.environ["T"]
DESK, SYS, TPL = T + "/desk", T + "/xdg/data/wps/System", T + "/templates"
PART = sys.argv[1]
fails = 0


def check(what, ok, more=""):
    global fails
    print(("ok   " if ok else "FAIL ") + PART + ": " + what + (("  (" + str(more) + ")") if not ok and more != "" else ""))
    fails += not ok


class Svc:
    env = {"WPS_RUNTIME_DIR": os.environ["WPS_RUNTIME_DIR"]}


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
    check("marked X-WPS-Startup, in [Desktop Entry]", text.index("X-WPS-Startup=true") < text.index("[Desktop Action"), text)
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

if PART == "old":
    os.makedirs(SYS, exist_ok=True)
    r = c.request(op="shadow", target=TPL, folder=DESK)
    check("a step-6 desktop has Templates", wait(lambda: len(tpl_shadows(DESK)) == 1), r)
r = c.request(op="first_start", desktop=DESK)
check("first start answers its step", r.get("result") == (6 if PART == "old" else 0), r)
check("Templates is in System", wait(lambda: len(tpl_shadows(SYS)) == 1), tpl_shadows(SYS))
check("and not on the desktop", tpl_shadows(DESK) == [], tpl_shadows(DESK))
with open(T + "/config/desktop-made") as f:
    check("the desktop is at step 7", "version 7" in f.read())
r = c.request(op="first_start", desktop=DESK)
time.sleep(0.5)
check("a second start changes nothing", r.get("result") == 7 and len(tpl_shadows(SYS)) == 1 and not tpl_shadows(DESK), r)
print("%d failed" % fails)
sys.exit(1 if fails else 0)
