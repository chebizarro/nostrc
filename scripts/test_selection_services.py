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
from pathlib import Path
import secrets
import select
import shutil
import signal
import socket
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


def _identity(name):
    executable = shutil.which(name)
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
        state['resource_mount'] = {'root': str(root), 'filesystem': 'tmpfs',
                                   'private_x11': 'display' in capabilities}
        yield state


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
