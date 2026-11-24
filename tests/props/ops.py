# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""Declared properties over the protocol (object-properties-design.md 2.5):
get, set with setup strings, props with origins (run by tests/props/run.sh
after the snapshot, on the same osdd)."""
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
    env = {"OSD_RUNTIME_DIR": os.environ["OSD_RUNTIME_DIR"]}


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
check("props: the class chain, the root's first", list(pr)[:3] == ["title", "template", "no_delete"], list(pr)[:5])
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
sch = setup.get("OsdSchemePalette", {}).get("address")
r = c.request(op="set", target=sch, setup="scheme=dark")
check("a settings object: scheme=dark", r.get("ok") and get(sch, "scheme") == "dark", r)
setup = (c.request(op="setup").get("result") or {}).get("objects") or {}
check("... the setup reply has it, and only its own settings",
      setup.get("OsdSchemePalette", {}).get("settings") == {"scheme": "dark"}, setup.get("OsdSchemePalette"))
sysobj = setup.get("OsdSystem", {}).get("address")
r = c.request(op="set", target=sysobj, setup="show_ext=no;confirm_delete=off")
check("checkboxes from no/off", r.get("ok") and get(sysobj, "show_ext") == "0" and get(sysobj, "confirm_delete") == "0", r)

# ---- the chain (object-properties-design.md 2.2)
r = c.request(op="set", target="class:OsdFolder", setup="bg_color=#102030;icon_titles=two lines")
check("class:OsdFolder set", r.get("ok"), r)
os.makedirs(DESK + "/Plain", exist_ok=True)
check("a new folder takes all folders' colour", get(DESK + "/Plain", "bg_color") == "#102030", get(DESK + "/Plain", "bg_color"))
pp = props(DESK + "/Plain")
check("... its origin: class:OsdFolder", pp.get("bg_color", {}).get("origin") == "class:OsdFolder", pp.get("bg_color"))
check("... and a choice (Titles: two lines)", get(DESK + "/Plain", "icon_titles") == "two lines")
check("its own value still wins (Projects)", get(P, "bg_color") == "#336699", get(P, "bg_color"))
r = c.request(op="reset", target=P, names="bg_color")
check("reset: Projects' own colour removed", r.get("ok") and get(P, "bg_color") == "#102030", get(P, "bg_color"))
check("... origin class:OsdFolder", props(P).get("bg_color", {}).get("origin") == "class:OsdFolder")
r = c.request(op="reset", target=P, names="title")
check("reset: a title is the object's own (refused)", not r.get("ok"), r)
s2 = c.request(op="settings", target=DESK + "/Plain").get("result") or []
f2 = {f["key"]: f["value"] for pg in s2 for f in pg["fields"]}
check("its notebook shows the class's value", f2.get("bg_color") == "#102030" and f2.get("icon_titles") == 1, f2.get("bg_color"))
# the class chain: a subclass's scope before its parent's
r = c.request(op="set", target="class:OsdTrashCan", setup="bg_color=#aa0000")
cp = props("class:OsdTrashCan")
check("class:OsdTrashCan: its own", cp.get("bg_color", {}).get("value") == "#aa0000", cp.get("bg_color"))
check("class:OsdTrashCan: the rest from class:OsdFolder", cp.get("icon_titles", {}).get("origin") == "class:OsdFolder", cp.get("icon_titles"))
# types (data files): object-own properties are not set for a type
r = c.request(op="set", target="type:image/*", setup="template=1")
check("type: object-own properties are refused", not r.get("ok"), r)
r = c.request(op="set", target="class:OsdObject", setup="icon=/x.png")
check("class:OsdObject: its icon is each object's own", not r.get("ok"), r)
r = c.request(op="set", target="class:Nonsense", setup="a=b")
check("no such class", not r.get("ok") and r.get("error") == "not found", r)
r = c.request(op="get", target="class:OsdFolder", name="bg_color")
check("get class:OsdFolder bg_color", r.get("result") == "#102030", r)
r = c.request(op="reset", target="class:OsdFolder", names="bg_color;icon_titles")
check("reset the class: the default again", get(DESK + "/Plain", "bg_color") == "" and get(DESK + "/Plain", "icon_titles") == "whole", get(DESK + "/Plain", "bg_color"))

# the System object's Folders and Fonts fields are all folders' (class:OsdFolder)
r = c.request(op="set", target=sysobj, setup="show_hidden=0;font_folders=11.Serif")
check("System: Show hidden off, Folders font", r.get("ok"), r)
check("... is class:OsdFolder's", c.request(op="get", target="class:OsdFolder", name="show_hidden").get("result") == "0"
      and c.request(op="get", target="class:OsdFolder", name="icon_font").get("result") == "11.Serif")
pp = props(DESK + "/Plain")
check("... a folder shows it, from class:OsdFolder", pp.get("show_hidden", {}).get("value") == "0"
      and pp.get("icon_font", {}).get("origin") == "class:OsdFolder", pp.get("show_hidden"))
r = c.request(op="set", target=DESK + "/Plain", setup="show_hidden=1")
check("... one folder shows hidden files anyway", get(DESK + "/Plain", "show_hidden") == "1" and get(P, "show_hidden") == "0")
check("System reads class:OsdFolder back", get(sysobj, "font_folders") == "11.Serif", get(sysobj, "font_folders"))
c.request(op="reset", target="class:OsdFolder", names="show_hidden;icon_font;show_ext")

# notebooks (object-properties-design.md 2.2): own values, inherited ones' source
c.request(op="set", target="class:OsdFolder", setup="bg_color=#123456")
c.request(op="set", target=P, setup="bg_mode=tiled")


def fields_of(target):
    res = c.request(op="settings", target=target).get("result") or []
    return {f["key"]: f for pg in res for f in pg["fields"]}


fp = fields_of(P)
check("a field with its own value says so (own)", fp.get("bg_mode", {}).get("own") is True, fp.get("bg_mode"))
check("an inherited one says where from", fp.get("bg_color", {}).get("from") == "Folder default", fp.get("bg_color"))
check("a default one says neither", "own" not in fp.get("bg_image", {}) and "from" not in fp.get("bg_image", {}), fp.get("bg_image"))
check("a title: never own (not reset)", "own" not in fp.get("title", {}), fp.get("title"))
cs = c.request(op="settings", target="class:OsdFolder")
cf = {f["key"]: f for pg in (cs.get("result") or []) for f in pg["fields"]}
check("class:OsdFolder's notebook: its settings", cf.get("bg_color", {}).get("value") == "#123456" and cf.get("bg_color", {}).get("own") is True, cf.get("bg_color"))
check("... no title, icon or template", not ({"title", "icon", "template", "local"} & set(cf)), sorted(cf)[:8])
check("... titled for all of them", (cs.get("object") or {}).get("title") == "All of class Folder", cs.get("object"))
r = c.request(op="settings_set", target="class:OsdFolder", values={"icon_titles": 1, "lock_all": True})
check("... its notebook sets them", r.get("ok") and get(DESK + "/Plain", "icon_titles") == "two lines" and get(DESK + "/Plain", "lock_all") == "1", r)
c.request(op="reset", target="class:OsdTrashCan", names="bg_color")
tc = {f["key"]: f for pg in (c.request(op="settings", target="class:OsdTrashCan").get("result") or []) for f in pg["fields"]}
check("class:OsdTrashCan's: from Folder default", tc.get("bg_color", {}).get("from") == "Folder default", tc.get("bg_color"))
c.request(op="reset", target="class:OsdFolder", names="bg_color;icon_titles;lock_all")

# events' actions (object-properties-design.md 2.4)
import time
lst = c.request(op="list", target=DESK, offset=0, count=200, details=True).get("result") or {}
prog = next((it["name"][1:] if it["name"].startswith("\x01") else DESK + "/" + it["name"])
            for it in lst.get("items", []) if it.get("title") == "My program")
out = T + "/opened.txt"
r = c.request(op="set", target=prog, setup="on:object.open=run:echo $OSD_EVENT $OSD_CLASS > " + out + " | sound:bell")
check("an action set on a program", r.get("ok"), r)
r = c.request(op="open", target=prog)
check("opened: its sound comes back for the GUI", (r.get("result") or {}).get("sound") == "bell", r)
for _ in range(50):
    if os.path.exists(out) and open(out).read().strip():
        break
    time.sleep(0.1)
check("... and run: ran, with its event and class", os.path.exists(out) and open(out).read().split() == ["object.open", "OsdProgram"],
      open(out).read() if os.path.exists(out) else "no file")
c.request(op="set", target=prog, setup="on:object.open=none")
r = c.request(op="open", target=prog)
check("none: silence (an empty sound)", (r.get("result") or {}).get("sound") == "", r)
c.request(op="reset", target=prog, names="on:object.open")
r = c.request(op="open", target=prog)
check("none of its own: no sound in the reply (the Sound object's)", "sound" not in (r.get("result") or {}), r)
r = c.request(op="set", target=prog, setup="on:object.open=beep:loud")
check("an unknown kind of action is refused", not r.get("ok"), r)
r = c.request(op="set", target="class:OsdFolder", setup=r"on:object.create=set:bg_color=#010203")
check("all folders: set: when made", r.get("ok"), r)
r = c.request(op="create", folder=DESK, **{"class": "OsdFolder"}, title="Made")
made = ((r.get("result") or {}).get("path")) or ""
check("a folder made: its own colour set by the action", made and get(made, "bg_color") == "#010203" and props(made)["bg_color"]["origin"] == "object",
      (made, get(made, "bg_color") if made else None))
c.request(op="reset", target="class:OsdFolder", names="on:object.create")

# before going to the Trash Can: the object's own action (run: with its path)
open(DESK + "/old.txt", "w").write("x")
mark = T + "/trashed.txt"
r = c.request(op="set", target=DESK + "/old.txt", setup="on:object.trash=run:echo \"$OSD_PATH\" > " + mark)
check("an action when put in the Trash Can", r.get("ok"), r)
sid = c.request(op="select", folder=DESK, items=["old.txt"]).get("result")
r = c.request(op="trash", selection=sid)
for _ in range(50):
    if os.path.exists(mark) and open(mark).read().strip():
        break
    time.sleep(0.1)
check("... ran with its path before it went", os.path.exists(mark) and open(mark).read().strip() == DESK + "/old.txt",
      open(mark).read() if os.path.exists(mark) else r)

# a set: action is checked when typed; one that fails is told in Messages
r = c.request(op="set", target=prog, setup="on:object.open=set:bg_color #336699")
check("set: without key=value is refused", not r.get("ok"), r)
before = len((c.request(op="msg_list").get("result") or {}).get("messages") or [])
c.request(op="set", target=prog, setup="on:object.open=call:osdNoSuchMethod 1")
c.request(op="open", target=prog)
msgs = (c.request(op="msg_list").get("result") or {}).get("messages") or []
check("an action that fails: a message says which and why", len(msgs) == before + 1 and "osdNoSuchMethod" in msgs[0].get("body", "")
      and msgs[0].get("app") == "Objectspace", msgs[:1])
c.request(op="reset", target=prog, names="on:object.open")

# a file whose name says nothing: its type from its contents (an empty Data File is text)
open(DESK + "/Data File", "w").close()
mt = (c.request(op="call", target=DESK + "/Data File", method="osdQueryMimeType").get("result"))
check("an empty Data File (no extension) is text/plain", mt == "text/plain", mt)
c.request(op="set", target="type:text/plain", setup="on:object.open=sound:bell")
pd = props(DESK + "/Data File")
check("... so all text files' action is its", pd.get("on:object.open", {}).get("origin") == "type:text/plain", pd.get("on:object.open"))
c.request(op="reset", target="type:text/plain", names="on:object.open")

# the built-in Folder template makes folders: its Class... is all folders'
tl = c.request(op="list", target=T + "/templates", offset=0, count=50, details=True).get("result") or {}
ftpl = next(((it["name"][1:] if it["name"].startswith("\x01") else T + "/templates/" + it["name"])
             for it in tl.get("items", []) if it.get("title") == "Folder"), None)
so = (c.request(op="settings", target=ftpl).get("object") or {}) if ftpl else {}
check("the Folder template says it makes folders (Class...: class:OsdFolder)", so.get("makes") == "OsdFolder" and so.get("makes_title") == "Folder", so)

# Linux keeps some (object-properties-design.md 2.6): a type's program (mimeapps.list)
tp = props("type:text/plain").get("open_with") or {}
apps = [a for a in tp.get("choices", "").split(",") if a]
check("all text files: Opens with, the type's programs to choose from", len(apps) >= 1, tp)
if apps:
    pick = apps[-1]
    r = c.request(op="set", target="type:text/plain", setup="open_with=" + pick)
    ml = T + "/xdg/config/mimeapps.list"
    text = open(ml).read() if os.path.exists(ml) else ""
    check("... set: mimeapps.list's default for text/plain (shared with GNOME)", r.get("ok") and "text/plain=" in text, (r, text[:200]))
    check("... a text file says the same", get(DESK + "/notes.txt", "open_with") == pick, get(DESK + "/notes.txt", "open_with"))
    r = c.request(op="reset", target="type:text/plain", names="open_with")
    check("... kept by Linux: not reset by Objectspace", not r.get("ok"), r)
r = c.request(op="set", target="class:OsdDataFile", setup="open_with=x")
check("a class has no one type: no Opens with", not r.get("ok"), r)

# a folder's sort and columns: all folders' too (0.4 step 1)
srt = DESK + "/Sorted"
os.makedirs(srt, exist_ok=True)
for n in ("zeta.txt", "alpha.txt", "mid.txt"):
    open(srt + "/" + n, "w").write(n)
r = c.request(op="set", target="class:OsdFolder", setup="sort_key=name;col_size=0")
check("all folders: sort by name, no Size column", r.get("ok"), r)
ps = props(srt)
check("... a folder follows (from class:OsdFolder)", ps["sort_key"]["value"] == "name" and ps["sort_key"]["origin"] == "class:OsdFolder"
      and ps["col_size"]["value"] == "0", (ps["sort_key"], ps["col_size"]))
names = [it["name"] for it in (c.request(op="list", target=srt, offset=0, count=50).get("result") or {}).get("items", [])]
check("... and its listing is in that order", names == ["alpha.txt", "mid.txt", "zeta.txt"], names)
c.request(op="reset", target="class:OsdFolder", names="sort_key;col_size")

print("%d failed" % fails)
sys.exit(1 if fails else 0)
