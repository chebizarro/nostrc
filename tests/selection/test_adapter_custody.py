"""Actual adapter cancellation: escaped TERM-ignoring trees, bytes, and siblings.

Runs on macOS (legacy compatibility, not identity-safe custody) and Linux.
Readiness/exit are socket/pipe events; guards are not sleeps or retries.
"""
import json
import array
import fcntl
import struct
import os
from pathlib import Path
import platform
import shutil
import select
import signal
import socket
import subprocess
import sys
import unittest
import uuid

import test_local_command_adapter as fixture

ROOT = Path(__file__).resolve().parents[2]


class AdapterCustodyTests(unittest.TestCase):
    def setUp(self):
        fixture.LocalCommandAdapterTests.setUp(self)
        if os.environ.get('NOSTRC_CUSTODY_TEST_EVIDENCE'):
            destination = Path(os.environ['NOSTRC_CUSTODY_TEST_EVIDENCE']) / (self._testMethodName + '-' + uuid.uuid4().hex)
            self.addCleanup(shutil.copytree, self.work, destination)
    write = fixture.LocalCommandAdapterTests.write
    git = fixture.LocalCommandAdapterTests.git
    commit = fixture.LocalCommandAdapterTests.commit
    save_policy = fixture.LocalCommandAdapterTests.save_policy
    gate = fixture.LocalCommandAdapterTests.gate

    def configure_ctest(self):
        profile = self.policy['profiles']['local.command']
        profile.update(compiler='clang' if platform.system() == 'Darwin' else 'gcc',
                       configure_args=['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug'])
        if platform.system() == 'Darwin':
            profile['configure_args'] += ['-DNIP55L_SECRET_BACKEND=libsecret',
                '-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service']
        self.policy['checks'] = {'ctest.actual': dict(kind='ctest', profiles=['local.command'],
            nodes=['app'], cost='ordinary', requirement='blocking', expected_outcome='pass',
            ctest_tests={'local.command': ['escaped']})}
        target = self.repo / 'cmake/NostrcTestInventory.cmake'
        target.parent.mkdir(exist_ok=True)
        shutil.copy2(ROOT / 'cmake/NostrcTestInventory.cmake', target)
        self.write('app/main.c', '#include <unistd.h>\nint main(int argc, char **argv) { (void)argc; execvp(argv[1], argv + 1); return 125; }\n')
        self.write('CMakeLists.txt', '''cmake_minimum_required(VERSION 3.22)
project(adapter_custody C)
enable_testing()
include(cmake/NostrcTestInventory.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(probe app/main.c)
add_test(NAME escaped COMMAND probe ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/app/check.py)
nostrc_describe_test(escaped OWNER app TARGETS probe INPUTS app/check.py EXPECTED pass)
nostrc_finalize_test_inventory()
''')
        self.save_policy()

    def cancel_tree(self, ctest=False, custody='legacy'):
        self.write('app/check.py', '''import json, os, signal, socket, subprocess, sys
if '--child' not in sys.argv:
    child = subprocess.Popen([sys.executable, __file__, '--child'], start_new_session=True)
    sys.exit(child.wait())
signal.signal(signal.SIGTERM, signal.SIG_IGN)
os.write(1, b'escaped ready \\xff\\n')
peer = socket.socket(socket.AF_UNIX)
peer.connect(os.environ['ADAPTER_READY_SOCKET'])
peer.sendall(json.dumps(dict(pid=os.getpid(), ppid=os.getppid(), pgid=os.getpgrp(),
                            sid=os.getsid(0))).encode() + b'\\n')
peer.recv(1)  # Test-owned emergency release, not a timer or numeric-PID kill.
''')
        if ctest:
            self.configure_ctest()
        head = self.commit()
        endpoint = '/tmp/nostrc-custody-' + uuid.uuid4().hex[:16] + '.sock'
        server = socket.socket(socket.AF_UNIX)
        self.addCleanup(server.close)
        server.bind(endpoint)
        self.addCleanup(os.unlink, endpoint)
        server.listen(1)
        server.settimeout(30)
        sibling = subprocess.Popen([sys.executable, '-u', '-c',
            "import sys; print('ready'); print(sys.stdin.readline().strip()); sys.stdin.read()"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, start_new_session=True)
        evidence = self.work / 'custody-evidence'
        process = subprocess.Popen(['bash', str(self.repo / 'scripts/test-gate.sh'), '--repo', str(self.repo),
            '--head', head, '--broad', '--evidence-dir', str(evidence), '--linux-custody', custody],
            env=dict(os.environ, ADAPTER_READY_SOCKET=endpoint, PYTHONDONTWRITEBYTECODE='1'),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        peer = None
        try:
            self.assertEqual(sibling.stdout.readline(), b'ready\n')
            try:
                peer, _ = server.accept()
            except TimeoutError:
                if process.poll() is not None:
                    output = process.communicate()[0]
                    logs = '\n'.join(str(p) + ': ' + p.read_text(errors='replace')[-6000:]
                                     for p in evidence.rglob('*.log'))
                    self.fail(f'adapter exited before readiness: {output!r}\n{logs}')
                raise
            peer.settimeout(20)
            info = json.loads(peer.recv(4096))
            self.assertEqual(info['pid'], info['pgid'], info)
            self.assertEqual(info['pid'], info['sid'], info)
            self.assertNotEqual(info['ppid'], info['pgid'], info)
            process.terminate()
            output = process.communicate(timeout=20)[0]
            self.assertEqual(process.returncode, 130, output)
            self.assertEqual(peer.recv(1), b'', 'escaped child still owns socket')
            state = subprocess.run(['ps', '-o', 'stat=', '-p', str(info['pid'])],
                                   capture_output=True, text=True).stdout.strip()
            self.assertTrue(not state or state.startswith('Z'), state)
            sibling.stdin.write(b'sibling survived\n')
            sibling.stdin.flush()
            self.assertEqual(sibling.stdout.readline(), b'sibling survived\n')
            self.assertIsNone(sibling.poll())
            receipt = json.loads(next(evidence.glob('*/adapter-receipt.json')).read_text())
            self.assertEqual(receipt['status'], 'cancelled')
            self.assertFalse(receipt['complete'])
            self.assertFalse(receipt['complete_gate_eligible'])
            self.assertEqual(receipt['custody_mode'], custody)
            if custody == 'ptrace':
                self.assertFalse(receipt['untraced_profile_parity'])
            if ctest:
                # CTest buffers test stdout without -V. Preserve its actual raw
                # cancellation stream, not bytes it has never emitted upstream.
                ctest_logs = list(evidence.rglob('ctest.log'))
                self.assertEqual(len(ctest_logs), 1, output)
                self.assertIn(b'    Start 1: escaped\n', ctest_logs[0].read_bytes())
            else:
                self.assertTrue(any(b'escaped ready \xff\n' in p.read_bytes()
                                    for p in evidence.rglob('command-*.log')), output)
            backend_files = list(evidence.glob('*/0/lane-*/receipt.json'))
            self.assertEqual(len(backend_files), 1, output)
            failure = json.loads(backend_files[0].read_text())
            self.assertFalse(failure['diagnostic_eligible'], failure)
            self.assertFalse(failure['complete_gate_eligible'], failure)
            self.assertFalse(failure['complete'], failure)
            backend = failure['failure']['backend_receipt']
            self.assertFalse(backend['diagnostic_eligible'], backend)
            self.assertFalse(backend['complete_gate_eligible'], backend)
            self.assertNotEqual(backend['outcome'], 'passed', backend)
            self.assertEqual(self.git('worktree', 'list', '--porcelain').count('worktree '), 1)
        finally:
            if peer:
                peer.close()
            if process.poll() is None:
                process.terminate()
            process.communicate(timeout=20)
            sibling.communicate(timeout=20)

    def test_command_escaped_child_cancellation(self):
        self.cancel_tree()

    def test_ctest_escaped_child_cancellation(self):
        self.cancel_tree(ctest=True)

    if sys.platform == 'linux':
        def test_ptrace_command_escaped_child_cancellation(self):
            self.cancel_tree(custody='ptrace')

        def test_ptrace_ctest_escaped_child_cancellation(self):
            self.cancel_tree(ctest=True, custody='ptrace')

        def test_ptrace_wrapped_ctest_refuses_before_lane_launch(self):
            self.configure_ctest()
            for capabilities in (['display'], ['private_bus'], ['display', 'private_bus']):
                with self.subTest(capabilities=capabilities):
                    self.policy['profiles']['local.command']['capabilities'] = capabilities
                    self.save_policy()
                    head = self.commit()
                    receipt, directory = self.gate('--head', head, '--linux-custody', 'ptrace', success=False)
                    self.assertEqual(receipt['status'], 'failed')
                    self.assertFalse(receipt['complete_gate_eligible'])
                    self.assertFalse(receipt['untraced_profile_parity'])
                    lane = receipt['executions'][0]['lanes'][0]
                    self.assertEqual(lane['error'], f'unsupported ptrace custody wrappers: {sorted(capabilities)}; lane not launched')
                    for artifact in ('lane.log', 'resolve.log', 'resolved.json', 'receipt.json', 'submodules.log', 'build'):
                        self.assertFalse(any(directory.rglob(artifact)), artifact)

        def test_ptrace_instrumentation_refuses_before_lane_launch(self):
            self.policy['profiles']['local.command']['instrumentation'] = 'asan'
            self.save_policy()
            head = self.commit()
            receipt, directory = self.gate('--head', head, '--linux-custody', 'ptrace', success=False)
            self.assertEqual(receipt['status'], 'failed')
            self.assertFalse(receipt['complete_gate_eligible'])
            self.assertFalse(receipt['untraced_profile_parity'])
            lane = receipt['executions'][0]['lanes'][0]
            self.assertEqual(lane['error'], 'unsupported ptrace custody instrumentation: asan; lane not launched')
            for artifact in ('lane.log', 'resolve.log', 'resolved.json', 'receipt.json', 'submodules.log', 'build'):
                self.assertFalse(any(directory.rglob(artifact)), artifact)

    def linux_supervisor_normal_exit_reaps_escaped_background_tree(self):
        self.write('app/background.py', '''import os, signal, socket
signal.signal(signal.SIGTERM, signal.SIG_IGN)
peer = socket.socket(socket.AF_UNIX)
peer.connect(os.environ['ADAPTER_READY_SOCKET'])
peer.sendall(b'ready')
peer.recv(1)
''')
        endpoint = '/tmp/nostrc-custody-' + uuid.uuid4().hex[:16] + '.sock'
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(endpoint)
            self.addCleanup(os.unlink, endpoint)
            server.listen(1)
            server.settimeout(20)
            # Root waits for the child readiness before exiting, without waiting
            # for the child itself. The supervisor must kill it on normal exit.
            script = '''import subprocess, sys
p = subprocess.Popen([sys.executable, sys.argv[1]], start_new_session=True)
sys.stdin.buffer.read(1)
'''
            process = subprocess.Popen([sys.executable, str(ROOT / 'scripts/adapter_custody.py'),
                sys.executable, '-c', script, str(self.repo / 'app/background.py')],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                env=dict(os.environ, ADAPTER_READY_SOCKET=endpoint))
            peer = None
            try:
                peer, _ = server.accept()
                peer.settimeout(20)
                self.assertEqual(peer.recv(5), b'ready')
                output = process.communicate(b'x', timeout=20)[0]
                self.assertEqual(process.returncode, 0, output)
                self.assertEqual(peer.recv(1), b'')
            finally:
                if peer:
                    peer.close()
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=20)


if sys.platform == 'linux':
    class LinuxSupervisorTests(unittest.TestCase):
        setUp = AdapterCustodyTests.setUp
        write = AdapterCustodyTests.write
        git = AdapterCustodyTests.git
        save_policy = AdapterCustodyTests.save_policy
        commit = AdapterCustodyTests.commit
        test_normal_exit_cleans_background_tree = AdapterCustodyTests.linux_supervisor_normal_exit_reaps_escaped_background_tree

        def test_startup_parent_reservation_eof_and_pending_term_never_launch_target(self):
            for pending_term in (False, True):
                with self.subTest(pending_term=pending_term):
                    marker = self.work / ('launched-' + str(pending_term))
                    read_fd, write_fd = os.pipe()
                    mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
                    try:
                        process = subprocess.Popen([sys.executable, str(ROOT / 'scripts/adapter_custody.py'),
                            '--parent-ready-fd', str(read_fd), sys.executable, '-c',
                            f'from pathlib import Path; Path({str(marker)!r}).touch()'],
                            pass_fds=(read_fd,), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                        handle = os.pidfd_open(process.pid)
                    finally:
                        signal.pthread_sigmask(signal.SIG_SETMASK, mask)
                        os.close(read_fd)
                    try:
                        if pending_term:
                            signal.pidfd_send_signal(handle, signal.SIGTERM)
                            os.write(write_fd, b'1')
                        os.close(write_fd)
                        output = process.communicate(timeout=20)[0]
                        self.assertEqual(process.returncode, 130 if pending_term else 125, output)
                        self.assertFalse(marker.exists())
                    finally:
                        os.close(handle)

        def test_repeated_cancellation_preserves_backend_finalization(self):
            endpoint = '/tmp/nostrc-custody-' + uuid.uuid4().hex[:16] + '.sock'
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(endpoint)
                self.addCleanup(os.unlink, endpoint)
                server.listen(1)
                server.settimeout(20)
                script = '''import os, signal, socket
peer = socket.socket(socket.AF_UNIX)
peer.connect(ENDPOINT)
def stop(sig, frame):
    os.write(1, b'cancelling \\xff\\n')
    peer.sendall(b'cancelled')
    peer.recv(1)
    os.write(1, b'final negative receipt boundary\\n')
    raise SystemExit(23)
signal.signal(signal.SIGTERM, stop)
peer.sendall(b'ready')
signal.pause()
'''.replace('ENDPOINT', repr(endpoint))
                process = subprocess.Popen([sys.executable, str(ROOT / 'scripts/adapter_custody.py'),
                    sys.executable, '-c', script], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                handle = os.pidfd_open(process.pid)
                peer = None
                try:
                    peer, _ = server.accept()
                    peer.settimeout(20)
                    self.assertEqual(peer.recv(5), b'ready')
                    signal.pidfd_send_signal(handle, signal.SIGTERM)
                    self.assertEqual(peer.recv(9), b'cancelled')
                    signal.pidfd_send_signal(handle, signal.SIGTERM)
                    signal.pidfd_send_signal(handle, signal.SIGINT)
                    peer.sendall(b'1')
                    output = process.communicate(timeout=20)[0]
                    self.assertEqual(process.returncode, 130, output)
                    self.assertEqual(output, b'cancelling \xff\nfinal negative receipt boundary\n')
                finally:
                    if peer:
                        peer.close()
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.communicate(timeout=20)
                    os.close(handle)

        def test_denied_ptrace_or_pidfd_refuses_before_target_exec(self):
            source = self.work / 'deny.c'
            binary = self.work / 'deny'
            source.write_text('''#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc, char **argv) {
    (void)argc;
    unsigned int nr = atoi(argv[1]);
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    struct sock_fprog p = { .len = 4, .filter = f };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &p)) return 120;
    execvp(argv[2], argv + 2);
    return 121;
}
''')
            subprocess.run(['cc', str(source), '-o', str(binary)], check=True, capture_output=True, timeout=20)
            ptrace_nr = {'aarch64': 117, 'x86_64': 101}[platform.machine()]
            for syscall in (ptrace_nr, 434, 424):  # pidfd_open / pidfd_send_signal on both ABIs.
                with self.subTest(syscall=syscall):
                    marker = self.work / str(syscall)
                    result = subprocess.run([str(binary), str(syscall), sys.executable,
                        str(ROOT / 'scripts/adapter_custody.py'), sys.executable, '-c',
                        f'from pathlib import Path; Path({str(marker)!r}).touch()'],
                        capture_output=True, timeout=20)
                    self.assertEqual(result.returncode, 125, result.stderr)
                    self.assertFalse(marker.exists())
            for syscall in (434, 424):
                evidence = self.work / ('denied-parent-' + str(syscall))
                result = subprocess.run([str(binary), str(syscall), 'bash', str(self.repo / 'scripts/test-gate.sh'),
                    '--repo', str(self.repo), '--head', self.head, '--broad', '--linux-custody', 'ptrace',
                    '--evidence-dir', str(evidence)], capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 1, result.stderr)
                receipt = json.loads(next(evidence.glob('*/adapter-receipt.json')).read_text())
                self.assertEqual(receipt['status'], 'failed')
                self.assertFalse(receipt['complete_gate_eligible'])
                self.assertFalse(any(evidence.glob('*/0/source/app/check.py')))

        def test_adapter_pending_term_at_reservation_does_not_release_target(self):
            source = self.work / 'hold-reservation.c'
            binary = self.work / 'hold-reservation'
            source.write_text('''#define _GNU_SOURCE
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc, char **argv) {
    (void)argc;
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_pidfd_open, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    struct sock_fprog p = { .len = 4, .filter = f };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return 120;
    int fd = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &p);
    if (fd < 0) return 121;
    char control[CMSG_SPACE(sizeof(fd))] = {0};
    char byte = '1';
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(fd));
    *(int *)CMSG_DATA(c) = fd;
    if (sendmsg(atoi(argv[1]), &msg, 0) != 1) return 122;
    close(atoi(argv[1])); close(fd);
    printf("%lu %lu\\n", (unsigned long)SECCOMP_IOCTL_NOTIF_RECV, (unsigned long)SECCOMP_IOCTL_NOTIF_SEND);
    fflush(stdout);
    execvp(argv[2], argv + 2);
    return 123;
}
''')
            subprocess.run(['cc', str(source), '-o', str(binary)], check=True, capture_output=True, timeout=20)
            parent, child = socket.socketpair(socket.AF_UNIX)
            evidence = self.work / 'queued-adapter-cancel'
            process = subprocess.Popen([str(binary), str(child.fileno()), 'bash',
                str(self.repo / 'scripts/test-gate.sh'), '--repo', str(self.repo), '--head', self.head,
                '--broad', '--linux-custody', 'ptrace', '--evidence-dir', str(evidence)],
                pass_fds=(child.fileno(),), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            child.close()
            handle = os.pidfd_open(process.pid)
            listener = None
            try:
                parent.settimeout(20)
                message, ancillary, _, _ = parent.recvmsg(1, socket.CMSG_SPACE(4))
                self.assertEqual(message, b'1')
                received = array.array('i')
                self.assertEqual(len(ancillary), 1)
                self.assertEqual(ancillary[0][:2], (socket.SOL_SOCKET, socket.SCM_RIGHTS))
                received.frombytes(ancillary[0][2])
                self.assertEqual(len(received), 1)
                listener = received[0]
                recv_ioctl, send_ioctl = map(int, process.stdout.readline().split())
                ready, _, _ = select.select([listener], [], [], 20)
                self.assertEqual(ready, [listener])
                request = bytearray(80)  # seccomp_notif with native seccomp_data
                fcntl.ioctl(listener, recv_ioctl, request, True)
                request_id, request_pid, _ = struct.unpack_from('QII', request)
                self.assertEqual(request_pid, process.pid)
                signal.pidfd_send_signal(handle, signal.SIGTERM)
                response = struct.pack('QqiI', request_id, 0, 0, 1)  # NOTIF_FLAG_CONTINUE
                fcntl.ioctl(listener, send_ioctl, response)
                output = process.communicate(timeout=20)[0]
                self.assertEqual(process.returncode, 130, output)
                receipt = json.loads(next(evidence.glob('*/adapter-receipt.json')).read_text())
                self.assertEqual(receipt['status'], 'cancelled')
                self.assertFalse(receipt['complete_gate_eligible'])
                self.assertFalse(any(evidence.glob('*/0/source')))
            finally:
                parent.close()
                if listener is not None:
                    os.close(listener)
                try:
                    signal.pidfd_send_signal(handle, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.communicate(timeout=20)
                os.close(handle)

        def test_proc_status_unavailable_refuses_before_fork(self):
            source = self.work / 'deny-open.c'
            library = self.work / 'deny-open.so'
            source.write_text('''#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
int deny_open(void) {
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_openat, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOENT),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    struct sock_fprog p = { .len = 4, .filter = f };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return 120;
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &p);
}
''')
            subprocess.run(['cc', '-shared', '-fPIC', str(source), '-o', str(library)],
                           check=True, capture_output=True, timeout=20)
            marker = self.work / 'proc-must-not-launch'
            script = f'''import ctypes, runpy, sys
h = runpy.run_path({str(ROOT / 'scripts/adapter_custody.py')!r})
lib = ctypes.CDLL({str(library)!r})
assert lib.deny_open() == 0
try:
    h['supervise']([sys.executable, '-c', {f'from pathlib import Path; Path({str(marker)!r}).touch()'!r}])
except h['CustodyError'] as exc:
    print(exc)
    sys.exit(125)
'''
            result = subprocess.run([sys.executable, '-c', script], capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 125, result.stderr)
            self.assertIn(b'proc status unavailable; target not launched', result.stdout)
            self.assertFalse(marker.exists())

        def test_collector_allocation_failure_kills_and_reaps_before_return(self):
            endpoint = '/tmp/nostrc-custody-' + uuid.uuid4().hex[:16] + '.sock'
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(endpoint)
                self.addCleanup(os.unlink, endpoint)
                server.listen(32)
                child = "import signal, socket; signal.signal(signal.SIGTERM, signal.SIG_IGN); p=socket.socket(socket.AF_UNIX); p.connect(" + repr(endpoint) + "); p.sendall(b'ready'); print('ready',flush=True); p.recv(1)"
                root = "import subprocess, sys; children=[]\nfor _ in range(20):\n p=subprocess.Popen([sys.executable,'-c'," + repr(child) + "],stdout=subprocess.PIPE,start_new_session=True); assert p.stdout.readline()==b'ready\\n'; p.stdout.close(); children.append(p)\nfor p in children: p.wait()"
                runner = f'''import resource, runpy, sys
h=runpy.run_path({str(ROOT / 'scripts/adapter_custody.py')!r})
resource.setrlimit(resource.RLIMIT_NOFILE, (10, 10))
try:
    sys.exit(h['supervise']([sys.executable, '-c', {root!r}]))
except OSError as exc:
    print('original collector error: ' + str(exc), file=sys.stderr)
    sys.exit(125)
'''
                process = subprocess.Popen([sys.executable, '-c', runner], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                handle = os.pidfd_open(process.pid)
                peers = []
                try:
                    while True:
                        ready, _, _ = select.select([server, handle], [], [], 20)
                        if not ready:
                            tree = subprocess.run(['ps', '-axo', 'pid,ppid,stat,wchan:30,command'], capture_output=True)
                            (self.work / 'collector-timeout-tree.log').write_bytes(tree.stdout + tree.stderr)
                        self.assertTrue(ready, 'collector exit guard expired')
                        if server in ready:
                            peer, _ = server.accept()
                            peer.settimeout(20)
                            peers.append(peer)
                            self.assertEqual(peer.recv(5), b'ready')
                        if handle in ready:
                            break
                    output = process.communicate(timeout=20)[0]
                    self.assertEqual(process.returncode, 125, output)
                    self.assertIn(b'original collector error:', output)
                    self.assertIn(b'Too many open files', output)
                    self.assertNotIn(b'cleanup_unverified', output)
                    self.assertTrue(peers, output)
                    for peer in peers:
                        self.assertEqual(peer.recv(1), b'', 'task alive beyond fatal-path reap barrier')
                finally:
                    for peer in peers:
                        peer.close()
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    final_output = process.communicate(timeout=20)[0]
                    (self.work / 'collector-final-output.log').write_bytes(final_output)
                    os.close(handle)

        def test_threads_nonleader_exec_fork_and_failure_bytes(self):
            nested = "import os; os.write(1, b'nested \\xff\\n')"
            replacement = f"import subprocess, sys; print('thread exec', flush=True); subprocess.run([sys.executable, '-c', {nested!r}], check=True); sys.exit(23)"
            script = f"import os, sys, threading; threading.Thread(target=lambda: os.execv(sys.executable, [sys.executable, '-c', {replacement!r}])).start(); threading.Event().wait()"
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/adapter_custody.py'),
                sys.executable, '-c', script], capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 23, result.stderr)
            self.assertEqual(result.stdout, b'thread exec\nnested \xff\n')

        def test_untraced_clone_and_clone3_refuse_in_actual_kernel(self):
            source = self.work / 'clone.c'
            binary = self.work / 'clone'
            source.write_text('''#define _GNU_SOURCE
#include <errno.h>
#include <linux/sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    long child = syscall(SYS_clone, CLONE_UNTRACED | SIGCHLD, 0, 0, 0, 0);
    if (child == 0) _exit(99);
    if (child > 0) { waitpid(child, 0, 0); return 10; }
    if (errno != EPERM) return 11;
    struct clone_args args = { .exit_signal = SIGCHLD };
    child = syscall(SYS_clone3, &args, sizeof(args));
    if (child == 0) _exit(99);
    if (child > 0) { waitpid(child, 0, 0); return 12; }
    if (errno != ENOSYS) return 13;
    puts("untraced creation refused");
    return 0;
}
''')
            subprocess.run(['cc', str(source), '-o', str(binary)], check=True, capture_output=True, timeout=20)
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/adapter_custody.py'), str(binary)],
                                    capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, b'untraced creation refused\n')

        def test_supervisor_death_kills_actual_escaped_child(self):
            endpoint = '/tmp/nostrc-custody-' + uuid.uuid4().hex[:16] + '.sock'
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(endpoint)
                self.addCleanup(os.unlink, endpoint)
                server.listen(1)
                server.settimeout(20)
                child = "import signal, socket; signal.signal(signal.SIGTERM, signal.SIG_IGN); p=socket.socket(socket.AF_UNIX); p.connect(" + repr(endpoint) + "); p.sendall(b'ready'); p.recv(1)"
                root = "import subprocess, sys; p=subprocess.Popen([sys.executable, '-c', " + repr(child) + "], start_new_session=True); p.wait()"
                process = subprocess.Popen([sys.executable, str(ROOT / 'scripts/adapter_custody.py'),
                    sys.executable, '-c', root], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                handle = os.pidfd_open(process.pid)
                peer = None
                try:
                    peer, _ = server.accept()
                    peer.settimeout(20)
                    self.assertEqual(peer.recv(5), b'ready')
                    signal.pidfd_send_signal(handle, signal.SIGKILL)
                    output = process.communicate(timeout=20)[0]
                    self.assertEqual(process.returncode, -signal.SIGKILL, output)
                    self.assertEqual(peer.recv(1), b'')
                finally:
                    if peer:
                        peer.close()
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.communicate(timeout=20)
                    os.close(handle)


if __name__ == '__main__':
    unittest.main()
