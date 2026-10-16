"""Real shared-engine command/profile execution inside the owned Linux domain."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import test_selection as selection
from test_selection_custody import validate_lifecycle


class ActualContainedEngine(unittest.TestCase):
    def setUp(self):
        self.image = os.environ['NOSTRC_CUSTODY_IMAGE']
        self.root = Path(tempfile.mkdtemp(prefix='nostrc-contained-engine-')).resolve()
        print('retained engine evidence: ' + str(self.root), flush=True)
        self.source = self.root / 'source'
        (self.source / 'scripts').mkdir(parents=True)
        (self.source / 'app').mkdir()
        (self.source / 'tests/selection').mkdir(parents=True)
        for name in ('test-plan.py', 'test_selection.py', 'test_selection_realization.py',
                     'test_selection_custody.py', 'contained-runner.py'):
            shutil.copyfile(ROOT / 'scripts' / name, self.source / 'scripts' / name)
        (self.source / '.gitignore').write_text('__pycache__/\n')
        (self.source / 'app/check.py').write_text("""import os, pathlib, subprocess, sys
assert os.environ['DISPLAY'] and os.environ['DBUS_SESSION_BUS_ADDRESS']
subprocess.Popen([sys.executable, '-c', 'import signal; signal.signal(signal.SIGTERM,signal.SIG_IGN); signal.pause()'], start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
pathlib.Path(sys.argv[1]).write_text('actual command')
os.write(1, b'engine ready \\xff\\n')
if os.environ.get('FIXTURE_MODE') == 'hold':
    import signal, socket
    listener = socket.socket(socket.AF_UNIX)
    listener.bind('/work/ready-pending.sock')
    listener.listen(1)
    os.rename('/work/ready-pending.sock', '/work/ready.sock')
    connection, _ = listener.accept()
    connection.sendall(b'ready-after-raw')
    connection.close()
    signal.pause()
sys.exit(19 if os.environ.get('FIXTURE_MODE') == 'fail' else 0)
""")
        profile = dict(platform='linux', architecture='native', compiler='unavailable-unused-compiler',
            build_type='Debug', instrumentation='none', source_dir='.', configure_args=[], environment={},
            capabilities=['private_bus', 'display'], parallelism=1, timeout_seconds=30, setup='contained fixture',
            local_availability='host_or_container', contexts=['local'], execution_domain='prepared_peer_linux_v1', prepared_peers=[])
        self.policy = dict(schema_version=1, nodes={'app': dict(owner_paths=['app'], depends_on=[], rationale='fixture')},
            path_rules=[dict(prefix='app/', nodes=['app'], effect='impact', reason='fixture')]
                + [dict(prefix=p, nodes=[], effect='global', reason='contract') for p in ('scripts/', 'tests/', '.gitignore')],
            test_overrides=[], release_components={}, profiles={'contained.command': profile},
            checks={'actual': dict(kind='command', profiles=['contained.command'], nodes=['app'], cost='ordinary',
                requirement='blocking', expected_outcome='pass', command=['python3', '{source}/app/check.py', '{build}/proof.txt'],
                working_directory='{source}', environment={}, runtime_inputs=['{source}/app/check.py'],
                build_targets=[], required_evidence=['proof.txt'])})
        (self.source / 'tests/selection/inventory.json').write_text(json.dumps(self.policy))
        self.git('init', '-q')
        self.git('config', 'user.name', 'Contained fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.git('add', '.')
        self.git('-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'exact fixture')
        sha = self.git('rev-parse', 'HEAD').decode().strip()
        self.plan = selection.make_plan(self.policy, [], head=sha, mode='broad', profiles=['contained.command'])
        self.plan_path = self.root / 'plan.json'
        self.plan_path.write_text(json.dumps(self.plan))

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.source), *args], stderr=subprocess.PIPE)

    def test_actual_engine_with_private_resources_and_final_host_attestation(self):
        process = subprocess.run([sys.executable, str(ROOT / 'scripts/contained-gate.py'), 'run',
            '--source-root', str(self.source), '--evidence-dir', str(self.root / 'evidence'), '--image', self.image,
            '--plan', str(self.plan_path), '--profile', 'contained.command', '--timeout', '45'],
            capture_output=True, timeout=120)
        self.assertEqual(process.returncode, 0, (process.stdout + process.stderr).decode(errors='replace'))
        receipt_path = Path(process.stdout.decode().strip().splitlines()[-1])
        receipt = json.loads(receipt_path.read_text())
        validate_lifecycle(receipt['contained_lifecycle'], self.plan, 'contained.command',
                           self.policy['profiles']['contained.command'], receipt['profile_fingerprint'])
        self.assertTrue(receipt['diagnostic_eligible'])
        self.assertFalse(receipt['complete_gate_eligible'])
        self.assertTrue(all(x['status'] == 'not_executed' for x in receipt['original_image_builds']))
        self.assertTrue(any(b'engine ready \xff\n' in p.read_bytes() for p in receipt_path.parent.rglob('command-*.log')))
        partial = selection.aggregate(self.policy, self.plan, [receipt], complete=False)
        self.assertTrue(partial['passed'], partial)
        self.assertFalse(selection.aggregate(self.policy, self.plan, [receipt], complete=True)['passed'])
        del receipt['contained_lifecycle']
        self.assertFalse(selection.aggregate(self.policy, self.plan, [receipt], complete=False)['passed'])

    def test_late_escaped_writer_overwrite_invalidates_final_evidence(self):
        self.late_evidence_mutation('overwrite')

    def test_late_escaped_writer_deletion_invalidates_final_evidence(self):
        self.late_evidence_mutation('delete')

    def late_evidence_mutation(self, mode):
        # This negative fixture holds process exit, not sealing/finalization,
        # until the real escaped writer acts. No scheduler race or sleep.
        runner = self.source / 'scripts/contained-runner.py'
        runner.write_text(runner.read_text().replace("if __name__ == '__main__':", '''original_main = main

def main():
    code = original_main()
    import ctypes, select
    libc = ctypes.CDLL(None, use_errno=True)
    fd = libc.inotify_init1(0)
    assert fd >= 0 and libc.inotify_add_watch(fd, b'/work', 0x100 | 0x80) >= 0
    while not Path('/work/tampered.json').exists():
        assert select.select([fd], [], [], 30)[0], 'late writer did not act'
        os.read(fd, 65536)
    os.close(fd)
    return code

if __name__ == '__main__':'''))
        (self.source / 'app/late.py').write_text('''import ctypes,json,os,pathlib,select,signal,sys
root=pathlib.Path('/work')
signal.signal(signal.SIGTERM,signal.SIG_IGN)
libc=ctypes.CDLL(None,use_errno=True)
fd=libc.inotify_init1(0)
assert fd>=0 and libc.inotify_add_watch(fd,b'/work',0x100|0x80)>=0
os.write(int(sys.argv[1]),b'ready');os.close(int(sys.argv[1]))
while not (root/'backend.json').exists():
 assert select.select([fd],[],[],30)[0]
 os.read(fd,65536)
os.close(fd)
artifacts=list(root.glob('execution/**/command-evidence/actual/proof.txt'))
logs=list(root.glob('execution/**/command-actual.log'))
assert artifacts and logs
for path in artifacts:
 if sys.argv[2]=='delete':path.unlink()
 else:path.write_bytes(b'late artifact\\xff\\n')
if sys.argv[2]=='overwrite':
 for path in logs:path.write_bytes(b'late log\\xff\\n')
(root/'tampered.json').write_text(json.dumps({'mode':sys.argv[2], 'pid':os.getpid(),'pgid':os.getpgrp(),'sid':os.getsid(0)}))
signal.pause()
''')
        (self.source / 'app/check.py').write_text('''import os,pathlib,subprocess,sys
assert os.environ['DISPLAY'] and os.environ['DBUS_SESSION_BUS_ADDRESS']
read,write=os.pipe()
subprocess.Popen([sys.executable,str(pathlib.Path(__file__).with_name('late.py')),str(write),sys.argv[2]],
 pass_fds=(write,),start_new_session=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
os.close(write);assert os.read(read,5)==b'ready';os.close(read)
pathlib.Path(sys.argv[1]).write_text('actual command')
os.write(1,b'engine ready \\xff\\n')
''')
        self.policy['checks']['actual']['runtime_inputs'].append('{source}/app/late.py')
        self.policy['checks']['actual']['command'].append(mode)
        (self.source / 'tests/selection/inventory.json').write_text(json.dumps(self.policy))
        self.git('add', '.')
        self.git('-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'real late evidence ' + mode)
        sha = self.git('rev-parse', 'HEAD').decode().strip()
        self.plan = selection.make_plan(self.policy, [], head=sha, mode='broad', profiles=['contained.command'])
        self.plan_path.write_text(json.dumps(self.plan))
        process = subprocess.run(self.command(), capture_output=True, timeout=120)
        self.assertNotEqual(process.returncode, 0, (process.stdout + process.stderr).decode(errors='replace'))
        receipt_path = Path(process.stdout.decode().splitlines()[-1])
        receipt = json.loads(receipt_path.read_text())
        work = receipt_path.parent / 'work'
        actor = json.loads((work / 'tampered.json').read_text())
        self.assertEqual(actor['pid'], actor['pgid'])
        self.assertEqual(actor['pid'], actor['sid'])
        self.assertEqual(actor['mode'], mode)
        self.assertFalse(receipt['diagnostic_eligible'])
        self.assertFalse(receipt['complete_gate_eligible'])
        self.assertEqual(receipt['job_conclusion'], 'failure')
        owner = receipt['contained_lifecycle']['receipt']
        self.assertTrue(owner['cleanup_complete'], owner)
        self.assertEqual(owner['status'], 'succeeded')
        final = receipt['contained_lifecycle']['retained_evidence']
        self.assertEqual(final['status'], 'failed')
        self.assertTrue(any(error['reason'] == ('missing' if mode == 'delete' else 'changed') for error in final['errors']))
        command = receipt['backend_receipt']['checks']['actual']
        self.assertEqual(command['outcome'], 'passed')
        log = work / Path(command['log']).relative_to('/work')
        if mode == 'overwrite':
            self.assertEqual(log.read_bytes(), b'late log\xff\n')
            self.assertNotEqual(selection.hashlib.sha256(log.read_bytes()).hexdigest(), command['log_sha256'])
        else:
            self.assertIn(b'engine ready \xff\n', log.read_bytes())
        self.assertFalse(selection.aggregate(self.policy, self.plan, [receipt], complete=False)['passed'])

    def mode_plan(self, mode):
        self.policy['checks']['actual']['environment'] = {'FIXTURE_MODE': mode}
        (self.source / 'tests/selection/inventory.json').write_text(json.dumps(self.policy))
        self.git('add', '.')
        self.git('-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'actual ' + mode)
        sha = self.git('rev-parse', 'HEAD').decode().strip()
        self.plan = selection.make_plan(self.policy, [], head=sha, mode='broad', profiles=['contained.command'])
        self.plan_path.write_text(json.dumps(self.plan))

    def command(self):
        return [sys.executable, str(ROOT / 'scripts/contained-gate.py'), 'run',
            '--source-root', str(self.source), '--evidence-dir', str(self.root / 'evidence'), '--image', self.image,
            '--plan', str(self.plan_path), '--profile', 'contained.command', '--timeout', '45']

    def test_actual_engine_nonzero_is_negative_after_cleanup(self):
        self.mode_plan('fail')
        process = subprocess.run(self.command(), capture_output=True, timeout=120)
        self.assertNotEqual(process.returncode, 0)
        receipt = json.loads(Path(process.stdout.decode().strip().splitlines()[-1]).read_text())
        self.assertFalse(receipt['diagnostic_eligible'])
        self.assertFalse(receipt['complete_gate_eligible'])
        self.assertTrue(receipt['contained_lifecycle']['receipt']['cleanup_complete'])
        self.assertEqual(receipt['contained_lifecycle']['receipt']['status'], 'failed')
        self.assertFalse(selection.aggregate(self.policy, self.plan, [receipt], complete=False)['passed'])

    def test_actual_engine_cancellation_retains_final_negative_attempt_and_raw_bytes(self):
        self.mode_plan('hold')
        process = subprocess.Popen(self.command(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try:
            for line in iter(process.stderr.readline, b''):
                if line.startswith(b'contained-gate: running '):
                    cid = line.decode().split()[-1]
                    break
            else:
                self.fail('contained engine never started')
            # The socket is published atomically only after raw command output.
            observe = """import ctypes, os, pathlib, select, socket
libc=ctypes.CDLL(None, use_errno=True)
p=pathlib.Path('/work/ready.sock')
fd=libc.inotify_init1(0)
assert fd >= 0 and libc.inotify_add_watch(fd,b'/work',0x100|0x80) >= 0
while not p.exists():
    assert select.select([fd],[],[],30)[0], 'command readiness timed out'
    os.read(fd,65536)
os.close(fd)
s=socket.socket(socket.AF_UNIX)
s.connect(str(p))
assert s.recv(64) == b'ready-after-raw'
"""
            observed = subprocess.run(['docker', 'exec', cid, 'python3', '-c', observe], capture_output=True, timeout=60)
            self.assertEqual(observed.returncode, 0, observed.stderr)
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=90)
            self.assertNotEqual(process.returncode, 0)
            receipt_path = Path(stdout.decode().strip().splitlines()[-1])
            receipt = json.loads(receipt_path.read_text())
            self.assertFalse(receipt['diagnostic_eligible'])
            owner = receipt['contained_lifecycle']['receipt']
            self.assertEqual(owner['status'], 'cancelled')
            self.assertTrue(owner['cleanup_complete'], owner.get('cleanup_errors'))
            logs = list(receipt_path.parent.rglob('command-*.log'))
            self.assertTrue(any(b'engine ready \xff\n' in p.read_bytes() for p in logs))
            attempts = [json.loads(p.read_text()) for p in (receipt_path.parent / 'work/execution').rglob('receipt.json')]
            self.assertTrue(any(p.get('outcome') == 'cancelled' for p in attempts), attempts)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=90)
            process.stdout.close()
            process.stderr.close()

    def test_original_native_cli_cannot_authorize_prepared_profile(self):
        process = subprocess.run([sys.executable, str(self.source / 'scripts/test-plan.py'), 'resolve',
            '--repo', str(self.source), '--build-dir', str(self.root / 'native-build'), '--configure',
            '--plan', str(self.plan_path), '--profile', 'contained.command'], capture_output=True, timeout=30)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn(b'contained Linux runner', process.stderr)
        self.assertFalse((self.root / 'native-build').exists())


if __name__ == '__main__':
    unittest.main()
