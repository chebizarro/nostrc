"""Real kernel custody tests; Linux positives require unprivileged namespaces.

No capability skips: a Linux host which denies this backend must run refusal
checks separately, not report positive coverage. macOS tests prove refusal.
"""
import errno
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / 'scripts/adapter_namespace_custody.py'


class NamespaceCustodyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nostrc-namespace-custody-')
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.runs = 0
        if os.environ.get('NOSTRC_CUSTODY_TEST_EVIDENCE'):
            destination = Path(os.environ['NOSTRC_CUSTODY_TEST_EVIDENCE']) / (self._testMethodName + '-' + uuid.uuid4().hex)
            self.addCleanup(shutil.copytree, self.work, destination)

    def run_helper(self, *argv):
        result = subprocess.run([sys.executable, str(HELPER), *argv], capture_output=True, timeout=25)
        self.runs += 1
        (self.work / f'run-{self.runs}.stdout').write_bytes(result.stdout)
        (self.work / f'run-{self.runs}.stderr').write_bytes(result.stderr)
        (self.work / f'run-{self.runs}.status').write_text(str(result.returncode))
        return result

    if sys.platform == 'darwin':
        def test_kernel_backend_refuses_before_exec_and_checkout(self):
            marker = self.work / 'never-executed'
            result = self.run_helper(sys.executable, '-c', f'open({str(marker)!r}, "w").close()')
            self.assertEqual(result.returncode, 125, result.stderr)
            self.assertIn(b'com.apple.developer.endpoint-security.client (not provisioned)', result.stderr)
            self.assertFalse(marker.exists())
            evidence = self.work / 'evidence'
            result = subprocess.run(['bash', str(ROOT / 'scripts/test-gate.sh'), '--repo', str(ROOT),
                '--head', 'HEAD', '--native-custody', 'pidns', '--evidence-dir', str(evidence)],
                capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 1, result.stderr)
            receipt = json.loads(next(evidence.glob('*/adapter-receipt.json')).read_text())
            self.assertEqual(receipt['status'], 'failed')
            self.assertFalse(receipt['complete_gate_eligible'])
            self.assertFalse(receipt['untraced_profile_parity'])
            self.assertFalse(any(evidence.glob('*/0/source')))

        def test_actual_descendant_client_entitlement_boundary(self):
            source, binary = self.work / 'es-probe.c', self.work / 'es-probe'
            source.write_text('''#include <EndpointSecurity/EndpointSecurity.h>
#include <dlfcn.h>
#include <stdio.h>
int main(void) {
 typedef es_new_client_result_t (*create_fn)(es_client_t **, es_handler_block_t);
 create_fn create = (create_fn)dlsym(RTLD_DEFAULT, "es_new_descendants_client");
 if (!create) { puts("descendant client unavailable on this OS; backend refuses"); return 0; }
 es_client_t *client = NULL;
 es_new_client_result_t r = create(&client, ^(es_client_t *c, const es_message_t *m) { (void)c; (void)m; });
 printf("descendants_result=%d not_entitled=%d\\n", r, ES_NEW_CLIENT_RESULT_ERR_NOT_ENTITLED);
 if (r == ES_NEW_CLIENT_RESULT_SUCCESS) es_delete_client(client);
 return r == ES_NEW_CLIENT_RESULT_ERR_NOT_ENTITLED ? 0 : 1;
}
''')
            subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-fblocks', str(source),
                            '-lEndpointSecurity', '-o', str(binary)], check=True, capture_output=True, timeout=20)
            result = subprocess.run([str(binary)], capture_output=True, timeout=20)
            (self.work / 'entitlement-result.stdout').write_bytes(result.stdout)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(b'descendants_result=3 not_entitled=3' in result.stdout
                            or b'descendant client unavailable on this OS; backend refuses' in result.stdout,
                            result.stdout)

        def test_actual_kqueue_recursive_tracking_is_unsupported(self):
            queue = select.kqueue()
            try:
                # NOTE_TRACK=1 is defined by XNU but deliberately unsupported.
                events = queue.control([select.kevent(os.getpid(), filter=select.KQ_FILTER_PROC,
                    flags=select.KQ_EV_ADD, fflags=1)], 1, 0)
            finally:
                queue.close()
            self.assertEqual(len(events), 1)
            self.assertTrue(events[0].flags & select.KQ_EV_ERROR)
            self.assertEqual(events[0].data, errno.ENOTSUP)

    if sys.platform == 'linux':
        def test_untraced_identity_procfs_and_unchanged_uid(self):
            code = '''import json, os
s = dict(line.split(':', 1) for line in open('/proc/self/status') if ':' in line)
print(json.dumps(dict(pid=os.getpid(), tgid=int(s['Tgid']), tracer=int(s['TracerPid']),
                     uid=os.getuid(), gid=os.getgid(), nnp=int(s['NoNewPrivs']))))
'''
            result = self.run_helper(sys.executable, '-c', code)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), dict(pid=2, tgid=2, tracer=0,
                uid=os.getuid(), gid=os.getgid(), nnp=1))

        def test_real_lsan_pass_and_leak_failure_without_ptrace(self):
            source, binary = self.work / 'leak.c', self.work / 'leak'
            source.write_text('''#include <stdlib.h>
#include <unistd.h>
__attribute__((noinline)) void allocate(void) { void *p = malloc(1234); if (!p) _exit(12); ((volatile char *)p)[0]=1; }
int main(int argc, char **argv) { (void)argv; if(argc > 1) allocate(); return 0; }
''')
            subprocess.run(['cc', '-g', '-O0', '-fsanitize=leak', str(source), '-o', str(binary)],
                           check=True, capture_output=True, timeout=20)
            positive = self.run_helper(str(binary))
            self.assertEqual(positive.returncode, 0, positive.stderr)
            negative = self.run_helper(str(binary), 'leak')
            self.assertEqual(negative.returncode, 23, negative.stderr)
            self.assertIn(b'LeakSanitizer: detected memory leaks', negative.stderr)
            self.assertIn(b'1234 byte(s)', negative.stderr)
            self.assertNotIn(b'does not work under ptrace', negative.stderr)

        def tree(self, action, nested=False):
            endpoint = '/tmp/nostrc-ns-' + uuid.uuid4().hex[:16]
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(endpoint)
                self.addCleanup(os.unlink, endpoint)
                server.listen(1)
                server.settimeout(20)
                child = '''import os, signal, socket
signal.signal(signal.SIGTERM, signal.SIG_IGN)
p=socket.socket(socket.AF_UNIX); p.connect(ENDPOINT); p.sendall(b'ready'); p.recv(1)
'''.replace('ENDPOINT', repr(endpoint))
                # Parent exits only after the escaped descendant is ready. A
                # double fork discards ancestry; setsid discards group custody.
                if not nested:
                    child = "import os\nif os.fork(): os._exit(0)\nos.setsid()\n" + child
                child_argv = (['unshare', '--user', '--map-current-user', '--pid', '--fork'] if nested else []) + [sys.executable, '-c', child]
                root = '''import os, signal, subprocess, sys
def stop(*_):
    os.write(1, b'cancel final \\xff\\n'); sys.exit(19)
signal.signal(signal.SIGTERM, stop)
p=subprocess.Popen(SPAWN, start_new_session=True)
WAIT
os.write(1, b'final bytes \\xff\\n')
sys.exit(EXIT)
'''.replace('SPAWN', repr(child_argv)).replace('EXIT', '23' if action == 'failure' else '0').replace(
                    'WAIT', 'signal.pause()' if action in ('cancel', 'kill', 'parentkill') else 'sys.stdin.buffer.read(1)')
                sibling = subprocess.Popen([sys.executable, '-u', '-c',
                    "import sys; print('ready'); print(sys.stdin.readline().strip()); sys.stdin.read()"],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, start_new_session=True)
                argv = [sys.executable, str(HELPER), sys.executable, '-c', root]
                if action == 'parentkill':
                    parent = '''import os, signal, subprocess
r,w=os.pipe()
signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
p=subprocess.Popen([PYTHON, HELPER, '--parent-ready-fd', str(r), '--parent-pid', str(os.getpid()), PYTHON, '-c', ROOT], pass_fds=(r,))
os.close(r)
fd=os.pidfd_open(p.pid); signal.pidfd_send_signal(fd, 0); os.write(w,b'1'); os.close(w)
p.wait()
'''.replace('PYTHON', repr(sys.executable)).replace('HELPER', repr(str(HELPER))).replace('ROOT', repr(root))
                    argv = [sys.executable, '-c', parent]
                process = subprocess.Popen(argv, stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                handle = os.pidfd_open(process.pid)
                peer = None
                try:
                    self.assertEqual(sibling.stdout.readline(), b'ready\n')
                    peer, _ = server.accept()
                    peer.settimeout(20)
                    self.assertEqual(peer.recv(5), b'ready')
                    if action in ('kill', 'parentkill'):
                        signal.pidfd_send_signal(handle, signal.SIGKILL)
                        output = process.communicate(timeout=20)[0]
                        self.assertEqual(process.returncode, -signal.SIGKILL, output)
                    elif action == 'cancel':
                        signal.pidfd_send_signal(handle, signal.SIGTERM)
                        signal.pidfd_send_signal(handle, signal.SIGINT)
                        output = process.communicate(timeout=20)[0]
                        self.assertEqual(process.returncode, 130, output)
                        self.assertEqual(output, b'cancel final \xff\n')
                    else:
                        output = process.communicate(b'1', timeout=20)[0]
                        self.assertEqual(process.returncode, 23 if action == 'failure' else 0, output)
                        self.assertEqual(output, b'final bytes \xff\n')
                    (self.work / 'final.stdout').write_bytes(output)
                    self.assertEqual(peer.recv(1), b'', 'escaped/nested child survived namespace barrier')
                    sibling.stdin.write(b'sibling survived\n'); sibling.stdin.flush()
                    self.assertEqual(sibling.stdout.readline(), b'sibling survived\n')
                    self.assertIsNone(sibling.poll())
                    (self.work / 'proof.json').write_text(json.dumps(dict(action=action, nested=nested,
                        returncode=process.returncode, escaped_socket_eof=True, sibling_roundtrip=True)))
                finally:
                    if peer:
                        peer.close()  # emergency fixture release, never a kill approximation
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.communicate(timeout=20)
                    os.close(handle)
                    sibling.communicate(timeout=20)

        def test_normal_exit_cleans_double_fork_setsid_child(self):
            self.tree('normal')

        def test_nonzero_exit_cleans_double_fork_setsid_child(self):
            self.tree('failure')

        def test_cancellation_cleans_child_and_retains_final_bytes(self):
            self.tree('cancel')

        def test_supervisor_death_kills_escaped_child(self):
            self.tree('kill')

        def test_adapter_parent_death_kills_actual_escaped_tree(self):
            self.tree('parentkill')

        def test_pending_cancel_and_unreserved_gate_never_launch(self):
            for cancel in (False, True):
                with self.subTest(cancel=cancel):
                    marker = self.work / ('never-executed-' + str(cancel))
                    read_fd, write_fd = os.pipe()
                    mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
                    try:
                        process = subprocess.Popen([sys.executable, str(HELPER), '--parent-ready-fd', str(read_fd),
                            '--parent-pid', str(os.getpid()), sys.executable, '-c',
                            f'open({str(marker)!r}, "w").close()'], pass_fds=(read_fd,),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                        handle = os.pidfd_open(process.pid)
                    finally:
                        signal.pthread_sigmask(signal.SIG_SETMASK, mask)
                        os.close(read_fd)
                    try:
                        if cancel:
                            signal.pidfd_send_signal(handle, signal.SIGTERM)
                            os.write(write_fd, b'1')
                        os.close(write_fd)
                        output = process.communicate(timeout=20)[0]
                        self.assertEqual(process.returncode, 130 if cancel else 125, output)
                        self.assertFalse(marker.exists())
                    finally:
                        os.close(handle)

        def test_nested_namespace_cannot_escape_cleanup(self):
            self.tree('cancel', nested=True)

        def test_denied_namespace_setup_never_launches_target(self):
            source, binary = self.work / 'deny.c', self.work / 'deny'
            source.write_text('''#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc, char **argv) {
 (void)argc;
 struct sock_filter f[]={
 BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
 BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,atoi(argv[1]),0,1),
 BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
 BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 struct sock_fprog p={.len=4,.filter=f};
 if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)||prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&p)) return 120;
 execvp(argv[2],argv+2); return 121;
}
''')
            subprocess.run(['cc', str(source), '-o', str(binary)], check=True, capture_output=True, timeout=20)
            import platform
            unshare_nr, mount_nr = {'aarch64': (97, 40), 'x86_64': (272, 165)}[platform.machine()]
            for nr in (unshare_nr, mount_nr, 434, 424):
                with self.subTest(syscall=nr):
                    marker = self.work / ('never-executed-' + str(nr))
                    result = subprocess.run([str(binary), str(nr), sys.executable, str(HELPER), sys.executable,
                        '-c', f'open({str(marker)!r}, "w").close()'], capture_output=True, timeout=20)
                    (self.work / f'denied-{nr}.stderr').write_bytes(result.stderr)
                    self.assertEqual(result.returncode, 125, result.stderr)
                    self.assertFalse(marker.exists())


if __name__ == '__main__':
    unittest.main()
