#!/usr/bin/env python3
"""Minimal org.freedesktop.secrets for a manual Groundhog smoke test on macOS.
'plain' sessions only; one always-unlocked collection 'login' (alias 'default').
Persists to a JSON file (plaintext!). TEST IDENTITIES ONLY."""
import json, os, sys, time, base64
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

STORE = sys.argv[1] if len(sys.argv) > 1 else '/tmp/ghsmoke/secrets.json'
BASE = '/org/freedesktop/secrets'
COLL = BASE + '/collection/login'
ALIAS = BASE + '/aliases/default'

XML = '''<node>
<interface name="org.freedesktop.Secret.Service">
 <method name="OpenSession"><arg type="s" direction="in"/><arg type="v" direction="in"/><arg type="v" direction="out"/><arg type="o" direction="out"/></method>
 <method name="CreateCollection"><arg type="a{sv}" direction="in"/><arg type="s" direction="in"/><arg type="o" direction="out"/><arg type="o" direction="out"/></method>
 <method name="SearchItems"><arg type="a{ss}" direction="in"/><arg type="ao" direction="out"/><arg type="ao" direction="out"/></method>
 <method name="Unlock"><arg type="ao" direction="in"/><arg type="ao" direction="out"/><arg type="o" direction="out"/></method>
 <method name="Lock"><arg type="ao" direction="in"/><arg type="ao" direction="out"/><arg type="o" direction="out"/></method>
 <method name="GetSecrets"><arg type="ao" direction="in"/><arg type="o" direction="in"/><arg type="a{o(oayays)}" direction="out"/></method>
 <method name="ReadAlias"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
 <method name="SetAlias"><arg type="s" direction="in"/><arg type="o" direction="in"/></method>
 <property name="Collections" type="ao" access="read"/>
 <signal name="CollectionCreated"><arg type="o"/></signal>
 <signal name="CollectionDeleted"><arg type="o"/></signal>
 <signal name="CollectionChanged"><arg type="o"/></signal>
</interface>
<interface name="org.freedesktop.Secret.Collection">
 <method name="Delete"><arg type="o" direction="out"/></method>
 <method name="SearchItems"><arg type="a{ss}" direction="in"/><arg type="ao" direction="out"/></method>
 <method name="CreateItem"><arg type="a{sv}" direction="in"/><arg type="(oayays)" direction="in"/><arg type="b" direction="in"/><arg type="o" direction="out"/><arg type="o" direction="out"/></method>
 <property name="Items" type="ao" access="read"/>
 <property name="Label" type="s" access="readwrite"/>
 <property name="Locked" type="b" access="read"/>
 <property name="Created" type="t" access="read"/>
 <property name="Modified" type="t" access="read"/>
 <signal name="ItemCreated"><arg type="o"/></signal>
 <signal name="ItemDeleted"><arg type="o"/></signal>
 <signal name="ItemChanged"><arg type="o"/></signal>
</interface>
<interface name="org.freedesktop.Secret.Item">
 <method name="Delete"><arg type="o" direction="out"/></method>
 <method name="GetSecret"><arg type="o" direction="in"/><arg type="(oayays)" direction="out"/></method>
 <method name="SetSecret"><arg type="(oayays)" direction="in"/></method>
 <property name="Locked" type="b" access="read"/>
 <property name="Attributes" type="a{ss}" access="readwrite"/>
 <property name="Label" type="s" access="readwrite"/>
 <property name="Created" type="t" access="read"/>
 <property name="Modified" type="t" access="read"/>
</interface>
<interface name="org.freedesktop.Secret.Session">
 <method name="Close"/>
</interface>
</node>'''
NODE = Gio.DBusNodeInfo.new_for_xml(XML)
IF = {i.name: i for i in NODE.interfaces}

items = {}   # id -> {label, attrs, secret(b64), ctype, created, modified}
counter = [0]
sessions = set()
regs = {}
conn = None

def load():
    if os.path.exists(STORE):
        d = json.load(open(STORE))
        items.update(d.get('items', {})); counter[0] = d.get('counter', 0)
def save():
    tmp = STORE + '.tmp'
    json.dump({'items': items, 'counter': counter[0]}, open(tmp, 'w'))
    os.replace(tmp, STORE)

def item_path(i): return COLL + '/' + i
def match(attrs, q): return all(attrs.get(k) == v for k, v in q.items())
def search(q): return [item_path(i) for i, it in items.items() if match(it['attrs'], q)]
def item_id(path):
    for pre in (COLL + '/', ALIAS + '/'):
        if path.startswith(pre): return path[len(pre):]
    return None
def secret_tuple(i, session):
    it = items[i]
    return (session, b'', base64.b64decode(it['secret']), it['ctype'])

def prop(obj, iface, name):
    if iface == 'org.freedesktop.Secret.Service':
        if name == 'Collections': return GLib.Variant('ao', [COLL])
    if iface == 'org.freedesktop.Secret.Collection':
        if name == 'Items': return GLib.Variant('ao', [item_path(i) for i in items])
        if name == 'Label': return GLib.Variant('s', 'Login')
        if name == 'Locked': return GLib.Variant('b', False)
        if name in ('Created', 'Modified'): return GLib.Variant('t', 0)
    if iface == 'org.freedesktop.Secret.Item':
        i = item_id(obj)
        if i not in items: return None
        it = items[i]
        if name == 'Locked': return GLib.Variant('b', False)
        if name == 'Attributes': return GLib.Variant('a{ss}', it['attrs'])
        if name == 'Label': return GLib.Variant('s', it['label'])
        if name == 'Created': return GLib.Variant('t', it['created'])
        if name == 'Modified': return GLib.Variant('t', it['modified'])
    return None

def emit(path, iface, sig, o):
    conn.emit_signal(None, path, iface, sig, GLib.Variant('(o)', (o,)))

def register_item(i):
    p = item_path(i)
    if p in regs: return
    regs[p] = conn.register_object(p, IF['org.freedesktop.Secret.Item'], on_call, on_get, on_set)

def on_call(c, sender, path, iface, method, params, inv):
    a = params.unpack()
    try:
        if iface == 'org.freedesktop.Secret.Service':
            if method == 'OpenSession':
                if a[0] != 'plain':
                    inv.return_dbus_error('org.freedesktop.DBus.Error.NotSupported', 'only plain'); return
                counter[0] += 1; sp = BASE + '/session/s%d' % counter[0]; sessions.add(sp)
                conn.register_object(sp, IF['org.freedesktop.Secret.Session'], on_call, None, None)
                inv.return_value(GLib.Variant('(vo)', (GLib.Variant('s', ''), sp))); return
            if method == 'CreateCollection':
                inv.return_value(GLib.Variant('(oo)', (COLL, '/'))); return
            if method == 'SearchItems':
                inv.return_value(GLib.Variant('(aoao)', (search(a[0]), []))); return
            if method in ('Unlock', 'Lock'):
                inv.return_value(GLib.Variant('(aoo)', (list(a[0]), '/'))); return
            if method == 'GetSecrets':
                out = {}
                for p in a[0]:
                    i = item_id(p)
                    if i in items: out[p] = secret_tuple(i, a[1])
                inv.return_value(GLib.Variant('(a{o(oayays)})', (out,))); return
            if method == 'ReadAlias':
                inv.return_value(GLib.Variant('(o)', (COLL if a[0] in ('default', 'login', 'session') else '/',))); return
            if method == 'SetAlias':
                inv.return_value(None); return
        if iface == 'org.freedesktop.Secret.Collection':
            if method == 'SearchItems':
                inv.return_value(GLib.Variant('(ao)', (search(a[0]),))); return
            if method == 'CreateItem':
                props, sec, replace = a
                attrs = dict(props.get('org.freedesktop.Secret.Item.Attributes', {}))
                label = props.get('org.freedesktop.Secret.Item.Label', '')
                now = int(time.time())
                existing = [i for i, it in items.items() if it['attrs'] == attrs] if replace else []
                if existing: i = existing[0]
                else:
                    counter[0] += 1; i = 'i%d' % counter[0]
                items[i] = {'label': label, 'attrs': attrs, 'secret': base64.b64encode(bytes(sec[2])).decode(),
                            'ctype': sec[3], 'created': items.get(i, {}).get('created', now), 'modified': now}
                save(); register_item(i)
                emit(COLL, 'org.freedesktop.Secret.Collection', 'ItemChanged' if existing else 'ItemCreated', item_path(i))
                inv.return_value(GLib.Variant('(oo)', (item_path(i), '/'))); return
            if method == 'Delete':
                inv.return_value(GLib.Variant('(o)', ('/',))); return
        if iface == 'org.freedesktop.Secret.Item':
            i = item_id(path)
            if i not in items:
                inv.return_dbus_error('org.freedesktop.Secret.Error.NoSuchObject', path); return
            if method == 'GetSecret':
                inv.return_value(GLib.Variant('((oayays))', (secret_tuple(i, a[0]),))); return
            if method == 'SetSecret':
                sec = a[0]; items[i]['secret'] = base64.b64encode(bytes(sec[2])).decode(); items[i]['ctype'] = sec[3]
                items[i]['modified'] = int(time.time()); save(); inv.return_value(None); return
            if method == 'Delete':
                del items[i]; save()
                p = item_path(i)
                if p in regs: conn.unregister_object(regs.pop(p))
                emit(COLL, 'org.freedesktop.Secret.Collection', 'ItemDeleted', p)
                inv.return_value(GLib.Variant('(o)', ('/',))); return
        if iface == 'org.freedesktop.Secret.Session' and method == 'Close':
            sessions.discard(path); inv.return_value(None); return
        inv.return_dbus_error('org.freedesktop.DBus.Error.UnknownMethod', method)
    except Exception as e:
        inv.return_dbus_error('org.freedesktop.DBus.Error.Failed', str(e))

def on_get(c, sender, path, iface, name):
    return prop(path, iface, name)

def on_set(c, sender, path, iface, name, value):
    i = item_id(path)
    if iface == 'org.freedesktop.Secret.Item' and i in items:
        if name == 'Attributes': items[i]['attrs'] = dict(value.unpack())
        if name == 'Label': items[i]['label'] = value.unpack()
        save()
    return True

def on_bus(c, name):
    global conn
    conn = c
    conn.register_object(BASE, IF['org.freedesktop.Secret.Service'], on_call, on_get, on_set)
    for p in (COLL, ALIAS):
        conn.register_object(p, IF['org.freedesktop.Secret.Collection'], on_call, on_get, on_set)
    for i in items: register_item(i)

def on_name(c, name): print('acquired', name, flush=True)
def on_lost(c, name): print('lost', name, flush=True); loop.quit()

load()
Gio.bus_own_name(Gio.BusType.SESSION, 'org.freedesktop.secrets', Gio.BusNameOwnerFlags.NONE, on_bus, on_name, on_lost)
loop = GLib.MainLoop(); loop.run()
