#!/usr/bin/env python3
"""Save a file the way GNOME Text Editor does: GtkSourceView 5's FileLoader
and FileSaver (the same library calls the editor makes), with no window.

usage: gtksource_save.py PATH
Waits for a line on stdin after loading, then appends text and saves, so
the caller can separate load events from save events.
"""
import sys

import gi

gi.require_version("GtkSource", "5")
gi.require_version("Gtk", "4.0")
from gi.repository import Gio, GLib, Gtk, GtkSource  # noqa: E402

Gtk.init()
GtkSource.init()
loop = GLib.MainLoop()
path = sys.argv[1]
buf = GtkSource.Buffer()
src = GtkSource.File()
src.set_location(Gio.File.new_for_path(path))
state = {}


def loaded(loader, res):
    loader.load_finish(res)
    print("loaded", flush=True)
    sys.stdin.readline()
    buf.insert(buf.get_end_iter(), "edited\n")
    saver = GtkSource.FileSaver.new(buf, src)
    saver.save_async(GLib.PRIORITY_DEFAULT, None, None, None, saved)


def saved(saver, res):
    try:
        saver.save_finish(res)
        state["ok"] = True
    except GLib.Error as e:
        state["error"] = e.message
    loop.quit()


GtkSource.FileLoader.new(buf, src).load_async(GLib.PRIORITY_DEFAULT, None, None, None, loaded)
loop.run()
print("saved" if state.get("ok") else "error " + state.get("error", "?"), flush=True)
