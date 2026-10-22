#!/usr/bin/env python3
"""Explicit Linux user/mount/PID-namespace custody, without ptrace.

The supervisor stays outside its new PID namespace. Its direct, unreaped init
child is held by pidfd; init exit is the kernel's all-descendant kill/reap
barrier (kernel/pid_namespace.c:zap_pid_ns_processes). setsid, double fork,
CLONE_UNTRACED and nested PID namespaces do not escape that boundary.

Not profile parity or a filesystem/network sandbox: UID/GID maps, procfs and
no_new_privs differ from an ordinary launch. External daemon work is not owned.
No privilege escalation, namespace-policy bypass, Docker or numeric-PID signal
fallback. Unsupported setup refuses before executing the requested program.
"""
import ctypes
import os
from pathlib import Path
import select
import signal
import sys
import time

from adapter_custody import CustodyError, GRACE_SECONDS

NEWUSER, NEWNS, NEWPID = 0x10000000, 0x20000, 0x20000000
SIGNALS = {signal.SIGCHLD, signal.SIGTERM, signal.SIGINT}


def checked(result, operation):
    if result == -1:
        error = ctypes.get_errno()
        raise OSError(error, operation + ': ' + os.strerror(error))


def send(handle, sig):
    try:
        signal.pidfd_send_signal(handle, sig)
    except ProcessLookupError:
        pass


def arm_parent_death(libc, handle):
    # The inherited pidfd covers death before PR_SET_PDEATHSIG, including the
    # namespace init whose parent is invisible (getppid() == 0) inside its NS.
    checked(libc.prctl(1, signal.SIGKILL, 0, 0, 0), 'parent-death signal')
    if select.select([handle], [], [], 0)[0]:
        raise CustodyError('custody parent exited; target not launched')


def init_namespace(libc, parent_handle, gate, argv, old_mask, resource_root=None, private_display=False):
    arm_parent_death(libc, parent_handle)
    os.close(parent_handle)
    if os.read(gate, 1) != b'1':
        return 125
    os.close(gate)
    if os.getpid() != 1:
        raise CustodyError('not a fresh PID namespace init; target not launched')
    # Private propagation was established before fork, in the new mount NS.
    checked(libc.mount(b'proc', b'/proc', b'proc', 14, None), 'private procfs mount')
    if int(next(row.split()[1] for row in Path('/proc/self/status').read_text().splitlines()
                if row.startswith('Tgid:'))) != 1:
        raise CustodyError('procfs namespace mismatch; target not launched')
    if resource_root is not None:
        from test_selection_services import prepare_resources
        prepare_resources(resource_root, private_display)
    # Retain the actual procfs directory, not a re-resolved mutable /proc path.
    proc = os.open('/proc', os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    read_fd, write_fd = os.pipe()
    root = os.fork()
    if root == 0:
        os.close(write_fd)
        os.close(proc)
        try:
            if os.read(read_fd, 1) != b'1':
                os._exit(125)
            os.close(read_fd)
            os.setsid()
            checked(libc.prctl(38, 1, 0, 0, 0), 'no_new_privs')
            for sig in SIGNALS | {signal.SIGPIPE}:
                signal.signal(sig, signal.SIG_DFL)
            signal.pthread_sigmask(signal.SIG_SETMASK, old_mask - SIGNALS)
            os.execvpe(argv[0], argv, os.environ)
        except BaseException as exc:
            os.write(2, ('namespace custody exec: ' + str(exc) + '\n').encode())
            os._exit(125)
    os.close(read_fd)
    seal_copy = os.environ.pop('NOSTRC_NATIVE_SEAL_FD', None)
    if seal_copy is not None:
        os.close(int(seal_copy))
    handle = None
    cancelled, deadline = False, None
    try:
        # root cannot be reused: this is our direct, unreaped child.
        handle = os.pidfd_open(root)
        signal.pidfd_send_signal(handle, 0)
        if signal.sigpending() & {signal.SIGTERM, signal.SIGINT}:
            return 130  # init exit kills the still-gated child
        os.write(write_fd, b'1')
        os.close(write_fd)
        write_fd = -1
        while True:
            # PID 1 also adopts double-fork orphans. Reap them throughout the
            # run rather than accumulating zombies until final namespace exit.
            child = 0
            for _ in range(256):
                child, status = os.waitpid(-1, os.WNOHANG)
                if not child:
                    break
                if child == root:
                    code = os.waitstatus_to_exitcode(status)
                    return 130 if cancelled else (128 - code if code < 0 else code)
            timeout = max(0, deadline - time.monotonic()) if deadline else None
            if child:
                timeout = 0
            notice = signal.sigtimedwait(SIGNALS, timeout) if timeout is not None else signal.sigwaitinfo(SIGNALS)
            if notice and notice.si_signo in (signal.SIGTERM, signal.SIGINT) and not cancelled:
                cancelled = True
                deadline = time.monotonic() + GRACE_SECONDS
                send(handle, signal.SIGTERM)
                # Every PID visible through this fresh procfs belongs to this
                # ownership domain, including reused PIDs and nested-NS tasks.
                # Bind before signaling; no ps ancestry or host PID is trusted.
                # root stays unreaped/reserved and alive to finalize raw logs.
                for entry in os.listdir(proc):
                    if not entry.isdecimal() or int(entry) in (1, root):
                        continue
                    try:
                        descendant = os.pidfd_open(int(entry))
                    except ProcessLookupError:
                        continue
                    try:
                        send(descendant, signal.SIGKILL)
                    finally:
                        os.close(descendant)
            if deadline and time.monotonic() >= deadline:
                # Exiting init invokes the kernel barrier even for concurrently
                # created, escaped, stopped or nested descendants. No fake drain.
                return 130
    finally:
        if write_fd != -1:
            os.close(write_fd)
        if handle is not None:
            os.close(handle)
        os.close(proc)


def supervise(argv, adapter_parent=None, resource_root=None, private_display=False):
    if sys.platform != 'linux':
        raise CustodyError('native kernel custody unavailable on macOS: EndpointSecurity descendant client requires '
                           'com.apple.developer.endpoint-security.client (not provisioned); '
                           'kqueue NOTE_TRACK is ENOTSUP; target not launched')
    if not all((hasattr(os, 'pidfd_open'), hasattr(signal, 'pidfd_send_signal'))):
        raise CustodyError('namespace pidfd custody unavailable; target not launched')
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mount.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_ulong, ctypes.c_void_p]
    libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
    old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, SIGNALS)
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    # PID 1 only accepts signals for which it has installed a handler, even when
    # consumed synchronously. The handlers remain blocked in both supervisors.
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: None)
    if signal.sigpending() & {signal.SIGTERM, signal.SIGINT}:
        return 130
    uid, gid = os.geteuid(), os.getegid()
    parent_handle = os.pidfd_open(os.getpid())
    child_handle = None
    read_fd, write_fd = os.pipe()
    child = None
    try:
        # Never acquire host CAP_SYS_ADMIN: only capabilities in a fresh user
        # namespace, with exactly the caller's own UID/GID mapped unchanged.
        checked(libc.unshare(NEWUSER), 'unshare user namespace (policy/privilege required)')
        Path('/proc/self/setgroups').write_text('deny')
        Path('/proc/self/uid_map').write_text(f'{uid} {uid} 1\n')
        Path('/proc/self/gid_map').write_text(f'{gid} {gid} 1\n')
        # User-namespace credential transitions can clear PDEATHSIG. Re-arm
        # using the retained adapter identity before authorizing any target.
        if adapter_parent is not None:
            arm_parent_death(libc, adapter_parent)
        checked(libc.unshare(NEWNS | NEWPID), 'unshare mount/PID namespaces')
        checked(libc.mount(None, b'/', None, (1 << 18) | (1 << 14), None), 'private mount propagation')
        child = os.fork()
        if child == 0:
            os.close(write_fd)
            if adapter_parent is not None:
                os.close(adapter_parent)
            try:
                code = init_namespace(libc, parent_handle, read_fd, argv, old_mask, resource_root, private_display)
            except BaseException as exc:
                os.write(2, ('namespace custody setup/cleanup failed: ' + str(exc) + '\n').encode())
                code = 125
            # Do not unwind into outer-supervisor cleanup in the forked init.
            os._exit(code)
        os.close(read_fd)
        read_fd = -1
        seal_copy = os.environ.pop('NOSTRC_NATIVE_SEAL_FD', None)
        if seal_copy is not None:
            os.close(int(seal_copy))
        child_handle = os.pidfd_open(child)
        signal.pidfd_send_signal(child_handle, 0)
        cancelled = bool(signal.sigpending() & {signal.SIGTERM, signal.SIGINT})
        if not cancelled:
            os.write(write_fd, b'1')
        os.close(write_fd)
        write_fd = -1
        while True:
            waited, status = os.waitpid(child, os.WNOHANG)
            if waited:
                child = None
                code = os.waitstatus_to_exitcode(status)
                return 130 if cancelled else (128 - code if code < 0 else code)
            notice = signal.sigwaitinfo(SIGNALS)
            if notice.si_signo in (signal.SIGTERM, signal.SIGINT):
                cancelled = True
                send(child_handle, signal.SIGTERM)
    finally:
        for fd in (read_fd, write_fd, parent_handle):
            if fd != -1:
                os.close(fd)
        if child is not None:
            try:
                if child_handle is not None:
                    send(child_handle, signal.SIGKILL)
            finally:
                # Gate EOF if reservation failed. Even a denied signal cannot
                # bypass the namespace-exit barrier or authorize fake cleanup.
                os.waitpid(child, 0)
        if child_handle is not None:
            os.close(child_handle)


def main():
    parent_handle = None
    if sys.platform == 'linux' and sys.argv[1:2] == ['--parent-ready-fd']:
        gate, parent = int(sys.argv[2]), int(sys.argv[4])
        if sys.argv[3] != '--parent-pid':
            raise CustodyError('missing adapter parent identity')
        del sys.argv[1:5]
        libc = ctypes.CDLL(None, use_errno=True)
        checked(libc.prctl(1, signal.SIGKILL, 0, 0, 0), 'adapter parent-death signal')
        if os.getppid() != parent:
            raise CustodyError('adapter parent exited; target not launched')
        parent_handle = os.pidfd_open(parent)
        if os.getppid() != parent:
            raise CustodyError('adapter parent exited; target not launched')
        try:
            if os.read(gate, 1) != b'1':
                raise CustodyError('parent pidfd reservation missing; target not launched')
        finally:
            os.close(gate)
    resource_root, private_display = None, False
    if sys.argv[1:2] == ['--private-resource-root']:
        if len(sys.argv) < 4:
            raise CustodyError('missing private resource root/command')
        resource_root = sys.argv[2]
        del sys.argv[1:3]
        if sys.argv[1:2] == ['--private-display']:
            private_display = True
            del sys.argv[1]
    try:
        return supervise(sys.argv[1:], parent_handle, resource_root, private_display)
    finally:
        if parent_handle is not None:
            os.close(parent_handle)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (CustodyError, OSError) as exc:
        print('namespace custody: ' + str(exc), file=sys.stderr, flush=True)
        sys.exit(125)
