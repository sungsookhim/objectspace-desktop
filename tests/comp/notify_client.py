"""A program that sends notifications and listens for what comes back
(tests/comp: the Objectspace session's notification service). Writes to argv[1]."""
import sys

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

out = open(sys.argv[1], "w", buffering=1)
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
for _ in range(100):   # the desktop takes the name when it starts
    if bus.name_has_owner("org.freedesktop.Notifications"):
        break
    GLib.usleep(100000)
n = dbus.Interface(bus.get_object("org.freedesktop.Notifications", "/org/freedesktop/Notifications"),
                   "org.freedesktop.Notifications")
print("server", *n.GetServerInformation(), file=out)
print("caps", ",".join(n.GetCapabilities()), file=out)


def invoked(nid, key):
    print("action", int(nid), key, file=out)


def closed(nid, reason):
    print("closed", int(nid), int(reason), file=out)


bus.add_signal_receiver(invoked, "ActionInvoked", "org.freedesktop.Notifications")
bus.add_signal_receiver(closed, "NotificationClosed", "org.freedesktop.Notifications")
a = n.Notify("Test Mail", 0, "mail-unread", "New mail from <b>Kim</b>",
             "Lunch tomorrow? &amp; a long line that wraps across the width of the pop-up window", 
             ["default", "Open", "reply", "Reply"], {"urgency": dbus.Byte(1)}, 0)
print("notify", int(a), file=out)
b = n.Notify("Test Mail", 0, "", "Second", "fades", [], {}, 1500)
print("notify", int(b), file=out)
GLib.timeout_add(int(float(sys.argv[2]) * 1000) if len(sys.argv) > 2 else 12000, lambda: GLib.MainLoop.quit(loop))
loop = GLib.MainLoop()
loop.run()
