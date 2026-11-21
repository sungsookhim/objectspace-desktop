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
