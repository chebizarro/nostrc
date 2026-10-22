"""Throwaway Secret Service inside an already inspected contained Linux domain.

CTest fixtures keep their own bus/service ownership. This provider is only for
commands explicitly requiring both gnome_keyring and private_bus; namespace exit,
log drain and removal are attested by the existing host ContainerOwner, not PIDs.
"""
import hashlib
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile

from test_selection_custody import CustodyError, file_hash, write_json

NAME = 'org.freedesktop.secrets'
ROOT = '/org/freedesktop/secrets'
PROTECTED = ('HOME', 'XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME',
             'XDG_RUNTIME_DIR', 'DBUS_SESSION_BUS_ADDRESS', 'GNOME_KEYRING_CONTROL')


def connection(address):
    from gi.repository import Gio
    return Gio.DBusConnection.new_for_address_sync(address,
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        None, None)


def call(bus, destination, path, interface, method, signature=None, values=None):
    from gi.repository import Gio, GLib
    return bus.call_sync(destination, path, interface, method,
        GLib.Variant(signature, values) if signature else None, None,
        Gio.DBusCallFlags.NO_AUTO_START, 5000, None).unpack()


def owner_pid(bus):
    owner = call(bus, 'org.freedesktop.DBus', '/org/freedesktop/DBus',
        'org.freedesktop.DBus', 'GetNameOwner', '(s)', (NAME,))[0]
    pid = call(bus, 'org.freedesktop.DBus', '/org/freedesktop/DBus',
        'org.freedesktop.DBus', 'GetConnectionUnixProcessID', '(s)', (owner,))[0]
    return owner, pid


def wait_owner(bus, timeout=15):
    """Initial owner query plus NameOwnerChanged notification; no readiness sleep."""
    from gi.repository import Gio, GLib
    loop = GLib.MainLoop()
    found = []
    watch = Gio.bus_watch_name_on_connection(bus, NAME, Gio.BusNameWatcherFlags.NONE,
        lambda *args: (found.append(args[-1]), loop.quit()), lambda *args: None)
    timer = GLib.timeout_add_seconds(timeout, lambda: (loop.quit(), False)[1])
    try:
        loop.run()
        if not found:
            raise CustodyError('throwaway Secret Service did not acquire its private bus name')
        return owner_pid(bus)
    finally:
        Gio.bus_unwatch_name(watch)
        if found:
            GLib.source_remove(timer)


def roundtrip(bus):
    """Prove an unlocked, noninteractive real service, not mere daemon presence."""
    from gi.repository import GLib
    _, session = call(bus, NAME, ROOT, 'org.freedesktop.Secret.Service', 'OpenSession',
        '(sv)', ('plain', GLib.Variant('s', '')))
    item = None
    try:
        collection = call(bus, NAME, ROOT, 'org.freedesktop.Secret.Service', 'ReadAlias', '(s)', ('default',))[0]
        if collection == '/':
            raise CustodyError('throwaway Secret Service has no unlocked default collection')
        nonce = secrets.token_bytes(32)
        properties = {'org.freedesktop.Secret.Item.Label': GLib.Variant('s', 'nostrc contained readiness'),
            'org.freedesktop.Secret.Item.Attributes': GLib.Variant('a{ss}', {'nostrc-runtime-probe': secrets.token_hex(16)})}
        item, prompt = call(bus, NAME, collection, 'org.freedesktop.Secret.Collection', 'CreateItem',
            '(a{sv}(oayays)b)', (properties, (session, b'', nonce, 'application/octet-stream'), False))
        if item == '/' or prompt != '/':
            raise CustodyError('throwaway Secret Service requested interactive item creation')
        value = call(bus, NAME, item, 'org.freedesktop.Secret.Item', 'GetSecret', '(o)', (session,))[0]
        if bytes(value[2]) != nonce:
            raise CustodyError('throwaway Secret Service secret roundtrip differs')
        prompt = call(bus, NAME, item, 'org.freedesktop.Secret.Item', 'Delete')[0]
        if prompt != '/':
            raise CustodyError('throwaway Secret Service requested interactive item deletion')
        item = None
        return {'create_read_delete': True, 'secret_sha256': hashlib.sha256(nonce).hexdigest(),
                'collection': collection, 'session_closed': True}
    finally:
        if item and item != '/':
            call(bus, NAME, item, 'org.freedesktop.Secret.Item', 'Delete')
        call(bus, NAME, session, 'org.freedesktop.Secret.Session', 'Close')


def provision(context, state):
    context.validate_namespace()
    if context.control['owner']['initial_inspect']['HostConfig'].get('Tmpfs') != {'/tmp': 'rw,nosuid,nodev,size=1g'}:
        raise CustodyError('contained keyring requires inspected private /tmp tmpfs')
    if 'private_bus' not in state or state['private_bus']['address'] != os.environ.get('DBUS_SESSION_BUS_ADDRESS'):
        raise CustodyError('contained keyring service requires the separately provisioned private_bus')
    executable = shutil.which('gnome-keyring-daemon')
    if not executable:
        raise CustodyError('actual gnome-keyring-daemon executable is missing')
    home = Path(tempfile.mkdtemp(prefix='nostrc-keyring-', dir='/tmp'))
    for variable in PROTECTED[:5]:
        directory = home / variable
        directory.mkdir(mode=0o700)
        os.environ[variable] = str(directory)
    control = Path(os.environ['XDG_RUNTIME_DIR']) / 'keyring'
    control.mkdir(mode=0o700)
    os.environ['GNOME_KEYRING_CONTROL'] = str(control)
    protected = {key: os.environ[key] for key in PROTECTED}
    validate_directories(str(home), protected)
    # An authored phase must not redirect this provider into ambient resources.
    check_environment(context.control['profile'], protected)
    bus = connection(protected['DBUS_SESSION_BUS_ADDRESS'])
    try:
        if call(bus, 'org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                'NameHasOwner', '(s)', (NAME,))[0]:
            raise CustodyError('private bus already has a Secret Service; refusing replacement')
        argv = [str(Path(executable).resolve()), '--foreground', '--unlock', '--components=secrets',
                '--control-directory=' + str(control)]
        with tempfile.TemporaryFile() as password, open('/work/private-keyring.raw', 'wb') as log:
            password.write(secrets.token_bytes(32).hex().encode())
            password.seek(0)
            daemon = subprocess.Popen(argv, stdin=password, stdout=log, stderr=subprocess.STDOUT)
        context.keyring_daemon = daemon  # Keep the real child handle; never signal a numeric PID.
        owner, pid = wait_owner(bus)
        if daemon.poll() is not None or pid != daemon.pid:
            raise CustodyError('private Secret Service owner is not the live foreground child')
        probe = roundtrip(bus)
        proof = {'kind': 'contained_keyring_runtime_v1', 'binding': context.control['binding'],
            'container_id': context.control['container_id'], 'run_id': context.control['owner']['run_id'],
            'pid_namespace': context.namespace, 'environment': protected, 'home_root': str(home),
            'home_scope': 'private_tmpfs:/tmp', 'bus_owner': owner, 'service_pid': pid,
            'executable': {'executable': 'gnome-keyring-daemon', 'path': argv[0], 'sha256': file_hash(argv[0])},
            'argv': argv, 'probe': probe, 'setup_verified': True,
            'cleanup_authority': 'host_container_namespace_exit_drain_removal'}
        context.keyring_runtime = proof
        state['keyring_runtime'] = proof
        Path('/work/execution').mkdir(exist_ok=True)
        write_json('/work/execution/keyring-setup.json', proof)
    finally:
        bus.close_sync(None)


def check_environment(profile, expected, commands=(), tests=()):
    overlays = [profile.get('environment', {})] + list(profile.get('phase_environments', {}).values())
    overlays += [spec.get('environment', {}) for spec in commands]
    for overlay in overlays:
        if any(key in overlay and overlay[key] != expected[key] for key in PROTECTED):
            raise CustodyError('authored environment redirects isolated keyring resources')
    for test in tests:
        for prop in test.get('properties', []):
            if prop['name'] in {'ENVIRONMENT', 'ENVIRONMENT_MODIFICATION'}:
                values = prop['value'] if isinstance(prop['value'], list) else [prop['value']]
                if any(str(value).split('=', 1)[0] in PROTECTED for value in values):
                    raise CustodyError('CTest environment redirects isolated keyring resources')


def validate_directories(home_root, environment):
    root = Path(home_root)
    if root.parent != Path('/tmp') or not root.name.startswith('nostrc-keyring-'):
        raise CustodyError('isolated keyring private HOME root differs')
    paths = [root] + [Path(environment[key]) for key in PROTECTED[:5]]
    paths.append(Path(environment['GNOME_KEYRING_CONTROL']))
    for path in paths:
        if (not path.is_relative_to(root) or path.resolve(strict=True) != path or not path.is_dir()
                or path.stat().st_uid != os.getuid() or path.stat().st_mode & 0o077
                or path.stat().st_dev != Path('/tmp').stat().st_dev):
            raise CustodyError('isolated keyring directory ancestry/ownership/tmpfs placement changed')


def validate(context, realization, resolved):
    proof = context.keyring_runtime
    if not proof:
        raise CustodyError('contained command keyring setup missing')
    context.validate_namespace()
    expected = proof['environment']
    if any(os.environ.get(key) != value for key, value in expected.items()):
        raise CustodyError('isolated keyring environment changed')
    selected = set(resolved.get('tests', []))
    check_environment(realization['profile'], expected, resolved.get('command_executions', {}).values(),
                      [test for test in realization['tests'] if test['name'] in selected])
    validate_directories(proof['home_root'], expected)
    if file_hash(proof['executable']['path']) != proof['executable']['sha256']:
        raise CustodyError('contained keyring executable changed')
    bus = connection(expected['DBUS_SESSION_BUS_ADDRESS'])
    try:
        if context.keyring_daemon.poll() is not None or owner_pid(bus) != (proof['bus_owner'], proof['service_pid']):
            raise CustodyError('contained Secret Service owner changed or exited')
    finally:
        bus.close_sync(None)
    return proof
