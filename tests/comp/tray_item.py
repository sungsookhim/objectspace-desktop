"""Programs' status icons for the WarpCenter's tray (tests/comp/tray.sh): item
A registers by bus name, with a red IconPixmap and a dbusmenu menu; item B
by object path (as Ayatana's libraries do), with a theme icon; item C is
Passive (not shown). After 4 s A needs attention: its attention pixmap is
blue. Writes what reaches them to argv[1]; quits after argv[2] seconds."""
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

out = open(sys.argv[1], "w", buffering=1)
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
IFACE = "org.kde.StatusNotifierItem"
MENU = "com.canonical.dbusmenu"


def pixmap(r, g, b, size=22):
    return dbus.Array([dbus.Struct((dbus.Int32(size), dbus.Int32(size),
                                    dbus.Array(bytes([255, r, g, b]) * (size * size), signature="y")),
                                   signature=None)], signature="(iiay)")


class Item(dbus.service.Object):
    def __init__(self, path, name, props):
        dbus.service.Object.__init__(self, bus, path)
        self.name, self.props = name, props

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        return self.props[prop]

    @dbus.service.method(IFACE, in_signature="ii")
    def Activate(self, x, y):
        print("activate", self.name, file=out)

    @dbus.service.method(IFACE, in_signature="ii")
    def SecondaryActivate(self, x, y):
        print("secondary", self.name, file=out)

    @dbus.service.method(IFACE, in_signature="ii")
    def ContextMenu(self, x, y):
        print("contextmenu", self.name, file=out)

    @dbus.service.signal(IFACE, signature="s")
    def NewStatus(self, status):
        pass


def node(i, label, kids=(), **extra):
    props = {"label": label}
    props.update(extra)
    return dbus.Struct((dbus.Int32(i), dbus.Dictionary(props, signature="sv"),
                        dbus.Array([dbus.Struct(k, signature=None) for k in kids], signature="v")),
                       signature=None)


class Menu(dbus.service.Object):
    @dbus.service.method(MENU, in_signature="iias", out_signature="u(ia{sv}av)")
    def GetLayout(self, parent, depth, names):
        root = node(0, "", [
            node(1, "_Open Window"),
            node(2, "", type="separator"),
            node(3, "Mute", **{"toggle-type": "checkmark", "toggle-state": dbus.Int32(1)}),
            node(4, "Hidden", visible=False),
            node(5, "Quit"),
        ])
        return dbus.UInt32(1), root

    @dbus.service.method(MENU, in_signature="i", out_signature="b")
    def AboutToShow(self, i):
        return False

    @dbus.service.method(MENU, in_signature="isvu")
    def Event(self, i, kind, data, ts):
        print("event", int(i), kind, file=out)


for _ in range(100):   # the desktop takes the name when the bar opens
    if bus.name_has_owner("org.kde.StatusNotifierWatcher"):
        break
    GLib.usleep(100000)
w = dbus.Interface(bus.get_object("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher"),
                   "org.kde.StatusNotifierWatcher")
props = dbus.Interface(bus.get_object("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher"),
                       "org.freedesktop.DBus.Properties")
print("host", bool(props.Get("org.kde.StatusNotifierWatcher", "IsStatusNotifierHostRegistered")), file=out)

name = dbus.service.BusName("org.example.TestTray", bus)
a = Item("/StatusNotifierItem", "A", {
    "Id": "test-a", "Title": "Test A", "Status": "Active", "IconName": "", "IconPixmap": pixmap(220, 20, 20),
    "AttentionIconPixmap": pixmap(20, 20, 220), "ToolTip": dbus.Struct(("", pixmap(0, 0, 0, 1), "Tray test A", ""),
                                                                       signature="sa(iiay)ss"),
    "Menu": dbus.ObjectPath("/MenuBar"), "ItemIsMenu": False})
Menu(bus, "/MenuBar")
w.RegisterStatusNotifierItem("org.example.TestTray")
b = Item("/org/ayatana/NotificationItem/b", "B", {"Id": "test-b", "Title": "Test B", "Status": "Active",
                                                  "IconName": "dialog-information", "ItemIsMenu": False})
w.RegisterStatusNotifierItem("/org/ayatana/NotificationItem/b")
c = Item("/org/ayatana/NotificationItem/c", "C", {"Id": "test-c", "Status": "Passive", "IconName": "dialog-error"})
w.RegisterStatusNotifierItem("/org/ayatana/NotificationItem/c")
print("items", len(props.Get("org.kde.StatusNotifierWatcher", "RegisteredStatusNotifierItems")), file=out)


def attention():
    a.props["Status"] = "NeedsAttention"
    a.NewStatus("NeedsAttention")
    print("attention", file=out)
    return False


GLib.timeout_add(4000, attention)
loop = GLib.MainLoop()
GLib.timeout_add(int(float(sys.argv[2]) * 1000), loop.quit)
loop.run()
