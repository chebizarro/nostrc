"""Offline hosted signer-scope verifier: refusal shapes over synthetic lane trees.

These trees are not hosted evidence and prove nothing about a lane; they pin
that the verifier refuses every non-delivery/non-execution shape."""
import copy
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('signer_scope_proof', ROOT / 'scripts/signer-scope-proof.py')
proof = importlib.util.module_from_spec(spec)
spec.loader.exec_module(proof)
import test_selection_execution as execution

SHA = 'a' * 40
PROFILE = 'grotto.linux'
SPEC = '/run/private/nip5f-delivery-x/resources.json'


def case(name, status='run', child=''):
    return f'<testcase name="{name}" status="{status}">{child}<system-out>ok</system-out></testcase>'


class VerifierShapes(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='scope-proof-'))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.raw = {
            'profile_id': PROFILE, 'source_identity': {'kind': 'committed', 'sha': SHA}, 'attempt': 1,
            'required_capabilities': ['signer_endpoint'], 'outcome': 'passed', 'ctest_exit_code': 0,
            'evidence_dir': '/home/runner/work/_temp/dependency/evidence/r/0/lane-a/evidence/x',
            'observed_executions': {'test_nip5f_tcp': 'passed'},
            'commands': [['ctest', '--show-only=json-v1'],
                         ['ctest', '--test-dir', 'b', '-I', 'i', '--output-on-failure', '--output-junit', 'r',
                          '--parallel', '1', '--resource-spec-file', SPEC, '--timeout', '3600']],
            'ctest_environment': {'base_hash': 'h', 'effective_hash': 'h', 'overlay_names': []},
            'signer_runtime': {'status': 'passed', 'approval_snapshot': proof.APPROVED,
                               'readiness': {'endpoint': 'tcp:127.0.0.1:41234'},
                               'delivery': {'mechanism': 'ctest_resource_allocation_v1', 'resource_type': 'nip5f_signer',
                                            'slots': 1, 'registrations': ['test_nip5f_tcp'],
                                            'endpoint': 'tcp:127.0.0.1:41234', 'resource_spec_file': SPEC,
                                            'ctest_environment_credentials': False, 'removed': True}}}
        self.cases = {name: case(name) for name in ('test_nip5f_tcp', *proof.WITNESSES, 'unrelated')}
        self.qualified = {'receipt_kind': execution.KIND, 'job_conclusion': 'success', 'profile': PROFILE,
                          'source_identity': {'kind': 'committed', 'sha': SHA},
                          'executed_registrations': ['test_nip5f_tcp', 'unrelated']}
        self.log = b'100% tests passed\n'

    def tree(self):
        target = self.root / 'trial'
        shutil.rmtree(target, ignore_errors=True)
        attempt = target / 'evidence/dependency/evidence/r/0/lane-a/evidence' / SHA / PROFILE / 'h/attempt-1'
        attempt.mkdir(parents=True)
        (attempt / 'receipt.json').write_text(json.dumps(self.raw))
        (attempt / 'results.xml').write_text('<testsuite>' + ''.join(self.cases.values()) + '</testsuite>')
        (attempt / 'ctest.log').write_bytes(self.log)
        receipts = target / 'receipt/dependency-receipt-p-grotto.linux-1' / PROFILE
        receipts.mkdir(parents=True)
        (receipts / 'receipt.json').write_text(json.dumps(self.qualified))
        (receipts / 'original-native-receipt.json').write_text('{}')
        return target / 'receipt', target / 'evidence', attempt

    def verify(self):
        receipts, evidence, _ = self.tree()
        return proof.verify(receipts, evidence, PROFILE, SHA)

    def refuses(self, pattern):
        with self.assertRaisesRegex(proof.ProofError, pattern):
            self.verify()

    def test_well_formed_post_fix_shape_is_accepted(self):
        result = self.verify()
        self.assertTrue(result['proved'])
        self.assertIn('not profile', result['scope'])

    def test_consumer_exit_77_skip_or_not_run_is_never_coverage(self):
        for status, child in (('run', '<skipped message="SKIP_RETURN_CODE=77"/>'), ('notrun', ''),
                              ('disabled', ''), ('fail', '<failure message="Failed"/>')):
            with self.subTest(status=status):
                self.cases['test_nip5f_tcp'] = case('test_nip5f_tcp', status, child)
                self.refuses('not a real passing run')
                with self.assertRaisesRegex(ValueError, 'did not actually execute and pass'):
                    execution.signer_consumer_junit(('<testsuite>' + self.cases['test_nip5f_tcp'] + '</testsuite>').encode())
        self.cases['test_nip5f_tcp'] = ''
        self.refuses('not a real passing run')

    def test_duplicate_consumer_case_refuses(self):
        self.cases['dup'] = case('test_nip5f_tcp')
        self.refuses('duplicate JUnit case')
        with self.assertRaisesRegex(ValueError, 'did not actually execute'):
            execution.signer_consumer_junit(b'<testsuite>' + (case('test_nip5f_tcp') * 2).encode() + b'</testsuite>')

    def test_qualifier_accepts_only_one_real_run(self):
        execution.signer_consumer_junit(('<testsuite>' + case('test_nip5f_tcp') + '</testsuite>').encode())
        for data in (b'<testsuite></testsuite>', b'<testsuite'):
            with self.assertRaisesRegex(ValueError, 'did not actually execute'):
                execution.signer_consumer_junit(data)

    def test_backend_outcome_fields_cannot_replace_the_junit(self):
        self.cases['test_nip5f_tcp'] = case('test_nip5f_tcp', 'run', '<skipped/>')
        self.assertEqual(self.raw['observed_executions']['test_nip5f_tcp'], 'passed')
        self.refuses('not a real passing run')

    def test_old_whole_process_overlay_refuses(self):
        self.raw['ctest_environment'] = {'base_hash': 'h', 'effective_hash': 'e',
                                         'overlay_names': ['NOSTR_SIGNER_ENDPOINT', 'NOSTR_SIGNER_TOKEN']}
        self.refuses('overlay the CTest environment')
        self.raw['ctest_environment'] = {'base_hash': 'h', 'effective_hash': 'e', 'overlay_names': []}
        self.refuses('differs from the pre-provisioning')

    def test_delivery_record_and_argv_are_exact(self):
        mutations = {
            'missing delivery': lambda r: r['signer_runtime'].pop('delivery'),
            'not removed': lambda r: r['signer_runtime']['delivery'].update(removed=False),
            'other registration': lambda r: r['signer_runtime']['delivery'].update(registrations=['alias']),
            'env credentials': lambda r: r['signer_runtime']['delivery'].update(ctest_environment_credentials=True),
            'other endpoint': lambda r: r['signer_runtime']['delivery'].update(endpoint='tcp:127.0.0.1:1'),
            'spec in evidence': lambda r: r['signer_runtime']['delivery'].update(resource_spec_file=r['evidence_dir'] + '/s.json'),
            'no approval': lambda r: r['signer_runtime'].update(approval_snapshot='(uint32 1, uint32 0, uint32 1)'),
            'argv spec absent': lambda r: r['commands'][1].__setitem__(slice(10, 12), []),
            'two ctest runs': lambda r: r['commands'].append(list(r['commands'][1])),
            'consumer not passed': lambda r: r['observed_executions'].update(test_nip5f_tcp='unexpected_skip'),
            'retried': lambda r: r.update(attempt=2),
        }
        base = copy.deepcopy(self.raw)
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                self.raw = copy.deepcopy(base)
                mutate(self.raw)
                with self.assertRaises(proof.ProofError):
                    self.verify()

    def test_isolation_witnesses_must_run_in_the_same_process(self):
        for name in proof.WITNESSES:
            with self.subTest(name=name):
                saved = self.cases[name]
                self.cases[name] = case(name, 'notrun')
                self.refuses('isolation witnesses')
                self.cases[name] = ''
                self.refuses('isolation witnesses')
                self.cases[name] = saved

    def test_credential_or_endpoint_bytes_anywhere_refuse(self):
        token = '0123456789abcdef' * 4
        for leak in (f'NOSTR_SIGNER_TOKEN={token}', f'AUTH {token}', f'id:p41234_{token},slots:1'):
            with self.subTest(leak=leak[:12]):
                receipts, evidence, attempt = self.tree()
                (attempt.parents[4] / 'build/Testing/Temporary').mkdir(parents=True)
                (attempt.parents[4] / 'build/Testing/Temporary/LastTest.log').write_text(leak)
                with self.assertRaisesRegex(proof.ProofError, 'credential-shaped'):
                    proof.verify(receipts, evidence, PROFILE, SHA)
        self.log = b'grotto-daemon: tcp ipc server started on 127.0.0.1:41234\n'
        self.refuses('endpoint appears in CTest output')

    def test_post_exit_qualification_is_required(self):
        receipts, evidence, _ = self.tree()
        (next(Path(receipts).rglob('original-native-receipt.json'))).unlink()
        with self.assertRaisesRegex(proof.ProofError, 'post-namespace-exit'):
            proof.verify(receipts, evidence, PROFILE, SHA)
        for key, value in (('job_conclusion', 'failure'), ('executed_registrations', ['unrelated'])):
            with self.subTest(key=key):
                self.qualified = dict(self.qualified, **{key: value})
                self.refuses('post-namespace-exit')

    def test_only_signer_profiles_and_exact_sha(self):
        receipts, evidence, _ = self.tree()
        with self.assertRaisesRegex(proof.ProofError, 'not a signer-provisioning profile'):
            proof.verify(receipts, evidence, 'core.macos.asan', SHA)
        with self.assertRaisesRegex(proof.ProofError, 'exact 40-hex'):
            proof.verify(receipts, evidence, PROFILE, 'abc')
        with self.assertRaisesRegex(proof.ProofError, 'exactly one'):
            proof.verify(receipts, evidence, PROFILE, 'b' * 40)

    def test_collect_only_names_exact_profile_artifacts_by_default(self):
        commands = proof.collect(1, PROFILE, self.root / 'new', 'o/r', execute=False)
        self.assertEqual([c[7] for c in commands], ['dependency-receipt-*-grotto.linux-*',
                                                    'dependency-evidence-*-grotto.linux-*'])
        self.assertFalse((self.root / 'new').exists())


if __name__ == '__main__':
    unittest.main()
