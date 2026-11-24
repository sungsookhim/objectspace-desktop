#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""What a screen reader sees of osd-shell (G4 accessibility): listens on
AT-SPI, as Orca does, for focus and window events, prints each with the
object's role, name, description and position in its set; at the end,
the osd-shell applications on the desktop and their windows.

usage: tests/gui/a11y.py SECONDS   (in a session with the AT-SPI bus; see
tests/gui/a11y-run.sh)
"""
import sys
import gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, GLib


def describe(o):
    try:
        attrs = o.get_attributes() or {}
        pos = f" ({attrs['posinset']} of {attrs['setsize']})" if "posinset" in attrs else ""
        states = o.get_state_set()
        st = [n for n in ("focused", "selected", "checked", "expanded") if states.contains(getattr(Atspi.StateType, n.upper()))]
        text = ""
        if "Text" in (o.get_interfaces() or []):
            text = f" text '{Atspi.Text.get_text(o, 0, Atspi.Text.get_character_count(o))}'"
        return f"{o.get_role_name()} '{o.get_name()}' [{o.get_description()}]{pos}{text} {','.join(st)}"
    except Exception as e:  # noqa: BLE001
        return f"? ({e})"


def on_event(e):
    app = e.source.get_application()
    if not app or not app.get_name().startswith("osd-shell"):
        return
    print(f"a11y: {e.type}: {describe(e.source)}", flush=True)


def dump():
    d = Atspi.get_desktop(0)
    for i in range(d.get_child_count()):
        a = d.get_child_at_index(i)
        if a and a.get_name().startswith("osd-shell"):
            print(f"a11y: application {a.get_name()} ({a.get_toolkit_name()}), {a.get_child_count()} windows", flush=True)
            for k in range(a.get_child_count()):
                w = a.get_child_at_index(k)
                print(f"a11y:   {describe(w)}", flush=True)
                for c in range(w.get_child_count()):
                    print(f"a11y:     {describe(w.get_child_at_index(c))}", flush=True)
    Atspi.event_quit()
    return False


listener = Atspi.EventListener.new(on_event)
for ev in ("object:state-changed:focused", "object:state-changed:checked", "window:activate"):
    listener.register(ev)
GLib.timeout_add(int(float(sys.argv[1] if len(sys.argv) > 1 else 10) * 1000), dump)
Atspi.event_main()
