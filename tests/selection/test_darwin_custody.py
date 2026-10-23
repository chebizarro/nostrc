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


def read_control(fd):
    """All JSON lines from a helper control pipe, until EOF."""
    data = b''
    while True:
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        data += chunk
    return [json.loads(line) for line in data.decode().splitlines()]


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
                os.write(write_fd, b'T')  # acknowledged only after tagging
            finally:
                os._exit(0)  # stays a zombie until the test reaps it
        os.close(write_fd)
        tagged = os.read(read_fd, 1)
        os.close(read_fd)
        try:
            self.assertEqual(tagged, b'T', 'child was not tagged')
            queue = select.kqueue()
            try:
                queue.control([select.kevent(pid, filter=select.KQ_FILTER_PROC,
                               flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                               fflags=select.KQ_NOTE_EXIT)], 0, 0)
                self.assertTrue(queue.control(None, 1, GUARD), 'tagged child never exited')
            except ProcessLookupError:
                pass  # already exited before registration
            finally:
                queue.close()
            self.assertIsNotNone(self.kernel.zombie(pid))
            # scan() returns only from a certain round: a zombie seen dead in
            # pass 1 must not keep every round uncertain.
            self.assertEqual(custody.scan(self.kernel, self.tag, time.monotonic() + GUARD), {})
            self.assertEqual(custody.classify(self.kernel, self.tag, pid)[0], 'zombie')
        finally:
            os.waitpid(pid, 0)

    def test_zombie_identity_lookup_order_rejects_pid_reuse(self):
        """PID reuse between the two lookups cannot be forced on a real host,
        so the lookup order itself is checked with scripted kernel answers."""
        class Scripted(custody.Kernel):
            def __init__(self, answers):
                super().__init__()
                self.answers = list(answers)

            def _uniq(self, pid, zombies):
                expected, answer = self.answers.pop(0)
                assert expected == zombies, 'zombie-inclusive lookup must come first'
                return answer

        dead, reused = (7, 70), (9, 90)
        # Zombie seen, then PID not live: it was dead before the check.
        self.assertEqual(Scripted([(True, dead), (False, None)]).zombie(123), dead)
        # Inclusive lookup returned a live process (reuse): never "dead".
        self.assertIsNone(Scripted([(True, reused), (False, reused)]).zombie(123))
        self.assertIsNone(Scripted([(True, None)]).zombie(123))

    def test_await_exits_keeps_notifications_of_earlier_members(self):
        children = []
        for _ in range(2):
            child = subprocess.Popen([sys.executable, '-c', 'import sys; sys.stdin.read(1)'], stdin=subprocess.PIPE)
            self.addCleanup(child.wait, timeout=GUARD)
            children.append(child)
        members = {c.pid: self.kernel.version(c.pid)[0] for c in children}

        class ExitDuringRegistration(custody.Kernel):
            """Each member exits right after its own registration check, so the
            first one's NOTE_EXIT is pending while the second, still live, is
            registered. A registration that returns events would consume it."""
            def version(inner, pid):
                answer = super().version(pid)
                for child in children:
                    if child.pid == pid and not child.stdin.closed:
                        child.stdin.close()
                        child.wait(timeout=GUARD)  # exited (reaped by the test)
                return answer

        custody.await_exits(ExitDuringRegistration(), members, time.monotonic() + 10)

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

    def recover_bound(self, tag_line):
        """Parent-bound recovery with the tag read from the control pipe."""
        control_read, control_write = os.pipe()
        try:
            result = subprocess.run([sys.executable, str(HELPER), '--control-fd', str(control_write),
                                     '--reap-tag', tag_line['denied'], tag_line['allowed']],
                                    capture_output=True, timeout=GUARD, env=self.env(), pass_fds=(control_write,))
        finally:
            os.close(control_write)
        try:
            final = read_control(control_read)
        finally:
            os.close(control_read)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([m['event'] for m in final], ['final'])
        self.assertTrue(final[0]['cleanup_complete'], final)
        return final[0]

    def test_tag_names_reject_equal_swapped_independent_and_malformed(self):
        tag = custody.Tag()
        for denied, allowed in ((tag.denied, tag.denied), (tag.allowed, tag.allowed), (tag.allowed, tag.denied),
                                (tag.denied, tag.denied[:-1] + 'b'), ('org.nostrc.custody.x.a', tag.allowed),
                                (tag.denied + '\n', tag.allowed), (None, tag.allowed)):
            with self.subTest(denied=denied, allowed=allowed), self.assertRaises(custody.CustodyError):
                custody.Tag(denied, allowed) if denied is not None else custody.Tag.from_json(
                    json.dumps({'denied': 7, 'allowed': tag.allowed}))
        bad = self.work / 'equal-tag.json'
        bad.write_text(json.dumps({'denied': tag.denied, 'allowed': tag.denied}))
        result = self.run_helper('--reap-tag-file', str(bad))
        self.assertEqual(result.returncode, 125, result.stderr)
        evidence = self.evidence()
        self.assertFalse(evidence['cleanup_complete'])
        self.assertEqual(evidence['tag_authority'], 'untrusted-file')
        result = self.run_helper('--reap-tag', tag.denied, tag.denied)
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertFalse(self.evidence()['cleanup_complete'])

    def substitution_attack(self, substitute):
        """The target rewrites the discovered tag file, then SIGKILLs the supervisor."""
        path, server = self.endpoint()
        tag_file = self.work / 'tag.json'
        attack = textwrap.dedent('''\
            import json, os, signal, subprocess, sys
            subprocess.Popen([sys.executable, '-c', CHILD]).wait()
            with open(TAG_FILE, 'w') as handle:
                handle.write(json.dumps(SUBSTITUTE))
            os.kill(os.getppid(), signal.SIGKILL)  # attacker kills its supervisor
            signal.pause()
        ''').replace('CHILD', repr(ESCAPE.replace('ENDPOINT', repr(path)))).replace(
            'TAG_FILE', repr(str(tag_file))).replace('SUBSTITUTE', repr(substitute))
        control_read, control_write = os.pipe()
        self.addCleanup(os.close, control_read)
        try:
            process = subprocess.Popen([sys.executable, str(HELPER), '--control-fd', str(control_write),
                                        sys.executable, '-c', attack],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       env=self.env(NOSTRC_DARWIN_CUSTODY_TAG_FILE=str(tag_file)),
                                       pass_fds=(control_write,), start_new_session=True)
        finally:
            os.close(control_write)
        peer = self.accept_ready(server)
        self.assertEqual(process.wait(timeout=GUARD), -signal.SIGKILL)
        control = read_control(control_read)
        # Authoritative tag received before release; no final line: failed lane.
        self.assertEqual([m['event'] for m in control], ['tag'])
        self.assertNotEqual(json.loads(tag_file.read_text()), {'denied': control[0]['denied'],
                                                               'allowed': control[0]['allowed']})
        return peer, tag_file, control[0], process

    def test_substituted_valid_tag_file_cannot_masquerade_as_cleanup(self):
        fake = custody.Tag()
        peer, tag_file, bound, process = self.substitution_attack({'denied': fake.denied, 'allowed': fake.allowed})
        untrusted = self.run_helper('--reap-tag-file', str(tag_file))
        self.assertEqual(untrusted.returncode, 0, untrusted.stderr)
        evidence = self.evidence()
        self.assertEqual(evidence['tag_authority'], 'untrusted-file')
        # The file-based "clean" result proves nothing: the real tree lives.
        peer.sendall(b'?')
        self.assertEqual(peer.recv(1), b'!')
        recovery = self.recover_bound(bound)
        self.assertGreaterEqual(recovery['killed_count'], 2)  # attacker root and escaped child
        self.assertEqual(peer.recv(1), b'', 'escaped descendant survived parent-bound recovery')
        process.communicate(timeout=GUARD)  # holders gone: output pipes reach EOF

    def test_substituted_equal_name_tag_file_is_refused(self):
        fake = custody.Tag()
        peer, tag_file, bound, process = self.substitution_attack({'denied': fake.denied, 'allowed': fake.denied})
        untrusted = self.run_helper('--reap-tag-file', str(tag_file))
        self.assertEqual(untrusted.returncode, 125, untrusted.stderr)
        self.assertFalse(self.evidence()['cleanup_complete'])
        peer.sendall(b'?')
        self.assertEqual(peer.recv(1), b'!')
        self.recover_bound(bound)
        self.assertEqual(peer.recv(1), b'')
        process.communicate(timeout=GUARD)

    def test_planted_evidence_file_fails_the_run(self):
        evidence_path = self.work / 'planted.json'
        plant = (f'import json; json.dump({{"cleanup_complete": True, "exit_code": 0}}, '
                 f'open({str(evidence_path)!r}, "x"))')
        control_read, control_write = os.pipe()
        try:
            result = subprocess.run([sys.executable, str(HELPER), '--control-fd', str(control_write),
                                     sys.executable, '-c', plant], capture_output=True, timeout=GUARD,
                                    env=dict(os.environ, NOSTRC_DARWIN_CUSTODY_EVIDENCE=str(evidence_path)),
                                    pass_fds=(control_write,))
        finally:
            os.close(control_write)
        control = read_control(control_read)
        os.close(control_read)
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertEqual([m['event'] for m in control], ['tag', 'final'])
        self.assertFalse(control[1]['cleanup_complete'])
        self.assertIn('evidence_file_error', control[1])
        self.assertEqual(json.loads(evidence_path.read_text()), {'cleanup_complete': True, 'exit_code': 0})

    def test_target_sees_no_custody_paths_or_control_fd(self):
        control_read, control_write = os.pipe()
        probe = (f'import os, json; print(json.dumps(sorted(k for k in os.environ if k.startswith("NOSTRC_DARWIN_CUSTODY_"))))\n'
                 f'try:\n    os.fstat({control_write}); print("control-open")\nexcept OSError:\n    print("control-closed")')
        try:
            result = subprocess.run([sys.executable, str(HELPER), '--control-fd', str(control_write),
                                     sys.executable, '-c', probe], capture_output=True, timeout=GUARD,
                                    env=self.env(NOSTRC_DARWIN_CUSTODY_TAG_FILE=str(self.work / 't.json')),
                                    pass_fds=(control_write,))
        finally:
            os.close(control_write)
        control = read_control(control_read)
        os.close(control_read)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [b'[]', b'control-closed'])
        self.assertEqual([m['event'] for m in control], ['tag', 'final'])

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
        control_read, control_write = os.pipe()
        self.addCleanup(os.close, control_read)
        try:
            process = subprocess.Popen([sys.executable, str(HELPER), '--control-fd', str(control_write),
                                        sys.executable, '-c', root],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       env=self.env(), pass_fds=(control_write,), start_new_session=True)
        finally:
            os.close(control_write)
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
            control = read_control(control_read)
            self.assertEqual(control[0]['event'], 'tag')
            if action == 'supervisorkill':
                # Documented limit: nothing reaps when the supervisor is SIGKILLed,
                # and no final line means the lane stays failed.
                self.assertEqual([m['event'] for m in control], ['tag'])
                peer.sendall(b'?')
                self.assertEqual(peer.recv(1), b'!')
                self.assertFalse((self.work / f'custody-{self.runs}.json').exists())
                recovery = self.recover_bound(control[0])
                self.assertEqual(recovery['tag_authority'], 'parent-bound')
                output, errors = process.communicate(timeout=GUARD)  # EOF: holders gone
                (self.work / 'final.stdout').write_bytes(output)
                (self.work / 'final.stderr').write_bytes(errors)
            evidence = self.evidence()
            self.assertTrue(evidence['cleanup_complete'], evidence)
            self.assertTrue(evidence['killed'], 'escaped member was not found by the tag scan')
            if action != 'supervisorkill':
                self.assertEqual(evidence['cancelled'], action == 'cancel')
                self.assertEqual([m['event'] for m in control], ['tag', 'final'])
                self.assertTrue(control[1]['cleanup_complete'])
                self.assertEqual(control[1]['exit_code'], process.returncode)
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

    def seal_helper(self, root, **popen):
        read_fd, write_fd = os.pipe()
        try:
            process = subprocess.Popen([sys.executable, str(HELPER), sys.executable, '-c', root],
                                       pass_fds=(write_fd,), env=self.env(NOSTRC_NATIVE_SEAL_FD=str(write_fd)),
                                       start_new_session=True, **popen)
        finally:
            os.close(write_fd)
        self.addCleanup(os.close, read_fd)
        return process, read_fd

    def test_native_seal_fd_is_retained_only_by_the_root(self):
        root = textwrap.dedent('''\
            import os, sys
            fd = int(os.environ['NOSTRC_NATIVE_SEAL_FD'])
            os.write(fd, b'anchor-digest')
            os.close(fd)
            sys.stdin.buffer.read(1)
        ''')
        process, read_fd = self.seal_helper(root, stdin=subprocess.PIPE)
        try:
            data = b''
            while True:
                chunk = os.read(read_fd, 64)  # EOF while the supervisor still runs
                if not chunk:
                    break
                data += chunk
            self.assertEqual(data, b'anchor-digest')
            self.assertIsNone(process.poll(), 'EOF must not depend on supervisor exit')
            process.communicate(b'1', timeout=GUARD)
            self.assertEqual(process.returncode, 0)
            self.assertTrue(self.evidence()['cleanup_complete'])
        finally:
            if process.returncode is None:
                process.kill()
                process.communicate(timeout=GUARD)

    def test_native_seal_eof_follows_the_cleanup_barrier(self):
        path, server = self.endpoint()
        root = textwrap.dedent('''\
            import os, subprocess, sys
            fd = int(os.environ['NOSTRC_NATIVE_SEAL_FD'])
            # A careless root lets an escaped descendant inherit the seal FD.
            subprocess.Popen([sys.executable, '-c', CHILD], pass_fds=(fd,)).wait()
            os.write(fd, b'anchor-digest')
        ''').replace('CHILD', repr(ESCAPE.replace('ENDPOINT', repr(path))))
        process, read_fd = self.seal_helper(root)
        try:
            peer = self.accept_ready(server)
            self.assertEqual(process.wait(timeout=GUARD), 0)
            self.assertTrue(self.evidence()['cleanup_complete'])
            # The escaped holder was killed before the supervisor exited, so
            # the anchor and EOF are already available: no wait is needed.
            self.assertEqual(select.select([read_fd], [], [], 0)[0], [read_fd])
            self.assertEqual(os.read(read_fd, 64), b'anchor-digest')
            self.assertEqual(os.read(read_fd, 64), b'')
            self.assertEqual(peer.recv(1), b'')
        finally:
            if process.returncode is None:
                process.kill()
                process.wait(timeout=GUARD)

    def test_invalid_seal_fd_refuses_before_launch(self):
        marker = self.work / 'never-executed'
        result = self.run_helper(sys.executable, '-c', f'open({str(marker)!r}, "w").close()',
                                 NOSTRC_NATIVE_SEAL_FD='987')
        self.assertEqual(result.returncode, 125, result.stderr)
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


SAN_FIXTURE = r'''
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

__attribute__((noinline)) static int heap_overflow(int n) {
    char *p = malloc(8);
    volatile char x = p[n + 8];
    free(p);
    return x;
}

__attribute__((noinline)) static int signed_overflow(int n) {
    volatile int big = INT_MAX;
    return big + n;
}

static void on_term(int s) {
    (void)s;
    static const char m[] = "cancel final \xff\n";
    write(1, m, sizeof m - 1);
    _exit(19);
}

/* Instrumented escaped descendant: double fork, setsid, TERM/HUP ignored,
 * stdio detached; it holds every inherited FD (including a seal FD). */
static void spawn_escapee(const char *endpoint) {
    pid_t p = fork();
    if (p == 0) {
        signal(SIGTERM, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
        setsid();
        if (fork())
            _exit(0);
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, endpoint, sizeof a.sun_path - 1);
        if (connect(s, (struct sockaddr *)&a, sizeof a) != 0)
            _exit(3);
        write(s, "ready", 5);
        char c;
        while (read(s, &c, 1) == 1)
            write(s, "!", 1);
        _exit(0);
    }
    int status;
    waitpid(p, &status, 0);
}

int main(int argc, char **argv) {
    const char *mode = argv[1];
    if (!strcmp(mode, "ok")) {
        write(1, "san ok \xff\n", 9);
        return 0;
    }
    if (!strcmp(mode, "heap"))
        return heap_overflow(argc);
    if (!strcmp(mode, "ubsan"))
        return signed_overflow(argc) == 0;
    if (!strcmp(mode, "leak")) {
        void *p = malloc(1234);
        ((volatile char *)p)[0] = 1;
        p = NULL;
        return 0;
    }
    /* tree ENDPOINT normal|cancel EXIT */
    signal(SIGTERM, on_term);
    spawn_escapee(argv[2]);
    const char *seal = getenv("NOSTRC_NATIVE_SEAL_FD");
    if (seal)
        write(atoi(seal), "anchor-digest", 13);
    char c;
    if (!strcmp(argv[3], "cancel"))
        pause();
    else
        read(0, &c, 1);
    write(1, "final bytes \xff\n", 14);
    return atoi(argv[4]);
}
'''

# CI-authored macOS sanitizer environment (nostrc-ci.yml / libgo-ci.yml macOS
# override) plus the workflow's strict UBSan halt; nothing is suppressed.
AUTHORED_SANITIZER_ENV = {'ASAN_OPTIONS': 'detect_leaks=0:strict_string_checks=1',
                          'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}


@unittest.skipUnless(sys.platform == 'darwin', 'native macOS custody; other platforms run the refusal test')
class DarwinCustodyInstrumentedTests(DarwinCustodySupervisorTests):
    """Tiny ASan/UBSan compatibility proof, NOT production root/profile parity.

    One fixture built with the project's authored flags (CMakeLists.txt
    -fsanitize=address -fno-omit-frame-pointer -fsanitize=undefined) runs both
    directly and under custody; results must agree. Inherits only helpers."""

    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='nostrc-darwin-san-')
        source = Path(cls.build.name) / 'san_fixture.c'
        source.write_text(SAN_FIXTURE)
        cls.fixture = Path(cls.build.name) / 'san_fixture'
        subprocess.run(['cc', '-g', '-O0', '-fsanitize=address', '-fno-omit-frame-pointer',
                        '-fsanitize=undefined', str(source), '-o', str(cls.fixture)],
                       check=True, capture_output=True, timeout=GUARD)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def san_env(self, **extra):
        return self.env(**dict(AUTHORED_SANITIZER_ENV, **extra))

    @staticmethod
    def as_helper_status(returncode):
        """The helper's documented encoding of a signal death: 128+N."""
        return 128 - returncode if returncode < 0 else returncode

    def both(self, *argv, **extra):
        direct = subprocess.run([str(self.fixture), *argv], capture_output=True, timeout=GUARD,
                                env=dict(os.environ, **dict(AUTHORED_SANITIZER_ENV, **extra)))
        held = subprocess.run([sys.executable, str(HELPER), str(self.fixture), *argv], capture_output=True,
                              timeout=GUARD, env=self.san_env(**extra))
        for label, result in (('direct', direct), ('custody', held)):
            (self.work / f'{argv[0]}-{label}.stdout').write_bytes(result.stdout)
            (self.work / f'{argv[0]}-{label}.stderr').write_bytes(result.stderr)
        self.assertTrue(self.evidence()['cleanup_complete'], self.evidence())
        return direct, held

    def test_instrumented_clean_run_matches_direct_run(self):
        direct, held = self.both('ok')
        self.assertEqual((direct.returncode, direct.stdout, direct.stderr), (0, b'san ok \xff\n', b''))
        self.assertEqual((held.returncode, held.stdout, held.stderr), (0, b'san ok \xff\n', b''))

    def test_asan_finding_fails_identically_and_is_symbolized(self):
        direct, held = self.both('heap')
        self.assertNotEqual(direct.returncode, 0)
        self.assertEqual(held.returncode, self.as_helper_status(direct.returncode))
        for result in (direct, held):
            self.assertIn(b'ERROR: AddressSanitizer: heap-buffer-overflow', result.stderr)
            self.assertIn(b'heap_overflow', result.stderr)  # symbolized under custody too

    def test_strict_ubsan_finding_fails_identically(self):
        direct, held = self.both('ubsan')
        self.assertNotEqual(direct.returncode, 0)
        self.assertEqual(held.returncode, self.as_helper_status(direct.returncode))
        for result in (direct, held):
            self.assertIn(b'runtime error: signed integer overflow', result.stderr)

    def test_leak_detection_is_a_toolchain_limit_not_a_custody_effect(self):
        # Apple clang's ASan refuses detect_leaks on macOS; the authored macOS
        # policy therefore sets detect_leaks=0. Custody must not change that.
        direct, held = self.both('leak', ASAN_OPTIONS='detect_leaks=1:strict_string_checks=1')
        for result in (direct, held):
            self.assertIn(b'detect_leaks is not supported on this platform', result.stderr)
        self.assertNotEqual(direct.returncode, 0)
        self.assertEqual(held.returncode, self.as_helper_status(direct.returncode))

    def instrumented_tree(self, action):
        path, server = self.endpoint()
        procs = self.siblings()
        read_fd, write_fd = os.pipe()
        self.addCleanup(os.close, read_fd)
        try:
            process = subprocess.Popen([sys.executable, str(HELPER), str(self.fixture), 'tree', path, action,
                                        '23' if action == 'normal' else '0'],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       pass_fds=(write_fd,), start_new_session=True,
                                       env=self.san_env(NOSTRC_NATIVE_SEAL_FD=str(write_fd)))
        finally:
            os.close(write_fd)
        try:
            peer = self.accept_ready(server)
            if action == 'cancel':
                process.send_signal(signal.SIGTERM)  # our direct, unreaped child
                output, errors = process.communicate(timeout=GUARD)
                self.assertEqual(process.returncode, 130, errors)
                self.assertEqual(output, b'cancel final \xff\n')
            else:
                output, errors = process.communicate(b'1', timeout=GUARD)
                self.assertEqual(process.returncode, 23, errors)
                self.assertEqual(output, b'final bytes \xff\n')
            (self.work / 'tree.stdout').write_bytes(output)
            (self.work / 'tree.stderr').write_bytes(errors)
            self.assertNotIn(b'Sanitizer', errors)
            evidence = self.evidence()
            self.assertTrue(evidence['cleanup_complete'], evidence)
            self.assertTrue(evidence['killed'], evidence)
            # The escaped instrumented holder of the seal FD died before the
            # supervisor exited: anchor and EOF are available at once.
            self.assertEqual(select.select([read_fd], [], [], 0)[0], [read_fd])
            self.assertEqual(os.read(read_fd, 64), b'anchor-digest')
            self.assertEqual(os.read(read_fd, 64), b'')
            self.assertEqual(peer.recv(1), b'', 'escaped instrumented descendant survived cleanup')
            self.assert_siblings_alive(procs)
        finally:
            if process.returncode is None:
                process.kill()
                process.communicate(timeout=GUARD)

    def test_instrumented_cancellation_cleans_escaped_descendant_and_seal(self):
        self.instrumented_tree('cancel')

    def test_instrumented_nonzero_exit_cleans_escaped_descendant_and_seal(self):
        self.instrumented_tree('normal')


if sys.platform == 'darwin':
    # Reuse fixture helpers only; never rerun the supervisor suite here.
    for _name in [n for n in vars(DarwinCustodySupervisorTests) if n.startswith('test_')]:
        setattr(DarwinCustodyInstrumentedTests, _name, None)


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
