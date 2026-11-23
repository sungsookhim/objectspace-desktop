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

# ---- the chain (object-properties-design.md 2.2)
r = c.request(op="set", target="class:WPFolder", setup="bg_color=#102030;icon_titles=two lines")
check("class:WPFolder set", r.get("ok"), r)
os.makedirs(DESK + "/Plain", exist_ok=True)
check("a new folder takes all folders' colour", get(DESK + "/Plain", "bg_color") == "#102030", get(DESK + "/Plain", "bg_color"))
pp = props(DESK + "/Plain")
check("... its origin: class:WPFolder", pp.get("bg_color", {}).get("origin") == "class:WPFolder", pp.get("bg_color"))
check("... and a choice (Titles: two lines)", get(DESK + "/Plain", "icon_titles") == "two lines")
check("its own value still wins (Projects)", get(P, "bg_color") == "#336699", get(P, "bg_color"))
r = c.request(op="reset", target=P, names="bg_color")
check("reset: Projects' own colour removed", r.get("ok") and get(P, "bg_color") == "#102030", get(P, "bg_color"))
check("... origin class:WPFolder", props(P).get("bg_color", {}).get("origin") == "class:WPFolder")
r = c.request(op="reset", target=P, names="title")
check("reset: a title is the object's own (refused)", not r.get("ok"), r)
s2 = c.request(op="settings", target=DESK + "/Plain").get("result") or []
f2 = {f["key"]: f["value"] for pg in s2 for f in pg["fields"]}
check("its notebook shows the class's value", f2.get("bg_color") == "#102030" and f2.get("icon_titles") == 1, f2.get("bg_color"))
# the class chain: a subclass's scope before its parent's
r = c.request(op="set", target="class:WPTrashCan", setup="bg_color=#aa0000")
cp = props("class:WPTrashCan")
check("class:WPTrashCan: its own", cp.get("bg_color", {}).get("value") == "#aa0000", cp.get("bg_color"))
check("class:WPTrashCan: the rest from class:WPFolder", cp.get("icon_titles", {}).get("origin") == "class:WPFolder", cp.get("icon_titles"))
# types (data files): object-own properties are not set for a type
r = c.request(op="set", target="type:image/*", setup="template=1")
check("type: object-own properties are refused", not r.get("ok"), r)
r = c.request(op="set", target="class:WPObject", setup="icon=/x.png")
check("class:WPObject: its icon is each object's own", not r.get("ok"), r)
r = c.request(op="set", target="class:Nonsense", setup="a=b")
check("no such class", not r.get("ok") and r.get("error") == "not found", r)
r = c.request(op="get", target="class:WPFolder", name="bg_color")
check("get class:WPFolder bg_color", r.get("result") == "#102030", r)
r = c.request(op="reset", target="class:WPFolder", names="bg_color;icon_titles")
check("reset the class: the default again", get(DESK + "/Plain", "bg_color") == "" and get(DESK + "/Plain", "icon_titles") == "whole", get(DESK + "/Plain", "bg_color"))

# the System object's Folders and Fonts fields are all folders' (class:WPFolder)
r = c.request(op="set", target=sysobj, setup="show_hidden=0;font_folders=11.Serif")
check("System: Show hidden off, Folders font", r.get("ok"), r)
check("... is class:WPFolder's", c.request(op="get", target="class:WPFolder", name="show_hidden").get("result") == "0"
      and c.request(op="get", target="class:WPFolder", name="icon_font").get("result") == "11.Serif")
pp = props(DESK + "/Plain")
check("... a folder shows it, from class:WPFolder", pp.get("show_hidden", {}).get("value") == "0"
      and pp.get("icon_font", {}).get("origin") == "class:WPFolder", pp.get("show_hidden"))
r = c.request(op="set", target=DESK + "/Plain", setup="show_hidden=1")
check("... one folder shows hidden files anyway", get(DESK + "/Plain", "show_hidden") == "1" and get(P, "show_hidden") == "0")
check("System reads class:WPFolder back", get(sysobj, "font_folders") == "11.Serif", get(sysobj, "font_folders"))
c.request(op="reset", target="class:WPFolder", names="show_hidden;icon_font;show_ext")

print("%d failed" % fails)
sys.exit(1 if fails else 0)
