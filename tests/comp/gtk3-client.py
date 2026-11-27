#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# A GTK 3 window for tests/comp/gtk3-theme.sh: a menu bar, a field, two
# buttons, a list with its first row selected, tabs; 480x320. It prints
# the theme it was drawn with, and quits after SECONDS (default 20).
import sys

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk  # noqa: E402

win = Gtk.Window(title="GTK3 test")
win.set_default_size(480, 320)
box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
bar = Gtk.MenuBar()
for name in ("File", "Edit", "Help"):
    bar.append(Gtk.MenuItem(label=name))
box.pack_start(bar, False, False, 0)
inner = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
inner.set_border_width(12)
entry = Gtk.Entry()
entry.set_text("Some text")
inner.pack_start(entry, False, False, 0)
row = Gtk.Box(spacing=8)
row.pack_start(Gtk.Button(label="OK"), False, False, 0)
row.pack_start(Gtk.Button(label="Cancel"), False, False, 0)
inner.pack_start(row, False, False, 0)
store = Gtk.ListStore(str)
for t in ("First (selected)", "Second", "Third"):
    store.append([t])
view = Gtk.TreeView(model=store)
view.append_column(Gtk.TreeViewColumn("Name", Gtk.CellRendererText(), text=0))
view.get_selection().select_path(Gtk.TreePath(0))
nb = Gtk.Notebook()
nb.append_page(view, Gtk.Label(label="List"))
nb.append_page(Gtk.Label(label="Other"), Gtk.Label(label="Other"))
inner.pack_start(nb, True, True, 0)
box.pack_start(inner, True, True, 0)
win.add(box)
win.connect("destroy", Gtk.main_quit)
win.show_all()
print("gtk3-client: theme", Gtk.Settings.get_default().get_property("gtk-theme-name"), flush=True)
GLib.timeout_add_seconds(int(sys.argv[1]) if len(sys.argv) > 1 else 20, Gtk.main_quit)
Gtk.main()
