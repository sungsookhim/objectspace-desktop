#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""Save a file the way GNOME Text Editor or gedit does, through their
GtkSourceView FileLoader and FileSaver (the same library calls the editors
make), with no window.

usage: gtksource_save.py PATH [5|300]
  5    GtkSourceView 5 on GTK 4, used by GNOME Text Editor (default)
  300  libgedit-gtksourceview on GTK 3, used by gedit 48 and later
Waits for a line on stdin after loading, then appends text and saves, so
the caller can separate load events from save events.
"""
import sys

import gi

flavour = sys.argv[2] if len(sys.argv) > 2 else "5"
gi.require_version("GtkSource", flavour)
gi.require_version("Gtk", "4.0" if flavour == "5" else "3.0")
from gi.repository import Gio, GLib, Gtk, GtkSource  # noqa: E402

if flavour == "5":
    Gtk.init()
if hasattr(GtkSource, "init"):
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
