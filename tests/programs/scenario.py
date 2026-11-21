"""Programs: wpsd's keeper of the Programs folder (run by run.sh)."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import harness  # noqa: E402

T = os.environ["T"]
APPS = T + "/share/applications"
MINE = T + "/xdg/data/applications"
PROGS = T + "/xdg/data/wps/Programs"


def entry(path, name, extra=""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("[Desktop Entry]\nType=Application\nName=%s\nExec=true\n%s" % (name, extra))


if sys.argv[1:] == ["setup"]:
    entry(APPS + "/editor.desktop", "Editor", "Categories=Development;IDE;\n")
    entry(APPS + "/chess.desktop", "Chess", "Categories=Game;BoardGame;\n")
    entry(APPS + "/player.desktop", "Player", "Categories=AudioVideo;Player;\n")
    entry(APPS + "/plain.desktop", "Plain")
    entry(APPS + "/hidden.desktop", "Hidden", "NoDisplay=true\nCategories=Utility;\n")
    entry(APPS + "/kdeonly.desktop", "KDE only", "OnlyShowIn=KDE;\nCategories=Utility;\n")
    entry(APPS + "/notgnome.desktop", "Not GNOME", "NotShowIn=GNOME;\nCategories=Utility;\n")
    entry(APPS + "/missing.desktop", "Missing", "TryExec=no-such-program-here\nCategories=Utility;\n")
    entry(APPS + "/kde4/calc.desktop", "Calc", "Categories=Utility;Calculator;\n")
    entry(APPS + "/link.desktop", "A link", "")
    with open(APPS + "/link.desktop", "w") as f:
        f.write("[Desktop Entry]\nType=Link\nName=A link\nURL=https://example.org\n")
    sys.exit(0)

fails = 0


def check(what, ok, more=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + (("  (" + str(more) + ")") if not ok and more != "" else ""))
    fails += not ok


class Svc:
    env = {"WPS_RUNTIME_DIR": os.environ["WPS_RUNTIME_DIR"]}


c = harness.Conn(Svc())


def items(folder):
    r = c.request(op="list", target=folder, offset=0, count=200, details=True).get("result") or {}
    return r.get("items", [])


def tree():
    out = {}
    for cat in items(PROGS):
        if cat.get("type") != "folder":
            continue
        out[cat["name"]] = sorted(os.path.basename(x.get("target_path") or x["name"]) for x in items(PROGS + "/" + cat["name"]))
    return out


def wait_tree(cond, secs=6):
    t = time.time()
    while time.time() - t < secs:
        tr = tree()
        if cond(tr):
            return tr
        time.sleep(0.2)
    return tree()


c.request(op="first_start", desktop=T + "/desk")
tr = wait_tree(lambda tr: "Development" in tr and len(tr) >= 5)
want = {"Accessories": ["calc.desktop"], "Development": ["editor.desktop"], "Games": ["chess.desktop"],
        "Multimedia": ["player.desktop"], "Other": ["plain.desktop"]}
check("made by category; hidden, other desktops', missing TryExec and links left out", tr == want, tr)
r = c.request(op="resolve", target=PROGS)["result"]
check("Programs is a WPProgramsFolder", r.get("class") == "WPProgramsFolder", r.get("class"))
desk = [x.get("title") for x in items(T + "/desk")]
check("Programs on the desktop", "Programs" in desk, desk)
it = items(PROGS + "/Development")
check("the shadow's title: the entry's Name", it and it[0].get("title") == "Editor", it)

entry(APPS + "/browser.desktop", "Browser", "Categories=Network;WebBrowser;\n")
tr = wait_tree(lambda tr: "Internet" in tr)
check("installed: appears in its category", tr.get("Internet") == ["browser.desktop"], tr)

os.unlink(APPS + "/chess.desktop")
tr = wait_tree(lambda tr: not tr.get("Games"))
check("removed: its shadow goes", not tr.get("Games"), tr)

entry(APPS + "/player.desktop", "Player", "Categories=Graphics;Viewer;\n")
tr = wait_tree(lambda tr: tr.get("Graphics") == ["player.desktop"] and not tr.get("Multimedia"))
check("recategorised: moves", tr.get("Graphics") == ["player.desktop"] and not tr.get("Multimedia"), tr)

entry(APPS + "/plain.desktop", "Plain", "NoDisplay=true\n")
tr = wait_tree(lambda tr: not tr.get("Other"))
check("hidden now: its shadow goes", not tr.get("Other"), tr)

# the user deletes a shadow: not made again
ed = items(PROGS + "/Development")[0]["name"]
r = c.request(op="vdelete", target=ed[1:])
check("the user deletes Editor's shadow", r.get("ok"), r)
entry(MINE + "/mine.desktop", "Mine", "Categories=Office;\n")   # (another change: a sync)
tr = wait_tree(lambda tr: "Office" in tr)
check("a later sync: Editor not made again", tr.get("Development") == [] and tr.get("Office") == ["mine.desktop"], tr)

# the user's own shadow in a category folder: left alone
own = c.request(op="shadow", target=APPS + "/kde4/calc.desktop", folder=PROGS + "/Office")
check("the user's own shadow made", own.get("ok"), own)
entry(APPS + "/another.desktop", "Another", "Categories=Office;\n")
tr = wait_tree(lambda tr: "another.desktop" in tr.get("Office", []))
check("a later sync leaves the user's own shadow", sorted(tr.get("Office", [])) == ["another.desktop", "calc.desktop", "mine.desktop"], tr)

# the user's own copy in ~/.local/share/applications wins over the system's
entry(MINE + "/browser.desktop", "My Browser", "Categories=Network;\n")
time.sleep(2.5)
it = items(PROGS + "/Internet")
check("the user's entry wins (same desktop file ID)", len(it) == 1 and it[0].get("target_path", "").startswith(MINE), it)

sys.exit(1 if fails else 0)
