"""Private bus/display children of the namespace's retained backend root.

No wrapper process sits above the backend finalizer. Each direct service is
reserved with a pidfd before cancellation is unblocked. Arbitrary descendants
remain the outer PID namespace's responsibility, never host PID/PGID cleanup.
This is resource setup, not tool capability or complete-profile attestation.
"""
import contextlib
import ctypes
import errno
import hashlib
import json
import os
import re
from pathlib import Path
import secrets
import select
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time


class ServiceError(RuntimeError):
    pass


def _ready(fd, timeout=15):
    deadline = time.monotonic() + timeout
    result = bytearray()
    while len(result) < 4096:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            raise ServiceError('private service readiness deadline exceeded')
        data = os.read(fd, 1)
        if not data:
            raise ServiceError('private service exited before readiness')
        if data == b'\n':
            return result.decode('ascii')
        result.extend(data)
    raise ServiceError('private service readiness record too large')


def _identity(name, search_path=None):
    executable = shutil.which(name, path=search_path)
    if not executable:
        raise ServiceError('private service executable unavailable: ' + name)
    path = Path(executable).resolve(strict=True)
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def _display_ready(number, cookie):
    """Authenticate a real X11 connection using the private cookie."""
    name, value = b'MIT-MAGIC-COOKIE-1', bytes.fromhex(cookie)
    pad = lambda data: data + b'\0' * (-len(data) % 4)
    request = struct.pack('<BBHHHHH', ord('l'), 0, 11, 0, len(name), len(value), 0)
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(15)
        peer.connect('/tmp/.X11-unix/X' + number)
        peer.sendall(request + pad(name) + pad(value))
        response = b''
        while len(response) < 8:
            data = peer.recv(8 - len(response))
            if not data:
                raise ServiceError('private display closed during authentication')
            response += data
        if response[0] != 1:
            raise ServiceError('private display authentication failed')


# AF_UNIX sun_path holds 107 bytes plus NUL. Retained evidence roots are deep
# (container /workspace/evidence/<run>/<lane>/..., host /private/tmp/...), so a
# socket below them fails in bind(): "Socket name too long". The tmpfs mountpoint
# therefore lives under a short owned directory, never below retained evidence.
UNIX_SOCKET_MAX = 107
RESOURCE_BASES = ('/tmp', '/var/tmp', '/dev/shm')
RESOURCE_PREFIX = 'nostrc-rr-'
# tempfile.mkdtemp('nip5f-private-') appends 8 characters; the bus is "bus" below.
FIXTURE_SOCKET_TAIL = len('/nip5f-private-') + 8 + len('/bus')
JOURNAL_NAME = 'resource-placement.jsonl'


def require_socket_budget(root, tail=FIXTURE_SOCKET_TAIL):
    """Refuse before launching a service whose socket path cannot be bound."""
    if len(os.fsencode(str(root))) + tail > UNIX_SOCKET_MAX:
        raise ServiceError('private resource root leaves no room for a UNIX socket path: ' + str(root))


def _journal_append(path, record, *, create=False):
    """Append-only, hash-chained, fsynced journal (O_EXCL on creation)."""
    previous = ''
    if not create:
        lines = path.read_bytes().splitlines()
        previous = hashlib.sha256(lines[-1]).hexdigest()
    record = dict(record, prev=previous)
    raw = (json.dumps(record, sort_keys=True) + '\n').encode()
    flags = os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW | (os.O_CREAT | os.O_EXCL if create else 0)
    fd = os.open(path, flags, 0o600)
    try:
        if os.write(fd, raw) != len(raw):
            raise ServiceError('resource placement journal short write')
        os.fsync(fd)
    finally:
        os.close(fd)


def read_resource_journal(path):
    path = Path(path)
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ServiceError('resource placement journal is not an owned private regular file')
    records, previous = [], ''
    for line in path.read_bytes().splitlines():
        record = json.loads(line)
        if record.get('prev') != previous:
            raise ServiceError('resource placement journal chain broken')
        previous = hashlib.sha256(line).hexdigest()
        records.append(record)
    if not records or records[0].get('state') != 'intent':
        raise ServiceError('resource placement journal lacks its intent record')
    return records


def allocate_resource_mountpoint(lane_dir, *, bases=RESOURCE_BASES):
    """Create the short, owned, empty 0700 mountpoint for the namespace tmpfs.

    The journal's intent record is only a plan: ownership exists once mkdir
    succeeded and the 'allocated' record captured the new directory's dev/ino.
    The tmpfs, HOME, runtime and sockets live only in the namespace mount and
    vanish with it; release_resource_mountpoint removes the empty host directory.
    """
    journal = Path(lane_dir) / JOURNAL_NAME
    token = secrets.token_hex(16)
    for base in bases:
        candidate = Path(base)
        try:
            info = candidate.lstat()
            usable = (stat.S_ISDIR(info.st_mode) and candidate.resolve(strict=True) == candidate
                      and os.access(candidate, os.W_OK | os.X_OK))
        except OSError:
            usable = False
        path = candidate / (RESOURCE_PREFIX + token)
        if usable and len(os.fsencode(str(path))) + FIXTURE_SOCKET_TAIL <= UNIX_SOCKET_MAX:
            break
    else:
        raise ServiceError('no short owned directory available for the private resource mountpoint')
    _journal_append(journal, {'state': 'intent', 'token': token, 'base': str(candidate), 'path': str(path),
                              'uid': os.geteuid(), 'pid': os.getpid()}, create=True)
    os.mkdir(path, 0o700)  # EEXIST: not ours; the journal stays intent-only
    info = os.lstat(path)
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise ServiceError('private resource mountpoint was not created owned/0700')
    _journal_append(journal, {'state': 'allocated', 'token': token, 'base': str(candidate), 'path': str(path),
                              'dev': info.st_dev, 'ino': info.st_ino})
    return path


def _journal_allocation(records):
    """The allocated record, only if it agrees with its intent (token/base/name)."""
    intent, allocated = records[0], records[1]
    if allocated.get('state') != 'allocated':
        raise ServiceError('resource placement journal lacks its allocated record')
    for field in ('token', 'base', 'path'):
        if intent.get(field) != allocated.get(field):
            raise ServiceError('resource placement journal intent/allocation disagree: ' + field)
    token, base, path = intent['token'], Path(intent['base']), Path(intent['path'])
    if (len(token) != 32 or any(c not in '0123456789abcdef' for c in token) or not base.is_absolute()
            or path != base / (RESOURCE_PREFIX + token)):
        raise ServiceError('resource placement journal path/token is not the allocator name')
    return allocated


def resource_mountpoint(lane_dir):
    """The allocated mountpoint named by the journal (namespace side)."""
    records = read_resource_journal(Path(lane_dir) / JOURNAL_NAME)
    if records[-1].get('state') != 'allocated' or len(records) != 2:
        raise ServiceError('resource placement journal is not in the allocated state')
    return Path(_journal_allocation(records)['path'])


def release_resource_mountpoint(lane_dir):
    """Outer cleanup after the namespace barrier; verified, journaled, idempotent.

    Only a directory this journal proved it created (allocated dev/ino, owner,
    mode, empty, unmounted) is ever removed, non-recursively. An intent-only
    journal never authorizes deletion: a present directory is refused and the
    incomplete recovery is retained; an absent one is recorded as absent.
    """
    journal = Path(lane_dir) / JOURNAL_NAME
    records = read_resource_journal(journal)
    last = records[-1]
    if last['state'] in ('released', 'absent'):
        return Path(last['path'])
    if last['state'] == 'recovery_refused':
        raise ServiceError('resource placement recovery refused earlier; unowned directory retained: ' + last['path'])
    if last['state'] == 'intent':
        path = Path(last['path'])
        try:
            os.lstat(path)
        except FileNotFoundError:
            _journal_append(journal, {'state': 'absent', 'path': str(path), 'allocation_proved': False})
            return path
        _journal_append(journal, {'state': 'recovery_refused', 'path': str(path),
                                  'reason': 'intent-only journal does not prove ownership'})
        raise ServiceError('intent-only resource placement journal cannot authorize removal of ' + str(path))
    if last['state'] != 'allocated' or len(records) != 2:
        raise ServiceError('unknown resource placement journal state')
    allocation = _journal_allocation(records)
    path = Path(allocation['path'])
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        raise ServiceError('private resource mountpoint vanished before release')
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise ServiceError('private resource mountpoint changed identity before release')
    if (info.st_dev, info.st_ino) != (allocation['dev'], allocation['ino']):
        raise ServiceError('private resource mountpoint was replaced before release')
    if os.path.ismount(path):
        raise ServiceError('private resource mountpoint is still mounted; namespace not gone')
    os.rmdir(path)  # ENOTEMPTY/EBUSY remain failures, never recursive deletion
    if path.exists():
        raise ServiceError('private resource mountpoint remained after release')
    _journal_append(journal, {'state': 'released', 'path': str(path), 'absent_after': True})
    return path


def prepare_resources(resource_root, display):
    """Helper-side setup, only in the new namespace init before target exec.

    The outer adapter creates the empty mountpoint. Nothing under the private
    HOME/runtime/socket mount survives namespace destruction or parent death.
    """
    if os.getpid() != 1:
        raise ServiceError('private resource mounts require namespace init')
    root = Path(resource_root)
    if not root.is_absolute() or root.is_symlink() or root.resolve(strict=True) != root:
        raise ServiceError('private resource mountpoint is not canonical')
    info = root.stat()
    if not root.is_dir() or info.st_uid != os.geteuid() or info.st_mode & 0o077 or any(root.iterdir()):
        raise ServiceError('private resource mountpoint is not empty/owned/private')
    sockets = Path('/tmp/.X11-unix')
    if display and (not sockets.is_dir() or sockets.is_symlink() or sockets.resolve() != sockets):
        raise ServiceError('private X11 requires an existing canonical socket mountpoint')
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mount.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_ulong, ctypes.c_void_p]

    def mount(source, target, filesystem, flags, options):
        if libc.mount(source, os.fsencode(target), filesystem, flags, options) == -1:
            error = ctypes.get_errno()
            raise ServiceError('private resource mount refused: ' + os.strerror(error))

    # Fresh user/mount namespace capabilities only; never host privilege setup.
    options = f'mode=0700,uid={os.geteuid()},gid={os.getegid()},size=64m'.encode()
    mount(b'tmpfs', root, b'tmpfs', 2 | 4, options)  # nosuid,nodev
    if display:
        private_sockets = root / 'x11'
        private_sockets.mkdir(mode=0o700)
        mount(os.fsencode(private_sockets), sockets, None, 4096, None)  # bind
    _namespace_resources(root, display)


def _namespace_resources(resource_root, display):
    """Require actual private mounts, not an environment/setup assertion."""
    if sys.platform != 'linux':
        raise ServiceError('private namespace resources require Linux')
    root = Path(resource_root)
    if not root.is_absolute() or root.is_symlink() or root.resolve(strict=True) != root:
        raise ServiceError('private resource root must be a canonical absolute directory')
    info = root.stat()
    if not root.is_dir() or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ServiceError('private resource root ownership/mode mismatch')
    mounts = {}
    for line in Path('/proc/self/mountinfo').read_text().splitlines():
        fields = line.split()
        # mountinfo escapes whitespace and backslashes with octal sequences.
        path = fields[4]
        for escaped, value in (('\\040', ' '), ('\\011', '\t'), ('\\012', '\n'), ('\\134', '\\')):
            path = path.replace(escaped, value)
        mounts[path] = fields[fields.index('-') + 1]
    if mounts.get(str(root)) != 'tmpfs':
        raise ServiceError('private resource root lacks its kernel tmpfs mount')
    if display and (mounts.get('/tmp/.X11-unix') != 'tmpfs' or
                    Path('/tmp/.X11-unix').stat().st_dev != info.st_dev):
        raise ServiceError('private X11 socket directory lacks the resource tmpfs binding')
    return root


@contextlib.contextmanager
def namespace_services(capabilities, evidence_dir, resource_root, finalizer_fd=None):
    root = _namespace_resources(resource_root, 'display' in capabilities)
    with private_services(capabilities, evidence_dir, resource_root=root, finalizer_fd=finalizer_fd) as state:
        os.environ['NOSTRC_NATIVE_RESOURCE_ROOT'] = str(root)
        state['resource_mount'] = {'root': str(root), 'filesystem': 'tmpfs',
                                   'private_x11': 'display' in capabilities}
        yield state


# Darwin custody private session bus. macOS has no tmpfs/mount namespace or
# abstract sockets: the private root is the journaled owned 0700 directory
# itself (allocated under canonical /private/tmp), removed by the outer
# adapter only after a validated helper exit. sun_path holds 103 bytes + NUL.
DARWIN_SOCKET_MAX = 103
DARWIN_RESOURCE_BASES = ('/private/tmp',)
DARWIN_BUS_CONFIG = ('<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" '
                     '"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">\n'
                     '<busconfig><type>session</type><listen>{address}</listen><auth>EXTERNAL</auth>'
                     '<policy context="default"><allow send_destination="*" eavesdrop="true"/>'
                     '<allow eavesdrop="true"/><allow own="*"/></policy></busconfig>\n')
AMBIENT_SESSION = ('DBUS_SESSION_BUS_ADDRESS', 'DBUS_SESSION_BUS_PID', 'DBUS_LAUNCHD_SESSION_BUS_SOCKET',
                   'DBUS_STARTER_ADDRESS', 'DBUS_STARTER_BUS_TYPE', 'DISPLAY', 'XAUTHORITY', 'GNOME_KEYRING_CONTROL')


@contextlib.contextmanager
def darwin_private_bus(evidence_dir, resource_root):
    """Private session bus inside Darwin custody (the caller is a tag member).

    The daemon is this process's direct, unreaped child and, like every
    descendant, carries the custody tag; the helper's reap owns any escapee.
    Its own minimal config has no launchd listen, service directories or
    includes, so no launchd/XPC activation is involved or inferred.
    """
    if sys.platform != 'darwin':
        raise ServiceError('Darwin private bus requires macOS')
    root = Path(resource_root)
    if not root.is_absolute() or root.is_symlink() or root.resolve(strict=True) != root:
        raise ServiceError('private resource root must be a canonical absolute directory')
    info = root.stat()
    if not root.is_dir() or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise ServiceError('private resource root ownership/mode mismatch')
    identities = {name: _identity(name) for name in ('dbus-daemon', 'dbus-send')}
    evidence = Path(evidence_dir)
    evidence.mkdir(parents=True, exist_ok=True)
    state = {'schema_version': 1, 'kind': 'darwin_private_bus_v1', 'capabilities': ['private_bus'],
             'executables': identities, 'services': [], 'status': 'failed', 'complete': False,
             'cleanup_scope': 'direct daemon reaped here; descendants by custody tag; files by outer adapter'}
    previous = dict(os.environ)
    private = Path(tempfile.mkdtemp(prefix='bus-', dir=root))
    socket_path = private / 'runtime' / 'bus'
    if len(os.fsencode(str(socket_path))) > DARWIN_SOCKET_MAX:
        raise ServiceError('private resource root leaves no room for the bus socket path: ' + str(socket_path))
    process = log = None
    original = None
    try:
        for name, path in {'HOME': private / 'home', 'XDG_RUNTIME_DIR': private / 'runtime',
                           'XDG_CONFIG_HOME': private / 'config', 'XDG_DATA_HOME': private / 'data',
                           'XDG_CACHE_HOME': private / 'cache'}.items():
            path.mkdir(mode=0o700)
            os.environ[name] = str(path)
        for name in AMBIENT_SESSION:
            os.environ.pop(name, None)
        config = private / 'bus.conf'
        config.write_text(DARWIN_BUS_CONFIG.format(address='unix:path=' + str(socket_path)))
        log = (evidence / 'dbus-daemon.log').open('xb')
        read_fd, write_fd = os.pipe()
        mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
        try:
            command = [identities['dbus-daemon']['path'], '--nofork', '--nopidfile', '--nosyslog',
                       '--config-file=' + str(config), '--print-address=' + str(write_fd)]
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, pass_fds=(write_fd,),
                                       restore_signals=True, start_new_session=True,
                                       preexec_fn=lambda: signal.pthread_sigmask(signal.SIG_SETMASK, mask))
            os.kill(process.pid, 0)  # direct, unreaped child: identity reserved
            record = {'executable': 'dbus-daemon', 'argv': command, 'log': str(log.name), 'pid': process.pid}
            state['services'].append(record)
        finally:
            os.close(write_fd)
            signal.pthread_sigmask(signal.SIG_SETMASK, mask)
        try:
            address = _ready(read_fd)
        finally:
            os.close(read_fd)
        match = re.fullmatch(r'unix:path=(.+),guid=([0-9a-f]{32})', address)
        if not match or match.group(1) != str(socket_path):
            raise ServiceError('private bus did not report its exact private socket address')
        socket_info = os.lstat(socket_path)
        if not stat.S_ISSOCK(socket_info.st_mode) or socket_info.st_uid != os.geteuid():
            raise ServiceError('private bus socket is not an owned Unix socket')
        os.environ['DBUS_SESSION_BUS_ADDRESS'] = address
        env = dict(os.environ)

        def call(method, signature_check):
            result = subprocess.run([identities['dbus-send']['path'], '--session', '--print-reply',
                                     '--dest=org.freedesktop.DBus', '/org/freedesktop/DBus',
                                     'org.freedesktop.DBus.' + method], capture_output=True, timeout=15, env=env)
            if result.returncode or signature_check not in result.stdout:
                raise ServiceError('private bus did not answer its real ' + method + ' call')
            return result.stdout.decode(errors='replace')
        # The calls use the full printed address including guid=: libdbus
        # refuses a server whose AUTH guid differs, so these replies come from
        # exactly the daemon that reported this address on our private pipe.
        # GetId is the bus UUID (distinct from the listening server's guid).
        call('ListNames', b'org.freedesktop.DBus')
        bus_id = re.search(r'string "([0-9a-f]{32})"', call('GetId', b'string "'))
        if not bus_id:
            raise ServiceError('private bus returned no bus identity')
        state.update(bus_address=address, server_guid=match.group(2), bus_id=bus_id.group(1),
                     socket={'path': str(socket_path), 'dev': socket_info.st_dev, 'ino': socket_info.st_ino,
                             'mode': oct(stat.S_IMODE(socket_info.st_mode))},
                     private_root=str(private), bus_readiness={'methods': ['ListNames', 'GetId']}, ready=True)
        os.environ['NOSTRC_NATIVE_RESOURCE_ROOT'] = str(root)
        yield state
        exited = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        if exited:
            raise ServiceError('private bus exited during the lane')
        state['status'] = 'passed'
    except BaseException as exc:
        original = exc
        state['error'] = type(exc).__name__ + ': ' + str(exc)
        raise
    finally:
        handlers = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGTERM, signal.SIGINT)}
        errors = []
        try:
            if process is not None:
                record = state['services'][-1]
                try:
                    process.send_signal(signal.SIGTERM)  # Popen refuses once reaped
                    try:
                        record['exit_code'] = process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()  # still unreaped: identity reserved
                        record['forced_exit'] = True
                        record['exit_code'] = process.wait(timeout=15)
                    record['reaped'] = True
                except Exception as exc:
                    errors.append(type(exc).__name__ + ': ' + str(exc))
            if log is not None:
                log.close()
            os.environ.clear()
            os.environ.update(previous)
            if errors:
                state.update(status='failed', cleanup_errors=errors)
            with (evidence / 'services.json').open('x') as stream:
                json.dump(state, stream, sort_keys=True, indent=2)
                stream.write('\n')
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
        if errors and original is None:
            raise ServiceError('private bus cleanup unverified: ' + '; '.join(errors))


def release_resource_directory(lane_dir):
    """Outer Darwin cleanup of the journal-proven owned directory and its contents.

    Call only after a validated custody exit (clean, or parent-bound recovery
    proved no member remains). Removal is symlink-attack resistant and never
    leaves the directory this journal proved it created (dev/ino/owner/mode).
    """
    journal = Path(lane_dir) / JOURNAL_NAME
    records = read_resource_journal(journal)
    if records[-1]['state'] in ('released', 'absent'):
        return Path(records[-1]['path'])
    if records[-1]['state'] != 'allocated' or len(records) != 2:
        raise ServiceError('resource journal does not authorize recursive release: ' + records[-1]['state'])
    allocation = _journal_allocation(records)
    path = Path(allocation['path'])
    info = os.lstat(path)
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700
            or (info.st_dev, info.st_ino) != (allocation['dev'], allocation['ino'])):
        raise ServiceError('private resource directory changed identity before release')
    if not shutil.rmtree.avoids_symlink_attacks:
        raise ServiceError('platform rmtree is not symlink-attack resistant; refusing recursive release')
    shutil.rmtree(path)
    if path.exists():
        raise ServiceError('private resource directory remained after release')
    _journal_append(journal, {'state': 'released', 'path': str(path), 'absent_after': True, 'recursive': True})
    return path


def retain_resource_directory(lane_dir, reason):
    """Record that the owned directory is intentionally retained (custody unverified)."""
    journal = Path(lane_dir) / JOURNAL_NAME
    records = read_resource_journal(journal)
    if records[-1]['state'] == 'allocated':
        _journal_append(journal, {'state': 'retained', 'path': records[-1]['path'], 'reason': reason})


SIGNER_CONSENT = 'unidentified-get-public-key-once'
SIGNER_INPUTS = ('tests/runtime/test_provisioned_nip5f.py', 'tests/runtime/nip5f_approver.c',
                 'gnome/groundhog/tests/app/test_secret_service.py',
                 'apps/grotto/data/org.nostr.Grotto.gschema.xml')
SIGNER_BUILD_INPUTS = ('apps/grotto/grotto-daemon', 'nips/nip5f/test_nip5f_tcp',
                       'nips/nip5f/test_nip5f_approver', 'apps/grotto/gschemas.compiled')
SIGNER_EXECUTABLES = SIGNER_BUILD_INPUTS[:3]
# Authored instrumentation -> exact realized cache entries. GNOSTR_ENABLE_ASAN
# forces GO_ENABLE_ASAN, whose Debug flags instrument every executable.
SIGNER_INSTRUMENTATION = {'none': {'GO_ENABLE_ASAN': None},
                          'asan_ubsan': {'GO_ENABLE_ASAN': 'GO_ENABLE_ASAN:BOOL=ON',
                                         'CMAKE_BUILD_TYPE': 'CMAKE_BUILD_TYPE:STRING=Debug'}}
SANITIZER_ENVIRONMENT = ('ASAN_OPTIONS', 'UBSAN_OPTIONS', 'LSAN_OPTIONS', 'G_SLICE')


def elf_symbol_names(data, label='signer executable'):
    """Names from ELF64 bytes' .symtab/.dynsym; no external tool or byte grep."""
    if data[:4] != b'\x7fELF' or len(data) < 64 or data[4] != 2 or data[5] != 1:
        raise ServiceError(label + ' is not a little-endian ELF64 file')
    try:
        shoff, = struct.unpack_from('<Q', data, 0x28)
        shentsize, shnum = struct.unpack_from('<HH', data, 0x3a)
        sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
        names = set()
        for _, kind, _, _, offset, size, link, _, _, entsize in sections:
            if kind not in (2, 11) or not entsize:
                continue  # SHT_SYMTAB, SHT_DYNSYM
            strings = sections[link]
            base, limit = strings[4], strings[4] + strings[5]
            for index in range(size // entsize):
                name, = struct.unpack_from('<I', data, offset + index * entsize)
                end = data.index(b'\0', base + name, limit)
                names.add(data[base + name:end].decode('ascii', 'replace'))
    except (struct.error, IndexError, ValueError) as exc:
        raise ServiceError(label + ' has a malformed ELF symbol table') from exc
    return names


def signer_instrumentation_evidence(build, instrumentation, audited=None):
    """Physical post-build proof that fixture executables match authored instrumentation.

    ASan is required exactly as authored. UBSan is recorded as observed, not
    required: the authored asan_ubsan profiles configure ASan only.
    """
    evidence = {}
    for relative in SIGNER_EXECUTABLES:
        path = Path(build) / relative
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if audited is not None and audited.get(str(path)) != digest:
            raise ServiceError('signer executable differs from its full-audit binding: ' + relative)
        names = elf_symbol_names(data, relative)
        asan = '__asan_init' in names
        if asan != (instrumentation == 'asan_ubsan'):
            raise ServiceError(f'signer executable instrumentation differs from authored {instrumentation}: {relative}')
        evidence[str(path)] = {'sha256': digest, 'asan': asan,
                               'ubsan_observed': any(n.startswith('__ubsan_handle_') for n in names)}
    return evidence


def signer_contract(realization, environment=None):
    """Authored provisioning contract only: never readiness or passing coverage."""
    profile = realization['profile']
    if (sys.platform != 'linux' or profile.get('platform') != 'linux' or
            profile.get('architecture') != os.uname().machine):
        raise ServiceError('signer fixture requires exact native Linux profile architecture')
    if profile.get('environment', {}).get('NOSTRC_TEST_NIP5F_APPROVAL') != SIGNER_CONSENT:
        raise ServiceError('signer fixture requires authored one-shot unidentified get_public_key consent')
    expected = {'ENABLE_TCP_IPC': ('BOOL', 'ON'), 'NIP55L_TEST_TRUST_ENV': ('BOOL', 'ON'),
                'NIP55L_SECRET_BACKEND': ('STRING', 'libsecret')}
    args = profile.get('configure_args', [])
    for name, (kind, value) in expected.items():
        supplied = [arg for arg in args if arg.startswith('-D' + name + '=') or
                    arg.startswith('-D' + name + ':')]
        if len(supplied) != 1 or supplied[0] not in ('-D' + name + '=' + value,
                                                   '-D' + name + ':' + kind + '=' + value):
            raise ServiceError('signer fixture requires explicit authored configuration: ' + name)
    root, build = Path(realization['repo_root']), Path(realization['build_dir'])
    cache_path = build / 'CMakeCache.txt'
    cache = cache_path.read_text().splitlines()
    for name, (kind, value) in expected.items():
        if [line for line in cache if line.startswith(name + ':')] != [name + ':' + kind + '=' + value]:
            raise ServiceError('signer fixture realized configuration mismatch: ' + name)
    instrumentation = profile.get('instrumentation', 'none')
    if instrumentation not in SIGNER_INSTRUMENTATION:
        raise ServiceError('signer fixture has no runtime contract for instrumentation: ' + str(instrumentation))
    if instrumentation != 'none':
        authored = {'-DCMAKE_BUILD_TYPE=Debug'} <= set(args) and bool(
            {'-DGO_ENABLE_ASAN=ON', '-DGNOSTR_ENABLE_ASAN=ON'} & set(args))
        if not authored:
            raise ServiceError('signer fixture instrumentation lacks authored Debug ASan configuration')
    for name, line in SIGNER_INSTRUMENTATION[instrumentation].items():
        realized = [row for row in cache if row.startswith(name + ':')]
        if (realized != [line]) if line else any(row.endswith('=ON') for row in realized):
            raise ServiceError('signer fixture realized instrumentation mismatch: ' + name)
    if 'CMAKE_HOME_DIRECTORY:INTERNAL=' + str(root) not in cache:
        raise ServiceError('signer fixture realized source root mismatch')
    inputs = {}
    for relative in SIGNER_INPUTS:
        path = root / relative
        if path.is_symlink() or path.resolve(strict=True) != path:
            raise ServiceError('signer fixture source input must be canonical')
        inputs[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    if environment is None:
        from test_selection_realization import phase_environment
        environment = phase_environment(profile, str(root), 'test')
    if environment.get('NOSTRC_TEST_NIP5F_APPROVAL') != SIGNER_CONSENT:
        raise ServiceError('signer fixture test-phase consent differs from authored one-shot approval')
    tools = {name: _identity(name, environment.get('PATH')) for name in ('dbus-daemon', 'gdbus')}
    tools['python'] = _identity(sys.executable)
    from test_selection_realization import phase_environment
    # Authored common + test-phase overlay, source-expanded, without ambient values.
    overlay = phase_environment(profile, str(root), 'test', base_environment={})
    authored_env = {key: value for key, value in overlay.items() if key in SANITIZER_ENVIRONMENT}
    # The whole effective projection: an inherited unauthored LSAN_OPTIONS
    # (e.g. a suppression file) must not reach fixture processes.
    if {key: environment[key] for key in SANITIZER_ENVIRONMENT if key in environment} != authored_env:
        raise ServiceError('signer fixture test-phase sanitizer environment differs from authored profile')
    return {'kind': 'nip5f_one_shot_provision_contract_v1', 'readiness': False,
            'executables': tools, 'instrumentation': instrumentation,
            'sanitizer_environment': authored_env,
            'source_identity': realization['source_identity'], 'consent': SIGNER_CONSENT,
            'operation': 'get_public_key', 'principal': 'unidentified', 'remember': False,
            'inputs': inputs, 'cache_sha256': hashlib.sha256(cache_path.read_bytes()).hexdigest(),
            'complete': False, 'complete_gate_eligible': False}


@contextlib.contextmanager
def provisioned_signer(realization, resolved, input_audit, evidence_dir, *, environment=None):
    """Live, audited test_nip5f_tcp resource; never share its HOME/bus with CTest.

    The outer PID namespace owns crash/cancellation descendants. Direct fixture
    children are joined on normal/nonzero/exception exits; post-exit namespace
    evidence remains the outer adapter's responsibility.
    """
    contract = signer_contract(realization, environment)
    if resolved.get('runtime_capability_evidence', {}).get('signer_endpoint') != contract:
        raise ServiceError('signer fixture resolved provisioning contract mismatch')
    if not input_audit.get('passed') or not input_audit.get('after_build'):
        raise ServiceError('signer fixture requires passing complete post-build input audit')
    consumers = {name for name in resolved['tests'] if 'signer_endpoint' in
                 next((d.get('requires', []) for d in realization['descriptors'] if d['name'] == name), [])}
    if consumers != {'test_nip5f_tcp'} or any('signer_endpoint' in spec['requires']
                                            for spec in resolved.get('command_executions', {}).values()):
        raise ServiceError('signer fixture supports only the exact test_nip5f_tcp consumer')
    root, build = Path(realization['repo_root']), Path(realization['build_dir'])
    paths = [root / name for name in SIGNER_INPUTS] + [build / name for name in SIGNER_BUILD_INPUTS]
    for path in paths:
        if path.is_symlink() or path.resolve(strict=True) != path or not path.is_file():
            raise ServiceError('signer fixture input absent or indirect')
        if input_audit.get('input_hashes', {}).get(str(path)) != hashlib.sha256(path.read_bytes()).hexdigest():
            raise ServiceError('signer fixture input lacks exact full-audit binding: ' + str(path))
    instrumented = signer_instrumentation_evidence(build, contract['instrumentation'],
                                                   input_audit.get('input_hashes', {}))
    resource_root = _namespace_resources(os.environ.get('NOSTRC_NATIVE_RESOURCE_ROOT', ''), False)
    require_socket_budget(resource_root)
    import runpy
    fixture = runpy.run_path(str(root / SIGNER_INPUTS[0]))
    config = fixture['prepare_fixture'](root, build, evidence_dir, consent=True,
                                         private_root=resource_root, prebuilt=True, environment=environment,
                                         runtime_tools=contract['executables'])
    owned = fixture['Signer']('worker', config=config)
    state = {'kind': 'nip5f_one_shot_runtime_v1', 'contract': contract, 'inputs': config.inputs,
             'executables': config.producer['executables'], 'status': 'failed',
             'instrumented_executables': instrumented,
             'complete': False, 'complete_gate_eligible': False}
    try:
        owned.start()
        state['readiness'] = dict(owned.result)
        # TCP clients need ONLY these values, never fixture HOME/bus/service.
        yield {key: owned.env[key] for key in ('NOSTR_SIGNER_ENDPOINT', 'NOSTR_SIGNER_TOKEN')}, state
        snapshot = owned.snapshot()
        if snapshot != '(uint32 1, uint32 1, uint32 0)':
            raise ServiceError('signer fixture did not observe exactly one completed explicit consent')
        state['approval_snapshot'] = snapshot
        state['status'] = 'passed'
    except BaseException as exc:
        state['error_type'] = type(exc).__name__
        raise
    finally:
        try:
            owned.finish()
            if any(fixture['identity'](value['path']) != value for value in config.inputs.values()):
                raise ServiceError('signer fixture input changed during execution')
            if signer_contract(realization, environment) != contract:
                raise ServiceError('signer fixture source/configuration changed during execution')
        except BaseException:
            state['status'] = 'failed'
            raise
        finally:
            state['cleanup'] = owned.result
            with (Path(evidence_dir) / 'signer-runtime.json').open('x') as stream:
                json.dump(state, stream, sort_keys=True, indent=2)
                stream.write('\n')


@contextlib.contextmanager
def private_services(capabilities, evidence_dir, resource_root=None, finalizer_fd=None):
    """Called in-process by the pidns backend, spanning resolve and run."""
    caps = set(capabilities)
    if caps - {'private_bus', 'display'}:
        raise ServiceError('unsupported private resource setup: ' + repr(sorted(caps)))
    if not hasattr(os, 'pidfd_open') or not hasattr(signal, 'pidfd_send_signal'):
        raise ServiceError('private services require Linux pidfd ownership')
    evidence = Path(evidence_dir)
    evidence.mkdir(parents=True, exist_ok=True)
    identities = {name: _identity(name) for name in
                  (['dbus-daemon', 'dbus-send'] if 'private_bus' in caps else []) +
                  (['Xvfb', 'xauth'] if 'display' in caps else [])}
    state = {'schema_version': 1, 'capabilities': sorted(caps), 'executables': identities,
             'services': [], 'status': 'failed', 'complete': False,
             'cleanup_scope': 'reserved direct services; outer namespace owns descendants'}
    children = []
    original = None
    previous = dict(os.environ)
    private = tempfile.TemporaryDirectory(prefix='nostrc-native-services-', dir=resource_root)
    root = Path(private.name)

    def xauth(authority, display, cookie):
        result = subprocess.run([identities['xauth']['path'], '-f', str(authority),
                                 'add', display, '.', cookie], capture_output=True)
        if result.returncode:
            raise ServiceError('private display authorization setup failed')

    def launch(name, argv, ready=False):
        log = (evidence / (name + '.log')).open('xb')
        read_fd, write_fd = os.pipe() if ready else (-1, -1)
        process = handle = None
        mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
        try:
            command = [identities[name]['path'], *argv]
            if ready:
                command = [part.replace('{ready_fd}', str(write_fd)) for part in command]
            def child_setup():
                # preexec_fn can run before close_fds. Close the protected pipe
                # explicitly before permitting this child's UID-map setup.
                if finalizer_fd is not None:
                    try:
                        os.close(finalizer_fd)
                    except OSError as exc:
                        if exc.errno != errno.EBADF:
                            raise
                signal.pthread_sigmask(signal.SIG_SETMASK, mask)
                if name == 'Xvfb' and os.geteuid() != 0:
                    # Xserver ignores -nolock for a nonzero namespace UID.
                    # Map only this existing UID/GID to child-namespace 0;
                    # never gain host root or change backend/worker identity.
                    uid, gid = os.geteuid(), os.getegid()
                    libc = ctypes.CDLL(None, use_errno=True)
                    if libc.prctl(4, 1, 0, 0, 0) != 0:
                        raise ServiceError('private display child mapping ownership unavailable')
                    if libc.unshare(0x10000000) == -1:
                        raise ServiceError('private display user namespace unavailable')
                    Path('/proc/self/setgroups').write_text('deny')
                    Path('/proc/self/uid_map').write_text(f'0 {uid} 1\n')
                    Path('/proc/self/gid_map').write_text(f'0 {gid} 1\n')
                    if os.geteuid() != 0 or os.getegid() != 0:
                        raise ServiceError('private display namespace identity mismatch')
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                pass_fds=(write_fd,) if ready else (), restore_signals=True,
                start_new_session=True, preexec_fn=child_setup)
            record = {'executable': name, 'argv': command, 'log': str(log.name)}
            if name == 'Xvfb':
                record['namespace_identity'] = ('caller-only nested user namespace UID/GID 0; no host root'
                                                if os.geteuid() != 0 else 'existing namespace UID/GID 0')
            children.append((process, None, log, record))
            state['services'].append(record)
            handle = os.pidfd_open(process.pid)
            children[-1] = (process, handle, log, record)
            signal.pidfd_send_signal(handle, 0)
        finally:
            if write_fd != -1:
                os.close(write_fd)
            signal.pthread_sigmask(signal.SIG_SETMASK, mask)
            # Reservation failure leaves the unreaped direct child owned by
            # Popen, but never authorizes a numeric signal fallback.
            if handle is None and read_fd != -1:
                os.close(read_fd)
        try:
            value = _ready(read_fd) if ready else None
            return process, value
        finally:
            if read_fd != -1:
                os.close(read_fd)

    try:
        for name, path in {'HOME': root / 'home', 'XDG_RUNTIME_DIR': root / 'runtime',
                           'XDG_CONFIG_HOME': root / 'config', 'XDG_DATA_HOME': root / 'data',
                           'XDG_CACHE_HOME': root / 'cache'}.items():
            path.mkdir(mode=0o700)
            os.environ[name] = str(path)
        for name in ('DBUS_SESSION_BUS_ADDRESS', 'DBUS_SESSION_BUS_PID', 'DISPLAY',
                     'XAUTHORITY', 'GNOME_KEYRING_CONTROL'):
            os.environ.pop(name, None)
        if 'private_bus' in caps:
            # An abstract Unix socket is kernel-owned even if SIGKILL prevents
            # daemon cleanup; never leave a session socket in shared host /tmp.
            _, address = launch('dbus-daemon', ['--session', '--nofork', '--nopidfile',
                                             '--address=unix:abstract=nostrc-private-' + secrets.token_hex(16),
                                             '--print-address={ready_fd}'], ready=True)
            if not address.startswith('unix:'):
                raise ServiceError('private bus did not report a Unix address')
            os.environ['DBUS_SESSION_BUS_ADDRESS'] = address
            probe = subprocess.run([identities['dbus-send']['path'], '--session', '--print-reply',
                '--dest=org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus.ListNames'],
                capture_output=True, timeout=15)
            if probe.returncode or b'org.freedesktop.DBus' not in probe.stdout:
                raise ServiceError('private bus did not answer its real readiness call')
            state['bus_readiness'] = {'method': 'org.freedesktop.DBus.ListNames', 'socket': 'kernel_abstract'}
        if 'display' in caps:
            lock_before = {path.name: (path.stat().st_ino, path.stat().st_mtime_ns, path.stat().st_size)
                           for path in Path('/tmp').glob('.X*-lock')}
            authority = root / 'Xauthority'
            authority.touch(mode=0o600)
            cookie = secrets.token_hex(16)
            # Xserver loads the cookie records from its auth file. Add the
            # assigned display's client record after the displayfd handshake.
            xauth(authority, ':0', cookie)
            display_process, number = launch('Xvfb', ['-displayfd', '{ready_fd}', '-screen', '0',
                                      '1280x800x24', '-nolisten', 'tcp', '-nolock', '-auth', str(authority)], ready=True)
            if not number.isdecimal():
                raise ServiceError('private display reported an invalid display number')
            shared_lock = Path('/tmp/.X' + number + '-lock')
            if shared_lock.exists() and lock_before.get(shared_lock.name) != (shared_lock.stat().st_ino, shared_lock.stat().st_mtime_ns, shared_lock.stat().st_size):
                raise ServiceError('private display created/changed a shared host X lock')
            state['display_identity'] = {
                'uid_map': Path(f'/proc/{display_process.pid}/uid_map').read_text().strip(),
                'gid_map': Path(f'/proc/{display_process.pid}/gid_map').read_text().strip(),
                'shared_lock_created': False}
            xauth(authority, ':' + number, cookie)
            os.environ.update(DISPLAY=':' + number, XAUTHORITY=str(authority))
            _display_ready(number, cookie)
        state['ready'] = True
        yield state
        for process, _, _, record in children:
            # Observe without reaping: keep the PID identity reserved until
            # pidfd teardown, including a service that exited unexpectedly.
            exited = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if exited:
                raise ServiceError('private service exited during lane: ' + record['executable'])
        state['status'] = 'passed'
    except BaseException as exc:
        original = exc
        state['error'] = type(exc).__name__ + ': ' + str(exc)
        raise
    finally:
        handlers = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGTERM, signal.SIGINT)}
        errors = []
        try:
            for process, handle, log, record in reversed(children):
                try:
                    if handle is None:
                        raise ServiceError('service pidfd reservation failed; namespace teardown required')
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    try:
                        record['exit_code'] = process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        # The still-unreaped child retains this pidfd identity.
                        try:
                            signal.pidfd_send_signal(handle, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        record['forced_exit'] = True
                        record['exit_code'] = process.wait(timeout=15)
                    record['reaped'] = True
                except Exception as exc:
                    errors.append(type(exc).__name__ + ': ' + str(exc))
                finally:
                    if handle is not None:
                        os.close(handle)
                    log.close()
            os.environ.clear()
            os.environ.update(previous)
            try:
                private.cleanup()
            except Exception as exc:
                errors.append(type(exc).__name__ + ': ' + str(exc))
            state['private_home_removed'] = not root.exists()
            if errors:
                state.update(status='failed', cleanup_errors=errors)
            destination = evidence / 'services.json'
            with destination.open('x') as stream:
                json.dump(state, stream, sort_keys=True, indent=2)
                stream.write('\n')
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
        if errors and original is None:
            raise ServiceError('private service cleanup unverified: ' + '; '.join(errors))


NATIVE_ROOTS = ('build', 'evidence', 'services', 'source-inputs')
NATIVE_FILES = ('receipt.json', 'resolved.json', 'resolve.log', 'run.log', 'lane.log')


def _native_files(work):
    result = {}
    for name in NATIVE_FILES:
        path = work / name
        if path.exists():
            if path.is_symlink() or not path.is_file():
                raise ServiceError('unsafe native evidence file: ' + name)
            result[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def _source_claims(result, source):
    claims = {}
    def visit(value):
        if isinstance(value, dict):
            for path, expected in value.get('input_hashes', {}).items():
                candidate = Path(path)
                if candidate.is_absolute() and candidate.is_relative_to(source):
                    if candidate.resolve(strict=True).is_relative_to(source):
                        if path in claims and claims[path] != expected:
                            raise ServiceError('contradictory native source input claim')
                        claims[path] = expected
                    else:
                        raise ServiceError('native source input escaped checkout')
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)
    visit(result)
    return claims


def seal_native_lane(work, source, result, resolved):
    """Trusted root seals original in-memory CLI claims before exiting."""
    from test_selection_custody import retained_files, attest_retained_evidence, digest
    work, source = Path(work).resolve(strict=True), Path(source).resolve(strict=True)
    output = work / 'receipt.json'
    if output.is_symlink() or json.loads(output.read_text()) != result:
        raise ServiceError('native output differs from original in-memory receipt')
    resolution = work / 'resolved.json'
    if resolution.is_symlink() or json.loads(resolution.read_text()) != resolved:
        raise ServiceError('native resolution differs from original in-memory contract')
    source_claims = _source_claims(result, source)
    retained_source = work / 'source-inputs'
    retained_source.mkdir(mode=0o700)
    for name, expected in source_claims.items():
        raw = Path(name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ServiceError('native source input changed before finalizer seal')
        destination = retained_source / (hashlib.sha256(name.encode()).hexdigest() + '.input')
        with destination.open('xb') as stream:
            stream.write(raw)
    snapshot = retained_files(work, NATIVE_ROOTS)
    seal = {'schema_version': 1, 'retained_evidence': snapshot, 'retained_evidence_hash': digest(snapshot),
            'files': _native_files(work), 'source_claims': source_claims}
    verification = attest_retained_evidence(work, result, seal, roots=NATIVE_ROOTS, claim_root=work)
    if verification['status'] != 'verified':
        raise ServiceError('native original receipt hashes differ from pre-exit seal')
    path = work / 'native-seal.json'
    raw = (json.dumps(seal, sort_keys=True, indent=2) + '\n').encode()
    with path.open('xb') as stream:
        stream.write(raw)
    return hashlib.sha256(raw).hexdigest()


def attest_native_lane(work, expected, *, require_source=True):
    """Outer-only check after helper wait/namespace exit, anchored by private FD."""
    from test_selection_custody import attest_retained_evidence
    work = Path(work).resolve(strict=True)
    path = work / 'native-seal.json'
    if len(expected) != 64 or any(c not in '0123456789abcdef' for c in expected):
        raise ServiceError('trusted native finalizer seal missing')
    if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise ServiceError('native finalizer seal bytes changed')
    seal = json.loads(path.read_text())
    if _native_files(work) != seal['files']:
        raise ServiceError('native top-level retained evidence changed after seal')
    result = json.loads((work / 'receipt.json').read_text())
    verification = attest_retained_evidence(work, result, seal, roots=NATIVE_ROOTS, claim_root=work)
    for name, value in seal['source_claims'].items():
        retained = work / 'source-inputs' / (hashlib.sha256(name.encode()).hexdigest() + '.input')
        if retained.is_symlink() or not retained.is_file() or hashlib.sha256(retained.read_bytes()).hexdigest() != value:
            raise ServiceError('native retained source input changed after finalizer seal')
        if require_source:
            candidate = Path(name)
            if not candidate.is_file() or hashlib.sha256(candidate.read_bytes()).hexdigest() != value:
                raise ServiceError('native source input changed after finalizer seal')
    if verification['status'] != 'verified':
        raise ServiceError('native retained evidence changed after finalizer seal')
    return {'schema_version': 1, 'status': 'verified', 'seal_sha256': expected,
            'retained_evidence': verification, 'complete': False, 'complete_gate_eligible': False}


def native_public_receipt(work):
    work = Path(work).resolve(strict=True)
    final_path = work / 'native-final.json'
    if final_path.is_symlink() or not final_path.is_file():
        raise ServiceError('unsafe native final evidence file')
    final = json.loads(final_path.read_text())
    if final.get('status') != 'verified':
        raise ServiceError('native final evidence did not verify')
    result = json.loads((work / 'receipt.json').read_text())
    result['native_custody_final'] = {'kind': 'native_kernel_custody_v1', 'evidence_root': str(work),
                                      'seal_sha256': final['seal_sha256']}
    return result


def verify_native_public_receipt(receipt):
    final = receipt.get('native_custody_final')
    if not isinstance(final, dict) or set(final) != {'kind', 'evidence_root', 'seal_sha256'} or final.get('kind') != 'native_kernel_custody_v1':
        raise ServiceError('native kernel custody final seal missing or malformed')
    work = Path(final['evidence_root'])
    if not work.is_absolute() or work.resolve(strict=True) != work:
        raise ServiceError('native evidence root is not canonical')
    original = dict(receipt)
    del original['native_custody_final']
    backend = original.get('backend_receipt', original)
    if Path(backend.get('build_dir', '')).parent != work or json.loads((work / 'receipt.json').read_text()) != original:
        raise ServiceError('native public receipt differs from original execution binding')
    # The outer adapter checked live source before deleting its checkout. Later
    # aggregation verifies the anchored exact retained input bytes instead.
    observed = attest_native_lane(work, final['seal_sha256'], require_source=False)
    final_path = work / 'native-final.json'
    if final_path.is_symlink() or not final_path.is_file():
        raise ServiceError('unsafe native final evidence file')
    if json.loads(final_path.read_text()) != observed:
        raise ServiceError('native final verification differs from retained evidence')
