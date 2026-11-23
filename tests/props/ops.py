"""Declared properties over the protocol (object-properties-design.md 2.5):
get, set with setup strings, props with origins (run by tests/props/run.sh
after the snapshot, on the same wpsd)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import harness  # noqa: E402

T = os.environ["T"]
DESK = T + "/desk"
P = DESK + "/Projects"
fails = 0


def check(what, ok, more=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + (("  (" + str(more) + ")") if not ok and more != "" else ""))
    fails += not ok


class Svc:
    env = {"WPS_RUNTIME_DIR": os.environ["WPS_RUNTIME_DIR"]}


c = harness.Conn(Svc())


def get(target, name):
    r = c.request(op="get", target=target, name=name)
    return r.get("result") if r.get("ok") else None


def props(target):
    return {p["name"]: p for p in c.request(op="props", target=target).get("result") or []}


check("get: a folder's title", get(P, "title") == "Projects", get(P, "title"))
check("get: a default (Background shown: scaled to fill)", get(P, "bg_mode") == "scaled to fill", get(P, "bg_mode"))
check("get: no such property", c.request(op="get", target=P, name="nonsense").get("error") == "not found")

r = c.request(op="set", target=P, setup="icon_font=12.Sans Bold;bg_color=#336699;bg_mode=tiled;sort_key=2")
check("set: a setup string of four", r.get("ok"), r)
check("... the font", get(P, "icon_font") == "12.Sans Bold", get(P, "icon_font"))
check("... the colour", get(P, "bg_color") == "#336699")
check("... a choice by its option", get(P, "bg_mode") == "tiled")
check("... a choice by its number", get(P, "sort_key") == "type", get(P, "sort_key"))
pr = props(P)
check("props: own values say so", pr.get("bg_color", {}).get("origin") == "object", pr.get("bg_color"))
check("props: others are defaults", pr.get("bg_image", {}).get("origin") == "default", pr.get("bg_image"))
check("props: the class chain, the root's first", list(pr)[:3] == ["title", "template", "icon"], list(pr)[:5])
s = c.request(op="settings", target=P).get("result") or []
fields = {f["key"]: f["value"] for pg in s for f in pg["fields"]}
check("the notebook shows what set set", fields.get("bg_color") == "#336699" and fields.get("bg_mode") == 1, fields.get("bg_mode"))

r = c.request(op="set", target=P, setup="bg_color=blue")
check("set: a bad colour is refused, named", not r.get("ok") and "bg_color" in (r.get("message") or ""), r)
r = c.request(op="set", target=P, setup="nonsense=1")
check("set: no such property, named", not r.get("ok") and "nonsense" in (r.get("message") or ""), r)
r = c.request(op="set", target=P, setup="title")
check("set: not key=value", not r.get("ok"), r)
r = c.request(op="set", target=P, setup=r"icon_font=10.Sans\;x")
check("set: \\; inside a value", r.get("ok") and get(P, "icon_font") == "10.Sans;x", get(P, "icon_font"))

setup = (c.request(op="setup").get("result") or {}).get("objects") or {}
sch = setup.get("WPSchemePalette", {}).get("address")
r = c.request(op="set", target=sch, setup="scheme=dark")
check("a settings object: scheme=dark", r.get("ok") and get(sch, "scheme") == "dark", r)
setup = (c.request(op="setup").get("result") or {}).get("objects") or {}
check("... the setup reply has it, and only its own settings",
      setup.get("WPSchemePalette", {}).get("settings") == {"scheme": "dark"}, setup.get("WPSchemePalette"))
sysobj = setup.get("WPSystem", {}).get("address")
r = c.request(op="set", target=sysobj, setup="show_ext=no;confirm_delete=off")
check("checkboxes from no/off", r.get("ok") and get(sysobj, "show_ext") == "0" and get(sysobj, "confirm_delete") == "0", r)

print("%d failed" % fails)
sys.exit(1 if fails else 0)
