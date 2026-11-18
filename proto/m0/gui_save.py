#!/usr/bin/env python3
"""Save a file in a real GUI editor, driven through the accessibility bus.

Starts the editor on PATH, waits for its window to appear on the AT-SPI
bus, inserts text into its document through the accessible text
interface, then activates its Save button (or, for GTK 4 apps, the window's "page.save"
action) and quits the editor. Needs a
desktop session (Wayland or X); no input injection tools are used.

usage: gui_save.py gedit|gnome-text-editor PATH
Prints "loaded", waits for a line on stdin, then saves and prints "saved"
(or "error ...").
"""
import subprocess
import sys
import time
import warnings

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402

warnings.filterwarnings("ignore", category=DeprecationWarning)

APPS = {
    "gedit": (["gedit", "--new-window"], "gedit"),
    "gnome-text-editor": (["gnome-text-editor", "--new-window"], "gnome-text-editor"),
}


def find(acc, pred, depth=0, limit=40):
    if acc is None or depth > limit:
        return None
    try:
        if pred(acc):
            return acc
        for i in range(acc.get_child_count()):
            r = find(acc.get_child_at_index(i), pred, depth + 1, limit)
            if r:
                return r
    except Exception:
        return None
    return None


def app_named(name):
    desk = Atspi.get_desktop(0)
    for i in range(desk.get_child_count()):
        a = desk.get_child_at_index(i)
        try:
            if a and (a.get_name() or "").lower() == name:
                return a
        except Exception:
            pass
    return None


def wait_for(fn, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        r = fn()
        if r:
            return r
        time.sleep(0.2)
    return None


def main():
    kind, path = sys.argv[1], sys.argv[2]
    argv, busname = APPS[kind]
    proc = subprocess.Popen(argv + [path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    app = wait_for(lambda: app_named(busname))
    if not app:
        print("error the editor did not appear on the accessibility bus", flush=True)
        proc.kill()
        return
    text = wait_for(lambda: find(app, lambda a: a.get_role() == Atspi.Role.TEXT
                                 and a.get_editable_text_iface() is not None))
    if not text:
        print("error no editable text found", flush=True)
        proc.kill()
        return
    print("loaded", flush=True)
    sys.stdin.readline()
    t = text.get_text_iface()
    end = Atspi.Text.get_character_count(t)
    Atspi.EditableText.insert_text(text.get_editable_text_iface(), end, "edited\n", len("edited\n"))
    save = find(app, lambda a: a.get_role() in (Atspi.Role.PUSH_BUTTON, Atspi.Role.BUTTON)
                and (a.get_name() or "").strip().lower() in ("save", "_save"))
    index = 0
    if not save:
        # GTK 4 apps (GNOME Text Editor) expose window actions such as
        # "page.save" on the frame instead of a Save button.
        def save_action(a):
            ai = a.get_action_iface()
            if not ai:
                return False
            names = [Atspi.Action.get_action_name(ai, k) for k in range(Atspi.Action.get_n_actions(ai))]
            return any(n in ("page.save", "win.save", "app.save") for n in names)
        save = find(app, save_action, limit=3)
        if save:
            ai = save.get_action_iface()
            names = [Atspi.Action.get_action_name(ai, k) for k in range(Atspi.Action.get_n_actions(ai))]
            index = next(k for k, n in enumerate(names) if n in ("page.save", "win.save", "app.save"))
    if not save:
        print("error no Save button or save action found", flush=True)
        proc.kill()
        return
    Atspi.Action.do_action(save.get_action_iface(), index)
    time.sleep(2.0)
    print("saved", flush=True)
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()


if __name__ == "__main__":
    main()
