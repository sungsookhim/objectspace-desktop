#!/usr/bin/env python3
"""A fake udisks2 for wpsd's drive tests (v0.2-plan step 4), on a private
bus: tests/fake_udisks.py ADDRESS MOUNT_ROOT

It has a system disk (root, mounted at "/"), a USB stick (not mounted), and
a LUKS volume (passphrase "secret"). Mounting makes MOUNT_ROOT/<label> and
reports it. Test controls on org.wps.FakeUDisks at /org/wps/Fake:
Plug(name), Pull(name) for "stick"; SetBusy(bool): Unmount fails busy;
SetAuth(bool): Mount fails not authorized."""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

UD = "org.freedesktop.UDisks2"
P = "/org/freedesktop/UDisks2"
OM = "org.freedesktop.DBus.ObjectManager"
PROPS = "org.freedesktop.DBus.Properties"

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(sys.argv[1])
MOUNT_ROOT = sys.argv[2]
busy = False
auth = True


def ay(s):
    return dbus.ByteArray(s.encode() + b"\0")


def mounts(*ps):
    return dbus.Array([ay(p) for p in ps], signature="ay")


class Err(dbus.DBusException):
    def __init__(self, name, msg):
        super().__init__(msg)
        self._dbus_error_name = UD + ".Error." + name


class Obj(dbus.service.Object):
    def __init__(self, path, ifaces):
        self.path = path
        self.ifaces = ifaces   # name -> {prop: value}
        super().__init__(bus, path)

    def set(self, iface, **kv):
        self.ifaces[iface].update(kv)
        self.PropertiesChanged(iface, dbus.Dictionary(kv, signature="sv"), dbus.Array([], signature="s"))

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        return self.ifaces[iface][prop]

    @dbus.service.method(UD + ".Filesystem", in_signature="a{sv}", out_signature="s")
    def Mount(self, opts):
        if not auth:
            raise Err("NotAuthorized", "Not authorized to perform operation")
        fs = self.ifaces[UD + ".Filesystem"]
        if len(fs["MountPoints"]):
            raise Err("AlreadyMounted", "already mounted")
        label = str(self.ifaces[UD + ".Block"]["IdLabel"]) or "disk"
        mp = os.path.join(MOUNT_ROOT, label)
        os.makedirs(mp, exist_ok=True)
        open(os.path.join(mp, "on-" + label + ".txt"), "w").write("x")
        self.set(UD + ".Filesystem", MountPoints=mounts(mp))
        return mp

    @dbus.service.method(UD + ".Filesystem", in_signature="a{sv}", out_signature="")
    def Unmount(self, opts):
        if busy:
            raise Err("DeviceBusy", "target is busy")
        if not len(self.ifaces[UD + ".Filesystem"]["MountPoints"]):
            raise Err("NotMounted", "not mounted")
        self.set(UD + ".Filesystem", MountPoints=mounts())

    @dbus.service.method(UD + ".Drive", in_signature="a{sv}", out_signature="")
    def Eject(self, opts):
        manager.pull("stick")

    @dbus.service.method(UD + ".Drive", in_signature="a{sv}", out_signature="")
    def PowerOff(self, opts):
        manager.pull("stick")

    @dbus.service.method(UD + ".Encrypted", in_signature="sa{sv}", out_signature="o")
    def Unlock(self, passphrase, opts):
        if passphrase != "secret":
            raise Err("Failed", "Error unlocking: wrong passphrase")
        o = manager.add(P + "/block_devices/dm_2d0", {
            UD + ".Block": block("/dev/dm-0", "clear-2222", "Secret", "ext4", "filesystem", 1 << 30, crypto=self.path),
            UD + ".Filesystem": {"MountPoints": mounts(), "Size": dbus.UInt64(1 << 30)},
        })
        self.set(UD + ".Encrypted", CleartextDevice=dbus.ObjectPath(o.path))
        return dbus.ObjectPath(o.path)

    @dbus.service.method(UD + ".Encrypted", in_signature="a{sv}", out_signature="")
    def Lock(self, opts):
        manager.remove(P + "/block_devices/dm_2d0")
        self.set(UD + ".Encrypted", CleartextDevice=dbus.ObjectPath("/"))


def block(dev, uuid, label, idtype, usage, size, drive="/", crypto="/", ignore=False, system=False):
    return {"Device": ay(dev), "IdUUID": uuid, "IdLabel": label, "IdType": idtype, "IdUsage": usage,
            "Size": dbus.UInt64(size), "Drive": dbus.ObjectPath(drive), "CryptoBackingDevice": dbus.ObjectPath(crypto),
            "HintIgnore": dbus.Boolean(ignore), "HintSystem": dbus.Boolean(system)}


def drive(bus_name, removable, ejectable=False, poweroff=False):
    return {"Removable": dbus.Boolean(removable), "Ejectable": dbus.Boolean(ejectable),
            "CanPowerOff": dbus.Boolean(poweroff), "ConnectionBus": bus_name,
            "MediaCompatibility": dbus.Array([], signature="s")}


class Manager(dbus.service.Object):
    def __init__(self):
        super().__init__(bus, P)
        self.objs = {}

    def add(self, path, ifaces):
        o = Obj(path, ifaces)
        self.objs[path] = o
        self.InterfacesAdded(dbus.ObjectPath(path), self.wire(o))
        return o

    def remove(self, path):
        o = self.objs.pop(path, None)
        if o:
            o.remove_from_connection()
            self.InterfacesRemoved(dbus.ObjectPath(path), dbus.Array(list(o.ifaces), signature="s"))

    def wire(self, o):
        return dbus.Dictionary({i: dbus.Dictionary(p, signature="sv") for i, p in o.ifaces.items()}, signature="sa{sv}")

    def plug(self, name):
        if name == "stick" and P + "/drives/Stick" not in self.objs:
            self.add(P + "/drives/Stick", {UD + ".Drive": drive("usb", True, True, True)})
            self.add(P + "/block_devices/sdc1", {
                UD + ".Block": block("/dev/sdc1", "AAAA-0001", "STICK", "vfat", "filesystem", 16 * 10**9, P + "/drives/Stick"),
                UD + ".Filesystem": {"MountPoints": mounts(), "Size": dbus.UInt64(16 * 10**9)},
            })

    def pull(self, name):
        if name == "stick":
            self.remove(P + "/block_devices/sdc1")
            self.remove(P + "/drives/Stick")

    @dbus.service.method(OM, in_signature="", out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        return dbus.Dictionary({dbus.ObjectPath(p): self.wire(o) for p, o in self.objs.items()}, signature="oa{sa{sv}}")

    @dbus.service.signal(OM, signature="oa{sa{sv}}")
    def InterfacesAdded(self, path, ifaces):
        pass

    @dbus.service.signal(OM, signature="oas")
    def InterfacesRemoved(self, path, ifaces):
        pass


class Control(dbus.service.Object):
    @dbus.service.method("org.wps.FakeUDisks", in_signature="s")
    def Plug(self, name):
        manager.plug(name)

    @dbus.service.method("org.wps.FakeUDisks", in_signature="s")
    def Pull(self, name):
        manager.pull(name)

    @dbus.service.method("org.wps.FakeUDisks", in_signature="b")
    def SetBusy(self, on):
        global busy
        busy = bool(on)

    @dbus.service.method("org.wps.FakeUDisks", in_signature="b")
    def SetAuth(self, on):
        global auth
        auth = bool(on)


manager = Manager()
Control(bus, "/org/wps/Fake")
manager.add(P + "/drives/Disk0", {UD + ".Drive": drive("sata", False)})
manager.add(P + "/block_devices/sda1", {
    UD + ".Block": block("/dev/sda1", "EFI-0000", "", "vfat", "filesystem", 600 * 10**6, P + "/drives/Disk0", ignore=True),
    UD + ".Filesystem": {"MountPoints": mounts("/boot/efi"), "Size": dbus.UInt64(0)},
})
manager.add(P + "/block_devices/sda2", {
    UD + ".Block": block("/dev/sda2", "ROOT-0000", "fedora", "btrfs", "filesystem", 500 * 10**9, P + "/drives/Disk0", system=True),
    UD + ".Filesystem": {"MountPoints": mounts("/", "/home"), "Size": dbus.UInt64(500 * 10**9)},
})
manager.add(P + "/block_devices/sda3", {
    UD + ".Block": block("/dev/sda3", "SWAP-0000", "", "swap", "other", 8 * 10**9, P + "/drives/Disk0"),
})
manager.add(P + "/block_devices/sdd1", {
    UD + ".Block": block("/dev/sdd1", "luks-1111", "", "crypto_LUKS", "crypto", 1 << 30, P + "/drives/Disk0"),
    UD + ".Encrypted": {"CleartextDevice": dbus.ObjectPath("/")},
})
manager.plug("stick")
bus.request_name(UD)
print("ready", flush=True)
GLib.MainLoop().run()
