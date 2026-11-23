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
want = {"Accessories": ["calc.desktop"], "Development": ["editor.desktop"], "Games": ["chess.desktop"],
        "Multimedia": ["player.desktop"], "Other": ["plain.desktop"]}
tr = wait_tree(lambda tr: tr == want)   # (the folders come before their shadows)
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

# an executable's Program page (0.4 step 6): shown for programs only;
# open follows it (its working directory; text mode in a terminal, kept
# open after it ends with Close window on exit off)
tool = T + "/desk/tool.sh"
with open(tool, "w") as f:
    f.write("#!/bin/sh\npwd > \"%s/ran.txt\"\n" % T)
os.chmod(tool, 0o755)
with open(T + "/desk/notes.txt", "w") as f:
    f.write("x")
def pages(t):
    return [p["title"] for p in c.request(op="settings", target=t).get("result", [])]
check("an executable has a Program page", "Program" in pages(tool), pages(tool))
check("a text file has none", "Program" not in pages(T + "/desk/notes.txt"), pages(T + "/desk/notes.txt"))
r = c.request(op="settings_set", target=tool, values={"prog_cwd": T + "/share", "prog_type": 1})
check("its page's settings taken", r.get("ok"), r)
r = c.request(op="open", target=tool)
deadline = time.time() + 5
while time.time() < deadline and not os.path.exists(T + "/ran.txt"):
    time.sleep(0.1)
ran = open(T + "/ran.txt").read().strip() if os.path.exists(T + "/ran.txt") else None
check("opened: in its page's working directory", r.get("ok") and ran == T + "/share", (r, ran))
c.request(op="settings_set", target=tool, values={"prog_type": 2, "prog_close": False})
r = c.request(op="open", target=tool)
deadline = time.time() + 5
while time.time() < deadline and not os.path.exists(T + "/term.txt"):
    time.sleep(0.1)
term = open(T + "/term.txt").read() if os.path.exists(T + "/term.txt") else ""
check("text mode: in a terminal, kept open after it ends", r.get("ok") and tool in term and "Press Enter" in term, (r, term))

# the click's activation token (0.4 step 6): the program has it, as
# XDG_ACTIVATION_TOKEN and DESKTOP_STARTUP_ID (its window to the front)
with open(tool, "w") as f:
    f.write("#!/bin/sh\necho \"$XDG_ACTIVATION_TOKEN $DESKTOP_STARTUP_ID\" > \"%s/tok.txt\"\n" % T)
c.request(op="settings_set", target=tool, values={"prog_type": 1})
r = c.request(op="open", target=tool, token_env=["XDG_ACTIVATION_TOKEN=tok-123", "DESKTOP_STARTUP_ID=tok-123", "PATH=/evil"])
deadline = time.time() + 5
while time.time() < deadline and not os.path.exists(T + "/tok.txt"):
    time.sleep(0.1)
tok = open(T + "/tok.txt").read().strip() if os.path.exists(T + "/tok.txt") else None
check("the activation token in its environment (both names)", r.get("ok") and tok == "tok-123 tok-123", (r, tok))

sys.exit(1 if fails else 0)
