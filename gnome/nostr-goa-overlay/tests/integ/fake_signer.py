#!/usr/bin/env python3
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

BUS_NAME = 'org.nostr.Signer'
OBJ_PATH = '/org/nostr/Signer'
IFACE = 'org.nostr.Signer'

class Signer:
    def __init__(self):
        self.npub = 'npub1testintegration'

    def do_get_public_key(self, invocation):
        res = GLib.Variant('(s)', (self.npub,))
        invocation.return_value(res)

xml = f"""
<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node>
  <interface name="{IFACE}">
    <method name="GetPublicKey">
      <arg type="s" name="npub" direction="out"/>
    </method>
  </interface>
</node>
"""

class Obj(Gio.DBusInterfaceSkeleton):
    pass

def main():
    loop = GLib.MainLoop()
    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    node_info = Gio.DBusNodeInfo.new_for_xml(xml)
    iface_info = node_info.interfaces[0]
    vtable = Gio.DBusInterfaceVTable()

    def handle_call(conn, sender, obj_path, interface_name, method_name, params, invocation, user_data):
        if method_name == 'GetPublicKey':
            invocation.return_value(GLib.Variant('(s)', ('npub1testintegration',)))
            return True
        return False

    conn.register_object(OBJ_PATH, iface_info, vtable, handle_call, None, None)
    owner_id = Gio.bus_own_name_on_connection(conn, BUS_NAME, Gio.BusNameOwnerFlags.ALLOW_REPLACEMENT | Gio.BusNameOwnerFlags.REPLACE, None, None)

    try:
        loop.run()
    finally:
        Gio.bus_unown_name(owner_id)

if __name__ == '__main__':
    main()
