#!/usr/bin/env python3
"""Same-launcher Linux userns diagnostic, not custody/profile acceptance.

Only a disposable child changes its own namespaces/maps. The parent observes
proc ownership from the original userns, while the child is handshake-gated.
No host settings/policy, workloads, elevated capabilities or dumpability increase.
An errno alone does not attribute a refusal to AppArmor; retain correlated audit.
"""
import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import sys

SETTINGS = (
    'fs/suid_dumpable', 'kernel/unprivileged_userns_clone',
    'kernel/apparmor_restrict_unprivileged_userns',
    'kernel/apparmor_restrict_unprivileged_unconfined', 'user/max_user_namespaces',
)


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError as exc:
        return {'errno': exc.errno, 'error': str(exc)}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def emit(record):
    print(json.dumps(dict(time_utc=datetime.now(timezone.utc).isoformat(), **record)), flush=True)


def snapshot(libc):
    status = read('/proc/self/status')
    return dict(pid=os.getpid(), uid=list(os.getresuid()), gid=list(os.getresgid()),
                dumpable=libc.prctl(3, 0, 0, 0, 0),
                apparmor=read('/proc/self/attr/current'),
                apparmor_stacked=read('/proc/self/attr/apparmor/current'),
                userns=os.readlink('/proc/self/ns/user'),
                status=[line for line in status.splitlines() if line.startswith(
                    ('Uid:', 'Gid:', 'Cap', 'NoNewPrivs:', 'Seccomp:'))] if isinstance(status, str) else status)


def probe(nondumpable=False, apparmor_profile=None):
    if sys.platform != 'linux' or os.geteuid() == 0:
        emit(dict(probe_exit=125, refusal='requires the ordinary nonroot Linux worker launcher'))
        return 125
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
    root = Path(__file__).resolve().parents[2]
    emit(dict(kind='namespace_host_probe_v1', scope='diagnostic_not_custody_or_profile_acceptance',
              apparmor_profile_requested=apparmor_profile, kernel=list(os.uname()), python_version=platform.python_version(), parent=snapshot(libc),
              source_sha256={str(path): digest(path) for path in (
                  Path(__file__).resolve(), root / 'scripts/adapter_namespace_custody.py',
                  Path(sys.executable).resolve())},
              runner={key: os.environ.get(key) for key in (
                  'GITHUB_SHA', 'GITHUB_WORKFLOW_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT',
                  'RUNNER_OS', 'RUNNER_ARCH', 'ImageOS', 'ImageVersion')},
              lsm=read('/sys/kernel/security/lsm'),
              settings={path: read('/proc/sys/' + path) for path in SETTINGS}))
    parent, child = socket.socketpair()
    parent.settimeout(30)
    child.settimeout(30)
    pid = os.fork()
    if pid == 0:
        parent.close()
        stream = child.makefile('rwb', buffering=0)
        uid, gid = os.geteuid(), os.getegid()

        def stage(name, **fields):
            stream.write((json.dumps(dict(stage=name, **snapshot(libc), **fields)) + '\n').encode())
            if stream.readline() != b'1\n':
                os._exit(125)

        try:
            if nondumpable and libc.prctl(4, 0, 0, 0, 0) == -1:
                raise OSError(ctypes.get_errno(), 'PR_SET_DUMPABLE=0')
            if apparmor_profile is not None:
                # The exact production entry, not a reimplementation of it.
                sys.path.insert(0, str(root / 'scripts'))
                from adapter_namespace_custody import enter_apparmor_profile
                try:
                    stage('profile-entered', label=enter_apparmor_profile(apparmor_profile))
                except Exception as exc:
                    stage('profile-refused', error=str(exc))
                    os._exit(125)
            stage('before-unshare')
            result = libc.unshare(0x10000000)
            stage('after-unshare', unshare_result=result,
                  unshare_errno=ctypes.get_errno() if result == -1 else 0)
            if result == -1:
                os._exit(125)
            for path, value in (('setgroups', 'deny'), ('uid_map', f'{uid} {uid} 1\n'),
                                ('gid_map', f'{gid} {gid} 1\n')):
                try:
                    fd = os.open('/proc/self/' + path, os.O_WRONLY | os.O_CLOEXEC)
                except OSError as exc:
                    stage('open-failed', path=path, errno=exc.errno, error=str(exc))
                    os._exit(125)
                try:
                    os.write(fd, value.encode())
                except OSError as exc:
                    stage('write-failed', path=path, errno=exc.errno, error=str(exc))
                    os._exit(125)
                finally:
                    os.close(fd)
                stage('write-ok', path=path)
            os._exit(0)
        except BaseException as exc:
            try:
                stage('probe-error', error=repr(exc))
            finally:
                os._exit(125)
    child.close()
    try:
        with parent, parent.makefile('rwb', buffering=0) as stream:
            for line in stream:
                record = json.loads(line)
                owners = {}
                for name in ('setgroups', 'uid_map', 'gid_map'):
                    try:
                        st = os.stat(f'/proc/{pid}/{name}')
                        owners[name] = dict(uid=st.st_uid, gid=st.st_gid, mode=oct(st.st_mode & 0o777))
                    except OSError as exc:
                        owners[name] = dict(errno=exc.errno)
                emit(dict(**record, original_userns_proc_owners=owners))
                stream.write(b'1\n')
    finally:
        _, status = os.waitpid(pid, 0)
    code = os.waitstatus_to_exitcode(status)
    emit(dict(probe_exit=code))
    return code


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nondumpable', action='store_true', help='only lower disposable child dumpability')
    parser.add_argument('--apparmor-profile', help='disposable child explicitly enters this provisioned profile first')
    options = parser.parse_args()
    sys.exit(probe(options.nondumpable, options.apparmor_profile))
