"""Native macOS seatbelt-tag custody tests (scripts/adapter_darwin_custody.py).

DRAFT backend tests. Positives run the actual helper against actual escaped
descendants; discovery races are driven deterministically through the scan's
observe seam with a command-driven, tagged lineage fixture (fork-and-exit,
exec) at the exact race points. No capability skips, no load generators, no
sleeps. Readiness and death are socket/kqueue events; timeouts are failure
guards. The tests never signal a descendant: members die only through the
helper's tag/audit-token path. Other platforms prove refusal.
"""
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / 'scripts/adapter_darwin_custody.py'
GUARD = 60

if sys.platform == 'darwin':
    sys.path.insert(0, str(ROOT / 'scripts'))
    import adapter_darwin_custody as custody

ESCAPE = textwrap.dedent('''\
    import os, signal, socket
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    if os.fork():
        os._exit(0)
    os.setsid()
    if os.fork():
        os._exit(0)
    # Detach stdio like a daemon: CTest waits for EOF from every output holder.
    null = os.open(os.devnull, os.O_RDWR)
    for fd in (0, 1, 2):
        os.dup2(null, fd)
    s = socket.socket(socket.AF_UNIX)
    s.connect(ENDPOINT)
    s.sendall(b'ready')
    while True:
        b = s.recv(1)
        if not b:
            os._exit(0)
        s.sendall(b'!')
''')

# One generation of a tagged lineage. Every generation shares one socket;
# its EOF on the test side proves that no generation is left.
LINEAGE = textwrap.dedent('''\
    import os, signal, struct, sys
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    fd = int(sys.argv[1])
    os.write(fd, b'R' + struct.pack('<i', os.getpid()))
    while True:
        c = os.read(fd, 1)
        if not c:
            os._exit(0)
        if c == b'f':
            if os.fork():
                os._exit(0)
            os.setsid()
            os.write(fd, b'R' + struct.pack('<i', os.getpid()))
        elif c == b'x':
            os.execv(sys.executable, [sys.executable, '-c', os.environ['NOSTRC_LINEAGE_SRC'], str(fd)])
''')


def root_script(child, action):
    return textwrap.dedent('''\
        import os, signal, subprocess, sys
        def stop(*_):
            os.write(1, b'cancel final \\xff\\n')
            sys.exit(19)
        signal.signal(signal.SIGTERM, stop)
        subprocess.Popen([sys.executable, '-c', CHILD]).wait()
        WAIT
        os.write(1, b'final bytes \\xff\\n')
        sys.exit(EXIT)
    ''').replace('CHILD', repr(child)).replace('EXIT', '23' if action == 'failure' else '0').replace(
        'WAIT', 'signal.pause()' if action in ('cancel', 'supervisorkill') else 'sys.stdin.buffer.read(1)')


def wait_exit(pid, version):
    """kqueue NOTE_EXIT for exactly (pid, version); True once it has exited."""
    kernel = custody.Kernel()
    queue = select.kqueue()
    try:
        events = queue.control([select.kevent(pid, filter=select.KQ_FILTER_PROC,
                                flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                                fflags=select.KQ_NOTE_EXIT)], 1, 0)
        if events and events[0].flags & select.KQ_EV_ERROR:
            return True
        current = kernel.version(pid)
        if current is None or current[0] != version:
            return True
        return bool(queue.control(None, 1, GUARD))
    finally:
        queue.close()


class Lineage:
    """A tagged process lineage driven by commands from the test."""

    def __init__(self, test, tag):
        self.kernel = custody.Kernel()
        self.mine, theirs = socket.socketpair()
        self.mine.settimeout(GUARD)
        env = dict(os.environ, NOSTRC_LINEAGE_SRC=LINEAGE)
        pid = os.fork()
        if pid == 0:
            try:
                self.mine.close()
                os.set_inheritable(theirs.fileno(), True)
                custody.apply_tag(self.kernel, tag)
                os.execve(sys.executable, [sys.executable, '-c', LINEAGE, str(theirs.fileno())], env)
            finally:
                os._exit(125)
        theirs.close()
        self.first = pid  # the test's direct child until it is reaped
        self.pid = self.ready()
        test.addCleanup(self.close)

    def ready(self):
        message = b''
        while len(message) < 5:
            chunk = self.mine.recv(5 - len(message))
            if not chunk:
                raise AssertionError('lineage closed before readiness')
            message += chunk
        assert message[:1] == b'R', message
        return struct.unpack('<i', message[1:])[0]

    def version(self):
        return self.kernel.version(self.pid)[0]

    def fork_and_exit(self):
        """Current generation forks the next one and exits; both observed."""
        old, version = self.pid, self.version()
        self.mine.sendall(b'f')
        self.pid = self.ready()
        assert wait_exit(old, version), 'old generation did not exit'
        if old == self.first:
            os.waitpid(old, 0)  # reap our own direct child: it is now gone
            self.first = None
        return old

    def exec_same(self):
        self.mine.sendall(b'x')
        assert self.ready() == self.pid

    def eof(self):
        return self.mine.recv(1) == b''

    def close(self):
        self.mine.close()
        if self.first is not None:
            os.waitpid(self.first, 0)


@unittest.skipUnless(sys.platform == 'darwin', 'native macOS custody; other platforms run the refusal test')
class DarwinDiscoveryBarrierTests(unittest.TestCase):
    """Deterministic fork/exec/exit races at the barrier's exact race points."""

    def setUp(self):
        self.kernel = custody.Kernel()
        self.tag = custody.Tag()

    def test_fork_after_snapshot_and_parent_exit_is_found_by_second_snapshot(self):
        lineage = Lineage(self, self.tag)
        parent = lineage.pid
        seen = []

        def observe(event, **info):
            if event == 'snapshot0' and not seen:
                seen.append(('fork', lineage.fork_and_exit()))
            elif event == 'classified':
                seen.append((info['pass_no'], info['pid'], info['state']))

        evidence = {}
        members = custody.scan(self.kernel, self.tag, time.monotonic() + GUARD, observe=observe,
                               evidence=evidence)
        child = lineage.pid
        first_round = []
        for entry in seen[1:]:
            if entry[0] == 1 and first_round and first_round[-1][0] == 2:
                break  # a later (rescanned) round begins
            first_round.append(entry)
        pass1 = {pid: state for p, pid, state in first_round if p == 1}
        pass2 = {pid: state for p, pid, state in first_round if p == 2}
        # A single snapshot would have concluded empty: the parent is gone
        # and the child was never in it. The second snapshot finds it.
        self.assertNotIn(child, pass1)
        self.assertNotEqual(pass1.get(parent), 'member')
        self.assertEqual(pass2.get(child), 'member')
        self.assertEqual(members, {child: lineage.version()})
        self.assertEqual(custody.reap(self.kernel, self.tag, time.monotonic() + GUARD, {}), None)
        self.assertTrue(lineage.eof())

    def test_member_exiting_after_second_snapshot_makes_round_uncertain(self):
        lineage = Lineage(self, self.tag)
        state = {'pass': 0, 'g1': None, 'done': False}

        def observe(event, **info):
            if event == 'snapshot0' and state['pass'] == 0:
                state['pass'] = 1
                lineage.fork_and_exit()          # G0 -> G1, G1 not in S0
                state['g1'] = lineage.pid
            elif event == 'snapshot1':
                state['pass'] = 2
            elif (event == 'classify' and state['pass'] == 2 and info['pid'] == state['g1']
                  and not state['done']):
                state['done'] = True
                lineage.fork_and_exit()          # G1 -> G2 while G1 is classified

        members, certain = custody.scan_round(self.kernel, self.tag, observe=observe)
        self.assertTrue(state['done'])
        self.assertEqual(members, {})
        self.assertFalse(certain, 'an uncertain round must never justify an empty custody scan')
        members = custody.scan(self.kernel, self.tag, time.monotonic() + GUARD)
        self.assertEqual(members, {lineage.pid: lineage.version()})
        evidence = {}
        custody.reap(self.kernel, self.tag, time.monotonic() + GUARD, evidence)
        self.assertTrue(lineage.eof())

    def test_rolling_lineage_across_reap_rounds_is_fully_reaped(self):
        lineage = Lineage(self, self.tag)
        steps = {'left': 6}

        def observe(event, **info):
            if event == 'snapshot0' and steps['left']:
                steps['left'] -= 1
                lineage.fork_and_exit()
            elif event == 'snapshot1' and steps['left']:
                steps['left'] -= 1
                lineage.fork_and_exit()

        evidence = {}
        custody.reap(self.kernel, self.tag, time.monotonic() + GUARD, evidence, observe=observe)
        self.assertEqual(steps['left'], 0)
        self.assertTrue(evidence['killed'])
        self.assertTrue(lineage.eof(), 'a lineage generation survived the reap')
        self.assertEqual(custody.scan(self.kernel, self.tag, time.monotonic() + GUARD), {})

    def test_exec_during_classification_is_unstable_then_reclassified(self):
        lineage = Lineage(self, self.tag)
        before = lineage.version()
        state = {'pass': 1, 'done': False}

        def observe(event, **info):
            if event == 'snapshot1':
                state['pass'] = 2
            elif event == 'classify' and info['pid'] == lineage.pid and not state['done']:
                state['done'] = True
                lineage.exec_same()
            elif event == 'classified' and info['pid'] == lineage.pid and info['pass_no'] == 1:
                state.setdefault('pass1', info['state'])

        members = custody.scan(self.kernel, self.tag, time.monotonic() + GUARD, observe=observe)
        self.assertEqual(state['pass1'], 'unstable')
        self.assertNotEqual(lineage.version(), before)
        self.assertEqual(members, {lineage.pid: lineage.version()})
        custody.reap(self.kernel, self.tag, time.monotonic() + GUARD, {})
        self.assertTrue(lineage.eof())

    def test_zombie_member_dead_before_second_snapshot_does_not_block_certainty(self):
        read_fd, write_fd = os.pipe()
        pid = os.fork()
        if pid == 0:
            try:
                os.close(read_fd)
                custody.apply_tag(self.kernel, self.tag)
            finally:
                os._exit(0)  # exits after tagging; stays a zombie until reaped
        os.close(write_fd)
        self.assertEqual(os.read(read_fd, 1), b'')  # child closed: exited
        os.close(read_fd)
        try:
            self.assertIsNotNone(self.kernel.zombie(pid))
            # scan() returns only from a certain round: a zombie seen dead in
            # pass 1 must not keep every round uncertain.
            self.assertEqual(custody.scan(self.kernel, self.tag, time.monotonic() + GUARD), {})
            self.assertEqual(custody.classify(self.kernel, self.tag, pid)[0], 'zombie')
        finally:
            os.waitpid(pid, 0)

    def test_foreign_profiles_and_stale_tokens_are_not_members(self):
        code = "import sys; print('ready', flush=True); sys.stdin.read()"
        allow_all = subprocess.Popen(['sandbox-exec', '-p', '(version 1)(allow default)', sys.executable,
                                      '-c', code], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        prefix = subprocess.Popen(['sandbox-exec', '-p',
            '(version 1)(allow default)(deny mach-lookup (global-name-prefix "org.nostrc.custody."))',
            sys.executable, '-c', code], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            for proc in (allow_all, prefix):
                self.assertEqual(proc.stdout.readline(), b'ready\n')  # sandboxed and exec'd
                self.assertEqual(self.kernel.check(proc.pid, b'org.nostrc.custody.probe'), 1 if proc is prefix else 0)
                self.assertEqual(custody.classify(self.kernel, self.tag, proc.pid)[0], 'other')
            version = self.kernel.version(prefix.pid)[0]
            self.assertEqual(self.kernel.signal(prefix.pid, version + 1, signal.SIGCONT), 3)  # ESRCH
            self.assertEqual(self.kernel.signal(prefix.pid, version, signal.SIGCONT), 0)
        finally:
            for proc in (allow_all, prefix):
                proc.communicate(b'', timeout=GUARD)
        self.assertEqual((allow_all.returncode, prefix.returncode), (0, 0))


@unittest.skipUnless(sys.platform == 'darwin', 'native macOS custody; other platforms run the refusal test')
class DarwinCustodySupervisorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nostrc-darwin-custody-')
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.runs = 0
        if os.environ.get('NOSTRC_CUSTODY_TEST_EVIDENCE'):
            destination = Path(os.environ['NOSTRC_CUSTODY_TEST_EVIDENCE']) / (self._testMethodName + '-' + uuid.uuid4().hex)
            self.addCleanup(shutil.copytree, self.work, destination)

    def env(self, **extra):
        env = dict(os.environ)
        self.runs += 1
        env['NOSTRC_DARWIN_CUSTODY_EVIDENCE'] = str(self.work / f'custody-{self.runs}.json')
        env.update(extra)
        return env

    def evidence(self, run=None):
        return json.loads((self.work / f'custody-{run or self.runs}.json').read_text())

    def endpoint(self):
        path = '/tmp/nostrc-dc-' + uuid.uuid4().hex[:16]
        server = socket.socket(socket.AF_UNIX)
        server.bind(path)
        self.addCleanup(os.unlink, path)
        self.addCleanup(server.close)
        server.listen(4)
        server.settimeout(GUARD)
        return path, server

    def accept_ready(self, server):
        peer, _ = server.accept()
        self.addCleanup(peer.close)  # fixture release only, never a kill approximation
        peer.settimeout(GUARD)
        self.assertEqual(peer.recv(5), b'ready')
        return peer

    def siblings(self):
        """Unrelated processes: plain, and inside an adversarial foreign sandbox."""
        code = "import sys; print('ready', flush=True); print(sys.stdin.readline().strip(), flush=True); sys.stdin.read()"
        foreign = '(version 1)(allow default)(deny mach-lookup (global-name-prefix "org.nostrc.custody."))'
        procs = [subprocess.Popen(argv + [sys.executable, '-u', '-c', code], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, start_new_session=True)
                 for argv in ([], ['sandbox-exec', '-p', foreign])]
        for proc in procs:
            self.addCleanup(proc.communicate, timeout=GUARD)
            self.assertEqual(proc.stdout.readline(), b'ready\n')
        return procs

    def assert_siblings_alive(self, procs):
        for proc in procs:
            proc.stdin.write(b'sibling survived\n')
            proc.stdin.flush()
            self.assertEqual(proc.stdout.readline(), b'sibling survived\n')
            self.assertIsNone(proc.poll())

    def run_helper(self, *argv, **env):
        result = subprocess.run([sys.executable, str(HELPER), *argv], capture_output=True,
                                timeout=GUARD, env=self.env(**env))
        (self.work / f'run-{self.runs}.stdout').write_bytes(result.stdout)
        (self.work / f'run-{self.runs}.stderr').write_bytes(result.stderr)
        return result

    def test_success_nonzero_signal_and_raw_bytes_are_unchanged(self):
        ok = self.run_helper(sys.executable, '-c', "import os; os.write(1, b'raw \\xff\\x00 bytes\\n')")
        self.assertEqual((ok.returncode, ok.stdout, ok.stderr), (0, b'raw \xff\x00 bytes\n', b''))
        evidence = self.evidence()
        self.assertTrue(evidence['cleanup_complete'])
        self.assertFalse(evidence['complete'])
        self.assertFalse(evidence['untraced_profile_parity'])
        failed = self.run_helper(sys.executable, '-c', 'import sys; sys.exit(23)')
        self.assertEqual(failed.returncode, 23, failed.stderr)
        self.assertTrue(self.evidence()['cleanup_complete'])
        aborted = self.run_helper(sys.executable, '-c', 'import os, signal; os.kill(os.getpid(), signal.SIGABRT)')
        self.assertEqual(aborted.returncode, 128 + signal.SIGABRT, aborted.stderr)

    def tree(self, action):
        path, server = self.endpoint()
        procs = self.siblings()
        root = root_script(ESCAPE.replace('ENDPOINT', repr(path)), action)
        tag_file = self.work / 'tag.json'
        process = subprocess.Popen([sys.executable, str(HELPER), sys.executable, '-c', root],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env=self.env(NOSTRC_DARWIN_CUSTODY_TAG_FILE=str(tag_file)),
                                   start_new_session=True)
        try:
            peer = self.accept_ready(server)
            if action == 'supervisorkill':
                process.kill()  # the test's own direct, unreaped child
                # The surviving tagged root still holds the output pipes.
                self.assertEqual(process.wait(timeout=GUARD), -signal.SIGKILL)
                output = errors = bytes()
            elif action == 'cancel':
                process.send_signal(signal.SIGTERM)  # our direct, unreaped child
                process.send_signal(signal.SIGINT)
                output, errors = process.communicate(timeout=GUARD)
                self.assertEqual(process.returncode, 130, errors)
                self.assertEqual(output, b'cancel final \xff\n')
            else:
                output, errors = process.communicate(b'1', timeout=GUARD)
                self.assertEqual(process.returncode, 23 if action == 'failure' else 0, errors)
                self.assertEqual(output, b'final bytes \xff\n')
            (self.work / 'final.stdout').write_bytes(output)
            (self.work / 'final.stderr').write_bytes(errors)
            if action == 'supervisorkill':
                # Documented limit: nothing reaps when the supervisor is SIGKILLed.
                peer.sendall(b'?')
                self.assertEqual(peer.recv(1), b'!')
                self.assertFalse((self.work / f'custody-{self.runs}.json').exists())
                self.assertEqual(tag_file.stat().st_mode & 0o777, 0o600)
                recovery = self.run_helper('--reap-tag-file', str(tag_file))
                self.assertEqual(recovery.returncode, 0, recovery.stderr)
                output, errors = process.communicate(timeout=GUARD)  # EOF: holders gone
                (self.work / 'final.stdout').write_bytes(output)
                (self.work / 'final.stderr').write_bytes(errors)
            evidence = self.evidence()
            self.assertTrue(evidence['cleanup_complete'], evidence)
            self.assertTrue(evidence['killed'], 'escaped member was not found by the tag scan')
            if action != 'supervisorkill':
                self.assertEqual(evidence['cancelled'], action == 'cancel')
            self.assertEqual(peer.recv(1), b'', 'escaped descendant survived custody cleanup')
            self.assert_siblings_alive(procs)
        finally:
            if process.returncode is None:
                process.kill()
                process.communicate(timeout=GUARD)

    def test_normal_exit_cleans_escaped_term_ignoring_child(self):
        self.tree('normal')

    def test_nonzero_exit_cleans_escaped_term_ignoring_child(self):
        self.tree('failure')

    def test_cancellation_cleans_tree_and_retains_final_bytes(self):
        self.tree('cancel')

    def test_supervisor_sigkill_leaves_tree_until_tag_recovery(self):
        self.tree('supervisorkill')

    def test_adapter_parent_death_kills_escaped_tree(self):
        path, server = self.endpoint()
        procs = self.siblings()
        evidence_path = self.work / 'parentkill-custody.json'
        root = root_script(ESCAPE.replace('ENDPOINT', repr(path)), 'cancel')
        parent = textwrap.dedent('''\
            import os, subprocess, sys
            r, w = os.pipe()
            p = subprocess.Popen([sys.executable, HELPER, '--parent-ready-fd', str(r), '--parent-pid',
                                  str(os.getpid()), sys.executable, '-c', ROOT], pass_fds=(r,))
            os.close(r)
            os.write(w, b'1')
            os.close(w)
            p.wait()
        ''').replace('HELPER', repr(str(HELPER))).replace('ROOT', repr(root))
        env = dict(os.environ, NOSTRC_DARWIN_CUSTODY_EVIDENCE=str(evidence_path))
        process = subprocess.Popen([sys.executable, '-c', parent], env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        peer = self.accept_ready(server)
        process.kill()  # our direct, unreaped child (the adapter parent)
        process.communicate(timeout=GUARD)
        self.assertEqual(peer.recv(1), b'', 'escaped descendant survived adapter-parent death')
        directory = os.open(self.work, os.O_RDONLY)
        queue = select.kqueue()
        try:
            queue.control([select.kevent(directory, filter=select.KQ_FILTER_VNODE,
                           flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR, fflags=select.KQ_NOTE_WRITE)], 0)
            while not evidence_path.exists():
                self.assertTrue(queue.control(None, 1, GUARD), 'supervisor evidence never written')
        finally:
            queue.close()
            os.close(directory)
        evidence = json.loads(evidence_path.read_text())
        self.assertTrue(evidence['adapter_parent_exit'], evidence)
        self.assertTrue(evidence['cleanup_complete'], evidence)
        self.assert_siblings_alive(procs)

    def test_pending_cancel_and_closed_gate_never_launch(self):
        for cancel in (False, True):
            with self.subTest(cancel=cancel):
                marker = self.work / f'never-executed-{cancel}'
                read_fd, write_fd = os.pipe()
                mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
                try:
                    process = subprocess.Popen([sys.executable, str(HELPER), '--parent-ready-fd', str(read_fd),
                        '--parent-pid', str(os.getpid()), sys.executable, '-c',
                        f'open({str(marker)!r}, "w").close()'], pass_fds=(read_fd,),
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=self.env())
                finally:
                    signal.pthread_sigmask(signal.SIG_SETMASK, mask)
                    os.close(read_fd)
                if cancel:
                    process.send_signal(signal.SIGTERM)  # our direct, unreaped child
                    os.write(write_fd, b'1')
                os.close(write_fd)
                output = process.communicate(timeout=GUARD)[0]
                self.assertEqual(process.returncode, 130 if cancel else 125, output)
                self.assertFalse(marker.exists())

    def test_private_resource_wrappers_refuse_before_launch(self):
        marker = self.work / 'never-executed'
        result = self.run_helper('--private-resource-root', str(self.work), sys.executable, '-c',
                                 f'open({str(marker)!r}, "w").close()')
        self.assertEqual(result.returncode, 125)
        self.assertIn(b'does not host private display/bus resources', result.stderr)
        self.assertFalse(marker.exists())

    def test_nested_custody_refuses_before_inner_target(self):
        marker = self.work / 'never-executed'
        target = [sys.executable, '-c', f'open({str(marker)!r}, "w").close()']
        result = self.run_helper(*target)
        self.assertEqual(result.returncode, 0, result.stderr)  # one level is fine
        self.assertTrue(marker.exists())
        marker.unlink()
        nested = self.run_helper(sys.executable, str(HELPER), *target)
        self.assertEqual(nested.returncode, 125, nested.stderr)
        self.assertIn(b'sandbox_init custody tag refused', nested.stderr)
        self.assertFalse(marker.exists())

    def test_parity_cost_nested_seatbelt_is_refused_inside_custody(self):
        result = self.run_helper('sandbox-exec', '-p', '(version 1)(allow default)', '/usr/bin/true')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'Operation not permitted', result.stderr)
        self.assertTrue(self.evidence()['cleanup_complete'])

    def ctest_tree(self, action):
        path, server = self.endpoint()
        procs = self.siblings()
        project = self.work / 'ctest'
        project.mkdir()
        (project / 'escape.py').write_text(ESCAPE.replace('ENDPOINT', repr(path)))
        body = 'import signal; signal.pause()' if action == 'cancel' else 'pass'
        (project / 'CMakeLists.txt').write_text(textwrap.dedent(f'''\
            cmake_minimum_required(VERSION 3.16)
            project(darwin_custody_ctest NONE)
            enable_testing()
            add_test(NAME escapes COMMAND {sys.executable} -c "import subprocess,sys; subprocess.run([sys.executable, '{project / 'escape.py'}']); {body}")
        '''))
        configure = subprocess.run(['cmake', '-S', str(project), '-B', str(project / 'build')],
                                   capture_output=True, timeout=GUARD)
        (self.work / 'cmake.log').write_bytes(configure.stdout + configure.stderr)
        self.assertEqual(configure.returncode, 0, configure.stderr)
        process = subprocess.Popen([sys.executable, str(HELPER), 'ctest', '--test-dir', str(project / 'build'),
                                    '--output-on-failure'], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env=self.env(), start_new_session=True)
        try:
            peer = self.accept_ready(server)
            if action == 'cancel':
                process.send_signal(signal.SIGTERM)  # our direct, unreaped child
                output, errors = process.communicate(timeout=GUARD)
                self.assertEqual(process.returncode, 130, errors)
            else:
                output, errors = process.communicate(timeout=GUARD)
                (self.work / 'ctest.stdout').write_bytes(output)
                (self.work / 'ctest.stderr').write_bytes(errors)
                self.assertEqual(process.returncode, 0, output + errors)
                self.assertIn(b'100% tests passed', output)
            (self.work / 'ctest.stdout').write_bytes(output)
            (self.work / 'ctest.stderr').write_bytes(errors)
            evidence = self.evidence()
            self.assertTrue(evidence['cleanup_complete'], evidence)
            self.assertTrue(evidence['killed'], evidence)
            self.assertEqual(peer.recv(1), b'', 'escaped CTest descendant survived custody cleanup')
            self.assert_siblings_alive(procs)
        finally:
            if process.returncode is None:
                process.kill()
                process.communicate(timeout=GUARD)

    def test_real_ctest_pass_leaves_no_escaped_descendant(self):
        self.ctest_tree('normal')

    def test_real_ctest_cancellation_cleans_escaped_descendant(self):
        self.ctest_tree('cancel')


class DarwinCustodyPlatformTests(unittest.TestCase):
    @unittest.skipIf(sys.platform == 'darwin', 'macOS runs the positive suites above')
    def test_other_platforms_refuse_before_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'never-executed'
            result = subprocess.run([sys.executable, str(HELPER), sys.executable, '-c',
                                     f'open({str(marker)!r}, "w").close()'], capture_output=True, timeout=GUARD)
            self.assertEqual(result.returncode, 125)
            self.assertIn(b'macOS-only', result.stderr)
            self.assertFalse(marker.exists())


if __name__ == '__main__':
    unittest.main()
