# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""A fake logind for the compositor's suspend test (0.6 step 6), on a
private bus: tests/comp/fake_logind.py ADDRESS LOG

What osd-compositor asks of logind: GetSession, GetSessionByPID, Inhibit
(a delay: a pipe's end), the session's SetIdleHint; and Suspend and
Hibernate, each written to LOG as a line ("suspend", "hibernate")."""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(sys.argv[1])
LOG = sys.argv[2]
MGR = "org.freedesktop.login1.Manager"
SES = "org.freedesktop.login1.Session"
SPATH = "/org/freedesktop/login1/session/test"
keep = []


def note(what):
    with open(LOG, "a") as f:
        f.write(what + "\n")


class Manager(dbus.service.Object):
    @dbus.service.method(MGR, in_signature="s", out_signature="o")
    def GetSession(self, sid):
        return dbus.ObjectPath(SPATH)

    @dbus.service.method(MGR, in_signature="u", out_signature="o")
    def GetSessionByPID(self, pid):
        return dbus.ObjectPath(SPATH)

    @dbus.service.method(MGR, in_signature="ssss", out_signature="h")
    def Inhibit(self, what, who, why, mode):
        r, w = os.pipe()
        keep.append(w)
        return dbus.types.UnixFd(r)

    @dbus.service.method(MGR, in_signature="b")
    def Suspend(self, interactive):
        note("suspend")

    @dbus.service.method(MGR, in_signature="b")
    def Hibernate(self, interactive):
        note("hibernate")

    @dbus.service.signal(MGR, signature="b")
    def PrepareForSleep(self, going):
        pass


class Session(dbus.service.Object):
    @dbus.service.method(SES, in_signature="b")
    def SetIdleHint(self, idle):
        note("idle-hint %s" % bool(idle))

    @dbus.service.signal(SES)
    def Lock(self):
        pass


name = dbus.service.BusName("org.freedesktop.login1", bus)
Manager(bus, "/org/freedesktop/login1")
Session(bus, SPATH)
note("ready")
GLib.MainLoop().run()
