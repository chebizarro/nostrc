#!/usr/bin/env python3
"""A CoreBluetooth-shaped FIPS advertiser on a BlueZ adapter, for the BlueZ
smoke (ble_bluez_smoke.py): it advertises the FIPS service UUID alone, with
no PSM service data, and serves the listener's PSM from the GATT PSM
characteristic of the FIPS service, as the macOS agent does. A fipsd node
on the same adapter (advertise and scan off) owns the L2CAP listener.

usage: ble_bluez_gatt_peer.py ADAPTER PSM   (prints 'gatt peer ready')
"""
import signal
import sys
import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

FIPS_UUID = '9c90b790-2cc5-42c0-9f87-c9cc40648f4c'
PSM_UUID = 'abdd3056-28fa-441d-a470-55a75a52553a'
ROOT = '/org/nostrc/fipsd/test/gatt'


class Characteristic(dbus.service.Object):
    def __init__(self, bus, path, service, psm):
        super().__init__(bus, path)
        self.path, self.service, self.value = path, service, [psm & 0xFF, psm >> 8]
        self.reads = 0

    def props(self):
        return {'org.bluez.GattCharacteristic1': {
            'Service': dbus.ObjectPath(self.service), 'UUID': PSM_UUID, 'Flags': dbus.Array(['read'], 's')}}

    @dbus.service.method('org.bluez.GattCharacteristic1', in_signature='a{sv}', out_signature='ay')
    def ReadValue(self, options):
        self.reads += 1
        print(f'gatt read {self.reads}', flush=True)
        return dbus.Array([dbus.Byte(b) for b in self.value], signature='y')


class Application(dbus.service.Object):
    def __init__(self, bus, psm):
        super().__init__(bus, ROOT)
        self.service = ROOT + '/service0'
        self.chr = Characteristic(bus, self.service + '/char0', self.service, psm)

    @dbus.service.method('org.freedesktop.DBus.ObjectManager', out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        return {dbus.ObjectPath(self.service): {'org.bluez.GattService1': {
                    'UUID': FIPS_UUID, 'Primary': True,
                    'Characteristics': dbus.Array([dbus.ObjectPath(self.chr.path)], 'o')}},
                dbus.ObjectPath(self.chr.path): self.chr.props()}


class Advertisement(dbus.service.Object):
    PATH = '/org/nostrc/fipsd/test/advert0'

    def __init__(self, bus):
        super().__init__(bus, self.PATH)

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        return {'Type': 'peripheral', 'ServiceUUIDs': dbus.Array([FIPS_UUID], 's')}

    @dbus.service.method('org.bluez.LEAdvertisement1')
    def Release(self):
        pass


def main():
    adapter, psm = sys.argv[1], int(sys.argv[2], 0)
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    loop = GLib.MainLoop()
    app, adv = Application(bus, psm), Advertisement(bus)
    obj = bus.get_object('org.bluez', f'/org/bluez/{adapter}')
    pending = {'n': 2}

    def done(_=None):
        pending['n'] -= 1
        if not pending['n']:
            print('gatt peer ready', flush=True)

    def failed(e):
        print(f'gatt peer failed: {e}', flush=True)
        loop.quit()
    dbus.Interface(obj, 'org.bluez.GattManager1').RegisterApplication(ROOT, {}, reply_handler=done, error_handler=failed)
    dbus.Interface(obj, 'org.bluez.LEAdvertisingManager1').RegisterAdvertisement(
        Advertisement.PATH, {}, reply_handler=done, error_handler=failed)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, loop.quit)
    loop.run()
    return 0


if __name__ == '__main__':
    sys.exit(main())
