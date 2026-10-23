"""Selected original MDK controller: fake-Docker lifecycle proofs, NOT live MDK acceptance."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tests/selection'))
import test_selection_mdk as mdk  # noqa: E402
import test_selection_services as services  # noqa: E402
from test_mdk_gate import FAKE  # noqa: E402

CACHE = {'08': ['BUILD_MDK_INTEROP:BOOL=ON'],
         '011': ['BUILD_MDK011_INTEROP:BOOL=ON', 'MDK011_INTEROP_IMAGE:STRING=nostrc-mdk-interop:ci-w27-voice']}


class Harness(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name).resolve()
        self.bin = self.base / 'bin'
        self.bin.mkdir()
        (self.bin / 'docker').write_text(FAKE)
        (self.bin / 'docker').chmod(0o700)
        self.trace = self.base / 'trace'
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ['PATH'], TRACE=str(self.trace))
        self.root = self.base / 'source'
        (self.root / 'scripts').mkdir(parents=True)
        for name in (mdk.HELPER, mdk.WRAPPER, mdk.MODULE):
            shutil.copy2(ROOT / name, self.root / name)
        for directory in ('driver', 'driver-0.9-probe', 'driver-0.11'):
            context = self.root / 'tests/interop/mdk' / directory
            context.mkdir(parents=True)
            (context / 'Dockerfile').write_text('FROM scratch # ' + directory + '\n')
        self.build = self.base / 'build'
        self.build.mkdir()
        self.profile = {'platform': 'linux', 'architecture': os.uname().machine,
                        'execution_domain': mdk.EXECUTION_DOMAIN, 'capabilities': ['docker', 'display', 'private_bus']}
        self.record = {'profile': self.profile, 'repo_root': str(self.root), 'build_dir': str(self.build),
                       'source_identity': {'kind': 'committed', 'sha': 'a' * 40}}
        self.platform = mock.patch.object(mdk.sys, 'platform', 'linux')
        self.platform.start()
        self.addCleanup(self.platform.stop)
        self.cache('011')

    def cache(self, *versions, extra=()):
        lines = ['CMAKE_HOME_DIRECTORY:INTERNAL=' + str(self.root),
                 'GROUNDHOG_DOCKER:FILEPATH=' + str(self.root / mdk.WRAPPER)]
        for version in versions:
            lines += CACHE[version]
        (self.build / 'CMakeCache.txt').write_text('\n'.join([*lines, *extra, '']))

    def contract(self):
        return mdk.contract(self.record, self.env)

    def audit(self, proof):
        return {'passed': True, 'after_build': True, 'input_hashes': dict(proof['inputs'])}

    def daemon_resources(self):
        root = Path(str(self.trace) + '.daemon')
        return {d: sorted(p.name for p in (root / d).iterdir() if '.' not in p.name)
                for d in ('containers', 'volumes')} if root.exists() else {'containers': [], 'volumes': []}


class ContractTests(Harness):
    def test_contract_binds_realized_versions_tags_contexts_and_tools(self):
        proof = self.contract()
        self.assertEqual(set(proof['versions']), {'011', '09'})
        self.assertEqual(proof['versions']['011']['tag'], 'nostrc-mdk-interop:ci-w27-voice')
        self.assertEqual(proof['versions']['09']['tag'], mdk.FIXED_TAGS['09'])
        self.assertFalse(proof['readiness'])
        self.assertFalse(proof['complete'])
        self.assertIn(str(self.root / mdk.WRAPPER), proof['inputs'])
        self.assertIn(str(self.root / 'tests/interop/mdk/driver-0.11/Dockerfile'), proof['inputs'])
        self.assertEqual(set(proof['tools']), {'docker', 'python'})
        self.cache('08')
        self.assertEqual(set(self.contract()['versions']), {'08'})
        self.cache('011', extra=['BUILD_MDK09_PROBE:BOOL=OFF'])
        self.assertEqual(set(self.contract()['versions']), {'011'})

    def test_unowned_or_unauthored_shapes_refuse(self):
        cases = {
            'execution domain': lambda: self.profile.update(execution_domain=None),
            'exact native Linux': lambda: self.profile.update(architecture='alias'),
            'source lifecycle wrapper': lambda: self.cache('011', extra=[]) or (self.build / 'CMakeCache.txt').write_text(
                (self.build / 'CMakeCache.txt').read_text().replace(str(self.root / mdk.WRAPPER), '/usr/bin/docker')),
            'external MDK driver': lambda: self.cache('011', extra=['MDK011_INTEROP_DRIVER:STRING=/peer']),
            'no original MDK image': lambda: self.cache(),
            'plain repository:tag': lambda: (self.build / 'CMakeCache.txt').write_text(
                (self.build / 'CMakeCache.txt').read_text().replace('ci-w27-voice', 'x y')),
            'ambiguous realized cache': lambda: self.cache('011', extra=['BUILD_MDK011_INTEROP:BOOL=OFF']),
        }
        for message, mutate in cases.items():
            with self.subTest(message=message):
                self.setUp()
                mutate()
                with self.assertRaisesRegex(mdk.MDKError, message):
                    self.contract()


class LifecycleTests(Harness):
    def start(self, proof):
        resolved = {'runtime_capability_evidence': {'docker': proof}}
        evidence = self.base / ('evidence-' + str(len(list(self.base.glob('evidence-*')))))
        evidence.mkdir()
        return resolved, evidence

    def test_refuses_before_docker_without_audit_or_pidns_custody(self):
        proof = self.contract()
        resolved, evidence = self.start(proof)
        with mock.patch.dict(os.environ, TRACE=str(self.trace)):
            with self.assertRaisesRegex(mdk.MDKError, 'post-build input audit'):
                with mdk.lifecycle(self.record, resolved, {'passed': True}, evidence, environment=self.env):
                    self.fail('unaudited lifecycle admitted')
            changed = self.audit(proof)
            changed['input_hashes'][str(self.root / mdk.WRAPPER)] = '0' * 64
            with self.assertRaisesRegex(mdk.MDKError, 'full-audit binding'):
                with mdk.lifecycle(self.record, resolved, changed, evidence, environment=self.env):
                    self.fail('stale input admitted')
            with mock.patch.dict(os.environ, NOSTRC_NATIVE_RESOURCE_ROOT=''):
                with self.assertRaisesRegex(mdk.MDKError, 'live pidns custody'):
                    with mdk.lifecycle(self.record, resolved, self.audit(proof), evidence, environment=self.env):
                        self.fail('uncustodied lifecycle admitted')
        self.assertFalse(self.trace.exists())  # no Docker request at all

    def run_lifecycle(self, body):
        proof = self.contract()
        resolved, evidence = self.start(proof)
        with mock.patch.dict(os.environ, TRACE=str(self.trace)), \
                mock.patch.object(services, '_namespace_resources', return_value=self.base):
            try:
                with mdk.lifecycle(self.record, resolved, self.audit(proof), evidence, environment=self.env) as (overlay, state):
                    body(dict(self.env, **overlay), proof)
            finally:
                runtime = json.loads((evidence / 'mdk-runtime.json').read_text())
        return proof, runtime

    def wrapper(self, env, *argv, **kwargs):
        return subprocess.run([str(self.root / mdk.WRAPPER), *argv], env=env, capture_output=True, timeout=30, **kwargs)

    def test_actual_wrapper_build_and_peer_then_exact_cleanup(self):
        def body(env, proof):
            for version, data in proof['versions'].items():
                built = self.wrapper(env, 'build', '-t', data['tag'], data['context'])
                self.assertEqual(built.returncode, 0, built.stderr)
                ran = self.wrapper(env, 'run', '--rm', '-i', '--network', 'host', data['tag'], input=b'{}\n')
                self.assertEqual(ran.returncode, 0, ran.stderr)
            self.assertTrue(self.daemon_resources()['volumes'])  # live builder state while consumers run
        proof, runtime = self.run_lifecycle(body)
        self.assertEqual(runtime['status'], 'passed')
        self.assertTrue(runtime['docker_cleanup_complete'])
        self.assertEqual(runtime['host_process_tree'], 'outer_pidns_namespace_exit_seal_required')
        self.assertFalse(runtime['complete'])
        self.assertEqual(len(runtime['containers']), 2)
        self.assertTrue(all(c['removed'] for c in runtime['containers']))
        self.assertEqual(self.daemon_resources(), {'containers': [], 'volumes': []})
        traces = [json.loads(x) for x in self.trace.read_text().splitlines()]
        builds = [t for t in traces if t[:2] == ['buildx', 'build']]
        self.assertEqual(len(builds), 2)
        self.assertTrue(all(t[t.index('--builder') + 1] == runtime['builder']['name'] for t in builds))
        self.assertFalse(any('prune' in t or '--use' in t for t in traces))
        backend = {'required_capabilities': ['docker'], 'runtime_capability_evidence': {'docker': proof},
                   'input_audit': self.audit(proof), 'mdk_runtime': runtime, 'custody_mode': 'pidns'}
        mdk.validate_runtime(backend, {'source_identity': self.record['source_identity']}, {}, self.profile)
        for key, value in (('status', 'failed'), ('docker_cleanup_complete', False), ('complete', True)):
            with self.subTest(promoted=key):
                with self.assertRaises(mdk.MDKError):
                    mdk.validate_runtime(dict(backend, mdk_runtime=dict(runtime, **{key: value})),
                                         {'source_identity': self.record['source_identity']}, {}, self.profile)
        with self.assertRaisesRegex(mdk.MDKError, 'pidns'):
            mdk.validate_runtime(dict(backend, custody_mode='legacy'), {'source_identity': self.record['source_identity']},
                                 {}, self.profile)
        # A cleanly closed in-namespace journal needs no outer recovery.
        records = mdk.recover_lane(self.base, self.root / mdk.HELPER, self.base)
        self.assertEqual(records, [{'state': runtime['state'], 'closed_in_namespace': True}])

    def test_wrapper_refuses_without_bound_lifecycle(self):
        result = self.wrapper({k: v for k, v in self.env.items() if not k.startswith('NOSTRC_MDK')}, 'build', '-t', 'x:y', '.')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'original MDK lifecycle state required', result.stderr)
        self.assertFalse(self.trace.exists())

    def test_failed_consumer_still_cleans_up_and_retains_failure(self):
        def body(env, proof):
            data = proof['versions']['011']
            self.assertEqual(self.wrapper(env, 'build', '-t', data['tag'], data['context']).returncode, 0)
            self.assertEqual(self.wrapper(env, 'run', '--rm', '-i', '--network', 'host', data['tag']).returncode, 0)
            raise RuntimeError('simulated selected CTest failure')
        with self.assertRaisesRegex(RuntimeError, 'simulated selected CTest failure'):
            self.run_lifecycle(body)
        runtime = json.loads(next(self.base.glob('evidence-*/mdk-runtime.json')).read_text())
        self.assertEqual(runtime['status'], 'failed')
        self.assertTrue(runtime['docker_cleanup_complete'])
        self.assertEqual(self.daemon_resources(), {'containers': [], 'volumes': []})

    def test_killed_lifecycle_is_recovered_from_its_journal_outside(self):
        proof = self.contract()
        evidence = self.base / 'killed'
        evidence.mkdir()
        ready = self.base / 'ready'
        script = f'''
import json, os, subprocess, sys, time
from pathlib import Path
from unittest import mock
sys.path[:0] = [{str(ROOT / 'scripts')!r}]
import test_selection_mdk as mdk, test_selection_services as services
record = json.loads(sys.argv[1]); proof = json.loads(sys.argv[2]); env = json.loads(sys.argv[3])
with mock.patch.object(mdk.sys, 'platform', 'linux'), mock.patch.object(services, '_namespace_resources', return_value=Path('/')):
    with mdk.lifecycle(record, {{'runtime_capability_evidence': {{'docker': proof}}}},
                       {{'passed': True, 'after_build': True, 'input_hashes': proof['inputs']}},
                       {str(evidence)!r}, environment=env) as (overlay, state):
        run = dict(env, **overlay); data = proof['versions']['011']
        subprocess.run([{str(self.root / mdk.WRAPPER)!r}, 'build', '-t', data['tag'], data['context']], env=run, check=True)
        subprocess.run([{str(self.root / mdk.WRAPPER)!r}, 'run', '--rm', '-i', '--network', 'host', data['tag']], env=run, check=True)
        Path({str(ready)!r}).write_text(state['state'])
        time.sleep(60)
'''
        child = subprocess.Popen([sys.executable, '-c', script, json.dumps(self.record), json.dumps(proof), json.dumps(self.env)],
                                 env=self.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 20
        while not ready.exists():
            self.assertLess(time.monotonic(), deadline, 'lifecycle never became ready')
            self.assertIsNone(child.poll())
            time.sleep(0.02)
        child.send_signal(signal.SIGKILL)  # namespace-grace kill: no in-process cleanup
        child.wait(timeout=10)
        state = Path(ready.read_text())
        self.assertTrue(self.daemon_resources()['containers'])  # builder + peer leaked so far
        self.assertFalse((evidence / 'mdk-runtime.json').exists())
        logs = self.base / 'outer-logs'
        logs.mkdir()
        with mock.patch.dict(os.environ, TRACE=str(self.trace), PATH=self.env['PATH']):
            record, = mdk.recover_lane(evidence, self.root / mdk.HELPER, logs)
        self.assertFalse(record['closed_in_namespace'])  # the lane fails even after recovery
        self.assertEqual(record['recovery_exit'], 0, Path(record['log']).read_text())
        result = json.loads(next(state.glob('recovery-*.json')).read_text())
        self.assertEqual(result['status'], 'recovered')
        self.assertEqual(self.daemon_resources(), {'containers': [], 'volumes': []})
        self.assertFalse((state / 'buildx/instances' / ('nostrc-mdk-' + state.name)).exists())
        again = subprocess.run([sys.executable, str(self.root / mdk.HELPER), 'recover', '--state', str(state)],
                               env=self.env, capture_output=True, timeout=60)
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertIn(b'already_closed', again.stdout)


if __name__ == '__main__':
    unittest.main()
