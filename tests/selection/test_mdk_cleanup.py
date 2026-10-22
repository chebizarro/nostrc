"""Standalone lifecycle regressions: fake Docker, real host processes/CTest.

These do not admit guarded selector execution or establish live peer acceptance.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from test_mdk_gate import FAKE

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = '''
import json,os,pathlib,subprocess,sys
state=pathlib.Path(os.environ['NOSTRC_MDK_STATE'])
config=json.loads((state/'config.json').read_text())
'''
BUILD = '''
for data in config['images'].values():
    subprocess.run([os.environ['NOSTRC_MDK_DOCKER'],'build','-t',data['tag'],data['context']],check=True)
    subprocess.run([os.environ['NOSTRC_MDK_DOCKER'],'run','--rm','-i',data['tag']],check=True)
'''


class LifecycleCleanupTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.source = self.base / 'source'
        scripts = self.source / 'scripts'
        scripts.mkdir(parents=True)
        shutil.copyfile(ROOT / 'scripts/mdk-gate.py', scripts / 'mdk-gate.py')
        driver = self.source / 'tests/interop/mdk/driver-0.11'
        driver.mkdir(parents=True)
        (driver / 'Dockerfile').write_text('fake context')
        tools = self.base / 'bin'
        tools.mkdir()
        (tools / 'docker').write_text(FAKE)
        (tools / 'docker').chmod(0o700)
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'], TRACE=str(self.base / 'trace'))
        self.evidence = self.base / 'evidence'

    def run_guard(self, body):
        result = subprocess.run([
            sys.executable, str(self.source / 'scripts/mdk-gate.py'), 'run',
            '--source-root', str(self.source), '--evidence-dir', str(self.evidence), '--version', '011',
            '--', sys.executable, '-c', PRELUDE + body,
        ], env=self.env, capture_output=True, text=True, timeout=15)
        receipts = list(self.evidence.glob('*/receipt.json'))
        self.assertEqual(len(receipts), 1, result.stderr)
        return result, json.loads(receipts[0].read_text()), receipts[0].parent

    def test_existing_ctest_fixture_chain_and_receipt_identity(self):
        body = '''
case=state/'ctest';case.mkdir()
data=config['images']['011'];wrapper=os.environ['NOSTRC_MDK_DOCKER']
def register(name,args):
    return 'add_test('+name+' '+ ' '.join('"'+a+'"' for a in args)+')\\n'
(case/'CTestTestfile.cmake').write_text(
    register('setup',[wrapper,'build','-t',data['tag'],data['context']])+
    'set_tests_properties(setup PROPERTIES FIXTURES_SETUP image)\\n'+
    register('consumer',[wrapper,'run','--rm','-i',data['tag']])+
    'set_tests_properties(consumer PROPERTIES FIXTURES_REQUIRED image)\\n')
subprocess.run(['ctest','--test-dir',str(case),'--output-on-failure','--no-tests=error','-R','^consumer$'],check=True)
'''
        result, receipt, state = self.run_guard(body)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('2/2 Test', result.stdout)
        self.assertEqual(receipt['status'], 'cleanup_unverified')
        self.assertEqual(receipt['command_status'], 'succeeded')
        self.assertEqual(receipt['command_exit_code'], 0)
        self.assertFalse(receipt['cleanup_complete'])
        self.assertEqual(receipt['cleanup_results'], dict(original_session='quiescent',
            recorded_containers='removed', arbitrary_process_tree='unverified'))
        self.assertFalse(receipt['complete'])
        config = json.loads((state / 'config.json').read_text())
        canonical = json.dumps(config, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()
        self.assertEqual(receipt['config_digest'], hashlib.sha256(canonical).hexdigest())
        for key in ('helper', 'wrapper'):
            identity = receipt[key]
            self.assertEqual(identity['sha256'], hashlib.sha256(Path(identity['path']).read_bytes()).hexdigest())
        self.assertEqual(receipt['configure_args'], json.loads((state / 'configure-args.json').read_text()))

    def assert_stopped(self, pid):
        status = subprocess.run(['ps', '-p', str(pid), '-o', 'stat='], capture_output=True, text=True).stdout.strip()
        self.assertTrue(not status or status.startswith('Z'), 'owned grandchild survived cleanup')

    @staticmethod
    def clean_known_descendant(pid, session):
        # The session belongs only to this test; never name-match or signal a
        # reused PID in another session, and never touch a sibling's session.
        try:
            if os.getsid(pid) == session:
                os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def test_exited_direct_child_does_not_leave_grandchild_or_kill_sibling(self):
        sibling = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(60)'], start_new_session=True)
        try:
            body = BUILD + '''
orphan=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
(state/'grandchild.pid').write_text(str(orphan.pid))
'''
            result, receipt, state = self.run_guard(body)
            pid = int((state / 'grandchild.pid').read_text())
            try:
                self.assert_stopped(pid)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(receipt['status'], 'cleanup_unverified')
                self.assertEqual(receipt['command_exit_code'], 0)
                self.assertFalse(receipt['cleanup_complete'])
                self.assertEqual(receipt['cleanup_results']['original_session'], 'quiescent')
                self.assertEqual(receipt['cleanup_results']['arbitrary_process_tree'], 'unverified')
                self.assertIsNone(sibling.poll(), 'sibling process was killed')
            finally:
                self.clean_known_descendant(pid, receipt['child_pid'])
        finally:
            sibling.terminate()
            sibling.wait(timeout=5)

    def test_later_success_cannot_hide_earlier_undrained_client(self):
        pidfile = self.base / 'pipe-holder.pid'
        self.env.update(PIPE_ONCE='1', PIPE_READY=str(pidfile))
        body = '''
data=config['images']['011'];wrapper=os.environ['NOSTRC_MDK_DOCKER']
first=subprocess.run([wrapper,'build','-t',data['tag'],data['context']])
assert first.returncode != 0
subprocess.run([wrapper,'build','-t',data['tag'],data['context']],check=True)
subprocess.run([wrapper,'run','--rm','-i',data['tag']],check=True)
'''
        try:
            result = subprocess.run([sys.executable, str(self.source / 'scripts/mdk-gate.py'), 'run',
                '--source-root', str(self.source), '--evidence-dir', str(self.evidence), '--version', '011',
                '--', sys.executable, '-c', PRELUDE + body], env=self.env, capture_output=True, text=True, timeout=45)
            receipt = json.loads(next(self.evidence.glob('*/receipt.json')).read_text())
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertEqual(receipt['command_exit_code'], 0)
            self.assertTrue(receipt['image_results'][0]['build_complete'])
            self.assertFalse(receipt['docker_lifecycle']['cleanup_complete'])
            self.assertIn('retained Docker client did not drain', ' '.join(receipt['cleanup_errors']))
            self.assertFalse(receipt['cleanup_complete'])
        finally:
            if pidfile.exists():
                pid = int(pidfile.read_text())
                try:
                    if os.getsid(pid) == pid:
                        os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass

    def test_corrupt_peer_journal_keeps_failed_receipt(self):
        result, receipt, state = self.run_guard(BUILD + "(state/'containers/broken.json').write_text('{')\n")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(receipt['docker_lifecycle']['cleanup_complete'])
        self.assertFalse(receipt['cleanup_complete'])
        self.assertTrue(any('invalid_journal' in entry for entry in receipt['containers']))
        self.assertEqual((state / 'containers/broken.json').read_text(), '{')

    def test_unconfirmed_process_cleanup_still_removes_owned_containers(self):
        body = BUILD + '''
import select
reader,writer=os.pipe()
code="import os,sys,time;os.setpgid(0,0);os.write(int(sys.argv[1]),b'R');time.sleep(60)"
orphan=subprocess.Popen([sys.executable,'-c',code,str(writer)],pass_fds=(writer,),stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
os.close(writer)
(state/'grandchild.pid').write_text(str(orphan.pid))
# Explicit readiness handshake: the new process group exists before root exit.
assert select.select([reader],[],[],5)[0], 'child readiness failed'
assert os.read(reader,1)==b'R'
os.close(reader)
'''
        result, receipt, state = self.run_guard(body)
        pid = int((state / 'grandchild.pid').read_text())
        try:
            self.assertNotEqual(result.returncode, 0, result.stderr)
            self.assertFalse(receipt['cleanup_complete'])
            self.assertEqual(receipt['status'], 'cleanup_failed')
            self.assertEqual(receipt['command_exit_code'], 0)
            self.assertEqual(receipt['cleanup_results'], dict(original_session='unverified',
                recorded_containers='removed', arbitrary_process_tree='unverified'))
            self.assertIn('owned session descendants remain', ' '.join(receipt['cleanup_errors']))
            trace = [json.loads(line) for line in (self.base / 'trace').read_text().splitlines()]
            removed = [args[-1] for args in trace if args[:2] == ['rm', '-f']]
            self.assertEqual(removed, [entry['container_id'] for entry in receipt['containers']])
        finally:
            self.clean_known_descendant(pid, receipt['child_pid'])


@unittest.skipUnless(os.environ.get('NOSTRC_MDK_REAL_DOCKER') == '1',
                     'opt-in real Docker image-build lifecycle (not MDK parity)')
class RealDockerLifecycleTests(unittest.TestCase):
    """Real private builders and scratch images; never a prepared-image proof.

    NOSTRC_MDK_REAL_DOCKER=1 explicitly requires Docker/buildx and a local musl
    BusyBox image (NOSTRC_MDK_FIXTURE_IMAGE, default alpine:latest). Missing
    capability fails, rather than silently skipping an explicitly requested run.
    Raw logs, contexts and both failed/successful receipts are retained below
    NOSTRC_MDK_REAL_EVIDENCE (default a printed /tmp directory).
    """
    @classmethod
    def setUpClass(cls):
        parent = os.environ.get('NOSTRC_MDK_REAL_EVIDENCE')
        if parent:
            Path(parent).mkdir(parents=True, exist_ok=True)
        cls.base = Path(tempfile.mkdtemp(prefix='nostrc-mdk-real-', dir=parent)).resolve()
        print('real original-Docker lifecycle evidence:', cls.base, flush=True)
        cls.docker = shutil.which('docker')
        if not cls.docker:
            raise AssertionError('explicit real Docker run requires Docker')
        cls.call('info', '--format', '{{.ID}}')
        cls.call('buildx', 'version')
        cls.selected_builder = cls.call('buildx', 'inspect').splitlines()[0]
        image = json.loads(cls.call('image', 'inspect', os.environ.get('NOSTRC_MDK_FIXTURE_IMAGE', 'alpine:latest')))[0]
        arch = image['Architecture']
        loader = {'arm64': 'aarch64', 'amd64': 'x86_64'}[arch]
        cls.inputs = cls.base / 'inputs'
        cls.inputs.mkdir(exist_ok=True)
        name = 'nostrc-mdk-fixture-' + cls.base.name + '-' + str(os.getpid())
        cid = cls.call('create', '--name', name, '--network', 'none', '--label',
                       'org.nostrc.mdk.fixture=' + name, image['Id']).strip()
        try:
            cls.call('cp', '-L', cid + ':/bin/busybox', str(cls.inputs / 'busybox'))
            cls.call('cp', '-L', cid + ':/lib/ld-musl-' + loader + '.so.1', str(cls.inputs / 'loader'))
        finally:
            cls.call('rm', '-f', cid)  # only the immutable CID this fixture created
        cls.loader = '/lib/ld-musl-' + loader + '.so.1'
        (cls.inputs / 'identity.json').write_text(json.dumps(dict(image_id=image['Id'], architecture=arch,
            source_container=cid, files={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                       for p in cls.inputs.iterdir() if p.name != 'identity.json'}), sort_keys=True))

    @classmethod
    def call(cls, *args, env=None):
        result = subprocess.run([cls.docker, *args], env=env, capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError('Docker ' + repr(args) + ': ' + result.stderr)
        return result.stdout

    def source(self, name, mode='cat'):
        source = self.base / (self._testMethodName + '-' + name)
        context = source / 'tests/interop/mdk/driver-0.11'
        context.mkdir(parents=True)
        for entry in ('busybox', 'loader'):
            shutil.copy2(self.inputs / entry, context / entry)
            self.assertTrue(os.access(context / entry, os.X_OK), 'fixture must retain executable mode')
        text = ('FROM scratch\nCOPY busybox /bin/busybox\nCOPY loader ' + self.loader + '\n')
        if mode == 'fail':
            text += 'RUN ["/bin/busybox", "sh", "-c", "echo OWNED-MDK-PARTIAL >&2; exit 29"]\n'
        elif mode == 'build':
            text += 'RUN ["/bin/busybox", "sh", "-c", "echo OWNED-MDK-BUILD-READY >&2; /bin/busybox sleep 600 & wait"]\n'
        text += ('ENTRYPOINT ["/bin/busybox", "sh", "-c", "echo OWNED-MDK-PEER-READY; /bin/busybox sleep 600 & wait"]\n'
                 if mode == 'peer' else 'ENTRYPOINT ["/bin/busybox", "cat"]\n')
        (context / 'Dockerfile').write_text(text)
        return source

    def start(self, source, env=None):
        evidence = source / 'evidence'
        log = source / 'lifecycle.log'
        proc = subprocess.Popen([sys.executable, str(ROOT / 'scripts/mdk-gate.py'), 'images',
            '--source-root', str(source), '--evidence-dir', str(evidence), '--version', '011', '--probe'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        condition, output = threading.Condition(), bytearray()
        def drain():
            with log.open('wb') as target:
                while True:
                    block = proc.stdout.read1(65536)
                    if not block:
                        break
                    target.write(block)
                    target.flush()
                    with condition:
                        output.extend(block)
                        condition.notify_all()
            proc.stdout.close()
            with condition:
                condition.notify_all()
        reader = threading.Thread(target=drain)
        reader.start()
        def stop():
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=150)
            reader.join(timeout=20)
        self.addCleanup(stop)
        return dict(proc=proc, reader=reader, condition=condition, output=output, evidence=evidence, log=log)

    def ready(self, run, marker):
        deadline = time.monotonic() + 180
        pattern = (rb'(?m)^#\d+ [0-9.]+ ' + marker.encode() + rb'\r?$'
                   if marker == 'OWNED-MDK-BUILD-READY' else rb'(?m)^' + marker.encode() + rb'\r?$')
        with run['condition']:
            while not re.search(pattern, run['output']):
                if run['proc'].poll() is not None or time.monotonic() >= deadline:
                    self.fail('No readiness marker ' + marker + ': ' + str(run['log']) + '\n' + run['output'].decode(errors='replace'))
                # Event/condition notification comes from actual emitted bytes;
                # elapsed time is a failure bound, never a readiness oracle.
                run['condition'].wait(timeout=min(1, max(0, deadline - time.monotonic())))
        return json.loads(next(run['evidence'].glob('*/config.json')).read_text())

    def finish(self, run, code):
        self.assertEqual(run['proc'].wait(timeout=150), code, str(run['log']))
        run['reader'].join(timeout=20)
        self.assertFalse(run['reader'].is_alive(), 'test output writer did not drain')
        receipts = list(run['evidence'].glob('*/receipt.json'))
        self.assertEqual(len(receipts), 1, str(run['log']))
        return json.loads(receipts[0].read_text())

    def assert_clean(self, receipt):
        self.assertTrue(receipt['cleanup_complete'], receipt)
        self.assertTrue(receipt['docker_lifecycle']['cleanup_complete'], receipt)
        self.assertFalse(receipt['complete'])
        self.assertEqual(receipt['cleanup_results']['arbitrary_process_tree'], 'not_applicable_docker_only')
        builder = receipt['builder']
        self.assertEqual(self.call('ps', '-aq', '--no-trunc', '--filter', 'name=^/' + builder['container_name'] + '$').strip(), '')
        self.assertEqual(self.call('volume', 'ls', '-q', '--filter', 'name=^' + builder['volume'] + '$').strip(), '')
        self.assertFalse(Path(builder['instance']['path']).exists())
        for peer in receipt['containers']:
            self.assertTrue(peer['removed'])
            self.assertTrue(peer['logs_drained'])
            self.assertEqual(self.call('ps', '-aq', '--no-trunc', '--filter', 'id=' + peer['container_id']).strip(), '')
        state = Path(receipt['state'])
        for client in (state / 'clients').glob('*.json'):
            data = json.loads(client.read_text())
            self.assertTrue(data['drained'], data)
            self.assertEqual(set(data['streams']), {'stdout', 'stderr'})
            for identity in data['streams'].values():
                self.assertEqual(identity['sha256'], hashlib.sha256(Path(identity['path']).read_bytes()).hexdigest())
        self.assertEqual(self.call('buildx', 'inspect').splitlines()[0], self.selected_builder,
                         'global builder selection changed')

    def test_actual_build_run_and_exact_metadata(self):
        receipt = self.finish(self.start(self.source('positive')), 0)
        self.assert_clean(receipt)
        self.assertEqual(receipt['status'], 'docker_lifecycle_succeeded')
        image, = receipt['image_results']
        self.assertTrue(image['build_complete'])
        meta = json.loads(Path(image['metadata']['path']).read_text())
        self.assertEqual(meta['containerimage.digest'], image['export_digest'])
        self.assertEqual(meta['containerimage.descriptor']['digest'], image['export_digest'])
        self.assertTrue(meta['buildx.build.ref'].startswith(receipt['builder']['name'] + '/' + receipt['builder']['node'] + '/'))
        self.assertEqual(image['build_argv'][image['build_argv'].index('--builder') + 1], receipt['builder']['name'])
        self.assertIn(b'{"id":1,"cmd":"hello"}', next((Path(receipt['state']) / 'containers').glob('*.stdout')).read_bytes())

    def test_actual_partial_build_retains_failure_and_drains(self):
        run = self.start(self.source('partial', 'fail'))
        receipt = self.finish(run, 1)
        self.assertIn(b'OWNED-MDK-PARTIAL', run['output'])
        self.assert_clean(receipt)
        image, = receipt['image_results']
        self.assertFalse(image['build_complete'])
        self.assertNotEqual(image['build_exit_code'], 0)
        self.assertTrue(image['logs_drained'])
        self.assertEqual(receipt['containers'], [])
        self.assertNotEqual(receipt['status'], 'docker_lifecycle_succeeded')

    def test_actual_cancel_build_drains_and_removes_namespace(self):
        run = self.start(self.source('cancel-build', 'build'))
        self.ready(run, 'OWNED-MDK-BUILD-READY')
        run['proc'].terminate()
        receipt = self.finish(run, 130)
        self.assert_clean(receipt)
        self.assertEqual(receipt['status'], 'cancelled')
        self.assertFalse(receipt['image_results'][0]['build_complete'])
        self.assertTrue(receipt['image_results'][0]['logs_drained'])

    def test_actual_cancel_peer_leaves_concurrent_sibling_running(self):
        sibling = self.start(self.source('sibling', 'peer'))
        sibling_config = self.ready(sibling, 'OWNED-MDK-PEER-READY')
        owned = self.start(self.source('owned', 'peer'))
        self.ready(owned, 'OWNED-MDK-PEER-READY')
        owned['proc'].terminate()
        self.assert_clean(self.finish(owned, 130))
        self.assertIsNone(sibling['proc'].poll())
        self.assertTrue(json.loads(self.call('container', 'inspect', sibling_config['builder']['container_id']))[0]['State']['Running'])
        peer = json.loads(next((Path(sibling_config['state']) / 'containers').glob('*.json')).read_text())
        self.assertTrue(json.loads(self.call('container', 'inspect', peer['container_id']))[0]['State']['Running'])
        sibling['proc'].terminate()
        self.assert_clean(self.finish(sibling, 130))

    def test_actual_corrupt_builder_metadata_refuses_sibling_removal(self):
        sibling = self.start(self.source('sibling', 'peer'))
        sibling_config = self.ready(sibling, 'OWNED-MDK-PEER-READY')
        owned = self.start(self.source('owned', 'build'))
        config = self.ready(owned, 'OWNED-MDK-BUILD-READY')
        instance = Path(config['builder']['instance']['path'])
        original = instance.read_bytes()
        data = json.loads(original)
        data['Nodes'][0]['Name'] = sibling_config['builder']['node']
        instance.write_text(json.dumps(data))
        try:
            owned['proc'].terminate()
            receipt = self.finish(owned, 130)
            self.assertFalse(receipt['cleanup_complete'])
            self.assertIn('private Buildx instance ownership mismatch', ' '.join(receipt['cleanup_errors']))
            self.assertTrue(json.loads(self.call('container', 'inspect', sibling_config['builder']['container_id']))[0]['State']['Running'])
        finally:
            # Test recovery only: restore the exact pre-corruption registration,
            # verify original immutable CID/run marker, then remove only that builder.
            instance.write_bytes(original)
            current = json.loads(self.call('container', 'inspect', config['builder']['container_id']))[0]
            self.assertEqual(current['Id'], config['builder']['container_id'])
            self.assertIn('NOSTRC_MDK_RUN=' + config['run_id'], current['Config']['Env'])
            env = dict(os.environ, BUILDX_CONFIG=str(Path(config['state']) / 'buildx'), DOCKER_HOST=config['docker_endpoint'])
            env.pop('DOCKER_CONTEXT', None)
            self.call('buildx', 'rm', '--force', config['builder']['name'], env=env)
        sibling['proc'].terminate()
        self.assert_clean(self.finish(sibling, 130))


if __name__ == '__main__':
    unittest.main()
