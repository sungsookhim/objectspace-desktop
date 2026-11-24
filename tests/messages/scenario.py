"""Messages: wpsd's list (run by run.sh: part 1, then 2 after a restart)."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import harness  # noqa: E402

PART = sys.argv[1]
fails = 0


def check(what, ok, more=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + (("  (" + str(more) + ")") if not ok and more != "" else ""))
    fails += not ok


class Svc:
    env = {"WPS_RUNTIME_DIR": os.environ["WPS_RUNTIME_DIR"]}


c = harness.Conn(Svc())


def msgs():
    r = c.request(op="msg_list").get("result") or {}
    return r.get("messages", []), r.get("unread")


def changed(timeout=2.0):
    ev = c.events(timeout, until=lambda m: m.get("event") == "messages_changed")
    ev = [m for m in ev if m.get("event") == "messages_changed"]
    return ev[-1] if ev else None


if PART == "1":
    # a subscription: events reach this client
    c.request(op="subscribe", objects=[], folders=[os.environ["T"]], jobs=True)
    a = c.request(op="msg_add", app="Firefox", icon="firefox", summary="Download finished", body="file.zip",
                  urgency=1, nid=7, sender=":1.42", actions=["default", "Open", "show", "Show in folder"])
    check("msg_add answers an ID", isinstance(a.get("result"), int), a)
    ev = changed()
    check("messages_changed after an add (unread 1)", ev and ev.get("unread") == 1 and ev.get("added") == a.get("result"), ev)
    b = c.request(op="msg_add", app="Workplace Shell", summary="Cannot delete: x", body="", urgency=2)
    ms, unread = msgs()
    check("two messages, newest first", [m["summary"] for m in ms] == ["Cannot delete: x", "Download finished"], ms)
    check("both unread", unread == 2, unread)
    check("actions kept", ms[1].get("actions") == ["default", "Open", "show", "Show in folder"], ms[1].get("actions"))
    check("program, time, urgency kept", ms[1]["app"] == "Firefox" and abs(ms[1]["time"] - time.time()) < 60
          and ms[0]["urgency"] == 2, ms)
    r = c.request(op="msg_set", ids=[a["result"]], read=True)
    check("msg_set marks one read", r.get("result") == 1, r)
    ms, unread = msgs()
    check("one unread now", unread == 1 and ms[1]["read"] and not ms[0]["read"], ms)
    r = c.request(op="msg_closed", nid=7, sender=":1.42")
    ms, _ = msgs()
    check("msg_closed: its actions are gone", r.get("result") == 1 and ms[1]["closed"], ms)
    r = c.request(op="msg_closed", nid=7, sender=":1.99")
    check("msg_closed by another sender changes nothing", r.get("result") == 0, r)
    for i in range(510):
        c.request(op="msg_add", app="Spam", summary="n%d" % i)
    ms, _ = msgs()
    check("the last 500 are kept", len(ms) == 500 and ms[0]["summary"] == "n509" and ms[-1]["summary"] == "n10",
          (len(ms), ms[0]["summary"], ms[-1]["summary"]))
    r = c.request(op="msg_delete", all=True)
    ms, unread = msgs()
    check("Delete all", r.get("result") == 500 and ms == [] and unread == 0, (r, len(ms)))
    c.request(op="msg_add", app="Mail", summary="Kept", body="across a restart", nid=3, sender=":1.5", actions=["a", "A"])
    c.request(op="msg_add", app="Mail", summary="Deleted", body="")
    ms, _ = msgs()
    c.request(op="msg_delete", ids=[ms[0]["id"]])
    ms, _ = msgs()
    check("Delete one", [m["summary"] for m in ms] == ["Kept"], ms)
    # the settings objects of step 7
    s = c.request(op="setup").get("result") or {}
    objs = s.get("objects", {})
    for cls in ("WPLaunchPad", "WPWarpCenter", "WPMessages"):
        check(cls + " object made", cls in objs, list(objs))
    wc = objs.get("WPWarpCenter", {}).get("settings", {})
    check("WarpCenter defaults", wc.get("edge") == "bottom" and wc.get("clock") == "24 hours" and wc.get("meters") == "1", wc)
    m = objs.get("WPMessages", {})
    check("Messages defaults", m.get("settings", {}).get("dnd") == "0", m)
    sysdir = os.environ["T"] + "/xdg/data/wps/System"
    titles = [it.get("title") for it in (c.request(op="list", target=sysdir, offset=0, count=100, details=True)
                                          .get("result") or {}).get("items", [])]
    check("LaunchPad, WarpCenter and Messages are in System", all(t in titles for t in ("LaunchPad", "WarpCenter", "Messages")), titles)
    o = c.request(op="open", target=m.get("address", ""))
    check("opening Messages answers its view", (o.get("result") or {}).get("view") == "messages", o)
    r = c.request(op="settings_set", target=objs["WPWarpCenter"]["address"], values={"edge": "top"})
    s = c.request(op="setup").get("result") or {}
    check("the WarpCenter's edge is set", s["objects"]["WPWarpCenter"]["settings"].get("edge") == "top", r)
else:
    ms, unread = msgs()
    check("kept across a restart", [m["summary"] for m in ms] == ["Kept"] and unread == 1, ms)
    check("its fields too", ms and ms[0]["body"] == "across a restart" and ms[0]["actions"] == ["a", "A"] and ms[0]["nid"] == 3, ms)
    a = c.request(op="msg_add", app="X", summary="after")
    ms, _ = msgs()
    check("IDs go on after a restart", ms[0]["id"] > ms[1]["id"], [m["id"] for m in ms])
    s = c.request(op="setup").get("result") or {}
    check("settings kept across a restart", s["objects"]["WPWarpCenter"]["settings"].get("edge") == "top")
    n = len([1 for _ in s["objects"]])
    check("no settings object made twice", n == 9, list(s["objects"]))

print("%d failed" % fails)
sys.exit(1 if fails else 0)
