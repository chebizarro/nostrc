"""Offline hosted signer-scope verifier: strict binding and refusal shapes.

Nothing here is hosted evidence or produces ``proved: true``. Acceptance rests
only on the existing strict transported-receipt path; these tests prove the
verifier reaches it, binds the retained original to it, and refuses every
mismatched, tampered, missing, replaced, forged, unsatisfiable or leaking shape.
"""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('signer_scope_proof', ROOT / 'scripts/signer-scope-proof.py')
proof = importlib.util.module_from_spec(spec)
spec.loader.exec_module(proof)
import test_selection as selection  # noqa: E402
import test_selection_execution as execution  # noqa: E402
from test_normal_transport import TransportFixture  # noqa: E402

PROFILE = 'grotto.linux'
EXCLUDED = ('core.linux', 'core.linux.asan', 'grotto.linux.asan_ubsan')


def case(name, status='run', child=''):
    return f'<testcase name="{name}" status="{status}">{child}<system-out>ok</system-out></testcase>'


class ConsumerJUnitQualification(unittest.TestCase):
    """qualify()'s own sealed-JUnit check: one real run, never a skip."""

    def test_qualifier_accepts_only_one_real_run(self):
        execution.signer_consumer_junit(('<testsuite>' + case('test_nip5f_tcp') + '</testsuite>').encode())
        for data in ('<testsuite>' + case('test_nip5f_tcp', 'notrun', '<skipped message="77"/>') + '</testsuite>',
                     '<testsuite>' + case('test_nip5f_tcp', 'notrun') + '</testsuite>',
                     '<testsuite>' + case('test_nip5f_tcp', 'fail', '<failure/>') + '</testsuite>',
                     '<testsuite>' + case('test_nip5f_tcp', 'run', '<error/>') + '</testsuite>',
                     '<testsuite>' + case('test_nip5f_tcp') * 2 + '</testsuite>',
                     '<testsuite></testsuite>', '<testsuite'):
            with self.subTest(data=data[:60]), self.assertRaisesRegex(ValueError, 'did not actually execute'):
                execution.signer_consumer_junit(data.encode())


class SatisfiableProfiles(unittest.TestCase):
    """F2: only profiles whose inventory maps every witness are claimed."""

    def setUp(self):
        self.policy = selection.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_claimed_profiles_equal_the_inventory_satisfiable_set(self):
        self.assertEqual(proof.satisfiable(self.policy), tuple(sorted(proof.PROFILES)))
        for profile in EXCLUDED:
            with self.subTest(profile=profile):
                self.assertIn(profile, self.policy['checks'][proof.CHECK]['profiles'])
                self.assertNotIn('signer/room-flow', proof.witness_map(self.policy, profile))
                with self.assertRaisesRegex(proof.ProofError, 'not a profile whose inventory can realize'):
                    proof.verify('/nonexistent', '/nonexistent', ROOT, profile)
                with self.assertRaisesRegex(proof.ProofError, 'not a profile whose inventory can realize'):
                    proof.collect(1, profile, '/nonexistent', 'o/r', execute=False)

    def test_inventory_drift_that_unmaps_a_witness_refuses(self):
        for ident in proof.witness_map(self.policy, PROFILE)['signer/room-flow']:
            self.policy['checks'][ident]['ctest_tests'][PROFILE].remove('signer/room-flow')
        self.assertNotIn(PROFILE, proof.satisfiable(self.policy))

    def test_collect_names_plan_receipt_and_evidence_artifacts_only(self):
        root = Path(tempfile.mkdtemp(prefix='scope-collect-'))
        self.addCleanup(shutil.rmtree, root, True)
        commands = proof.collect(1, PROFILE, root / 'new', 'o/r', execute=False)
        self.assertEqual([c[7] for c in commands], ['dependency-plan-*', 'dependency-receipt-*-grotto.linux-*',
                                                    'dependency-evidence-*-grotto.linux-*'])
        self.assertEqual([c[9] for c in commands], [str(root / 'new' / d) for d in ('plan', 'receipts', 'evidence')])
        self.assertFalse((root / 'new').exists())


class Diagnostics(unittest.TestCase):
    """Read-only diagnostics, reached in production only after strict acceptance."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='scope-diag-')).resolve()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.policy = selection.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.evidence = self.root / 'lane/evidence'
        self.evidence.mkdir(parents=True)
        self.public = {'backend_receipt': {'required_capabilities': ['signer_endpoint'],
                                           'evidence_dir': str(self.evidence),
                                           'signer_runtime': {'readiness': {'endpoint': 'tcp:127.0.0.1:41234'}}}}
        self.receipt = {'executed_registrations': ['test_nip5f_tcp', *proof.WITNESSES, 'unrelated']}
        self.cases = {name: case(name) for name in ('test_nip5f_tcp', *proof.WITNESSES)}
        self.log = b'100% tests passed\n'

    def run_diagnose(self):
        (self.evidence / 'results.xml').write_text('<testsuite>' + ''.join(self.cases.values()) + '</testsuite>')
        (self.evidence / 'ctest.log').write_bytes(self.log)
        return proof.diagnose(self.policy, {}, self.receipt, self.public, execution.Locator(), PROFILE)

    def refuses(self, pattern):
        with self.assertRaisesRegex(proof.ProofError, pattern):
            self.run_diagnose()

    def test_witnesses_must_be_co_selected_into_the_same_qualified_lane(self):
        self.assertEqual(self.run_diagnose()['witnesses'], list(proof.WITNESSES))  # diagnostics, not a proof
        for name in proof.WITNESSES:
            with self.subTest(name=name):
                self.receipt = {'executed_registrations': ['test_nip5f_tcp', *(w for w in proof.WITNESSES if w != name)]}
                self.refuses('not co-selected')

    def test_consumer_or_witness_skip_failure_or_absence_refuses(self):
        for name in ('test_nip5f_tcp', *proof.WITNESSES):
            for value in (case(name, 'notrun', '<skipped/>'), case(name, 'fail', '<failure/>'), ''):
                with self.subTest(name=name, value=value[:30]):
                    saved, self.cases[name] = self.cases[name], value
                    self.refuses('consumer JUnit case' if name == 'test_nip5f_tcp' else 'did not run and pass')
                    self.cases[name] = saved

    def test_unprovisioned_lane_and_endpoint_in_output_refuse(self):
        self.log = b'grotto-daemon: tcp ipc server started on 127.0.0.1:41234\n'
        self.refuses('endpoint appears in CTest output')
        self.public['backend_receipt']['required_capabilities'] = []
        self.refuses('did not provision the signer')

    def test_credential_shapes_anywhere_refuse(self):
        token = '0123456789abcdef' * 4
        with self.assertRaisesRegex(proof.ProofError, 'no retained evidence'):
            proof.scan(self.root / 'lane')  # an empty tree never counts as clean
        (self.evidence / 'ctest.log').write_bytes(self.log)
        self.assertEqual(proof.scan(self.root / 'lane'), 1)
        for leak in (f'NOSTR_SIGNER_TOKEN={token}', f'AUTH {token}', f'id:p41234_{token},slots:1'):
            with self.subTest(leak=leak[:12]):
                path = self.root / 'lane/build/Testing/Temporary/LastTest.log'
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(leak)
                with self.assertRaisesRegex(proof.ProofError, 'credential-shaped'):
                    proof.scan(self.root / 'lane')
                path.unlink()


def _tree_digest(root, skip=None):
    """Exact names, modes, bytes and link targets under root (mtimes excluded)."""
    root, digest = Path(root), hashlib.sha256()
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root)
        if skip is not None and relative.parts[0] == skip:
            continue
        digest.update(os.fsencode(relative) + b'\0' + oct(path.lstat().st_mode).encode() + b'\0')
        if path.is_symlink():
            digest.update(os.fsencode(os.readlink(path)))
        elif path.is_file():
            digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


class StrictBinding(TransportFixture):
    """F1: the real worker -> artifact -> aggregate path over a genuine synthetic
    sealed bundle. The synthetic lane provisions no signer, so a genuine bundle
    passes strict acceptance and the original binding, then is refused at the
    first signer diagnostic: no synthetic success is ever produced.

    Only the read-only verifier is under test here, so ONE genuine worker run
    (real configure/build/run, seal and worker_qualify: ~90% of this class's
    measured time when repeated per case) is shared by the class. Every case
    still gets its own fresh download of those exact worker bytes into its own
    temporary directory and its own inventory/plan copies; the shared worker
    receipts and committed checkout are proven byte-unchanged before every case
    and after the last, so no case can observe another's mutation."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        genuine = cls('runTest')  # holder for the class-shared fixture; never part of the suite
        cls.addClassCleanup(genuine.doCleanups)
        TransportFixture.setUp(genuine)  # the unchanged fixture chain (skips without CMake/Ninja)
        genuine.qualified = genuine.worker()
        cls.genuine = genuine
        cls.pristine = cls.shared_state()
        cls.addClassCleanup(cls.assert_shared_unchanged)  # LIFO: runs before the fixture is removed

    @classmethod
    def shared_state(cls):
        genuine = cls.genuine
        head = subprocess.run(['git', '-C', str(genuine.root), 'rev-parse', 'HEAD'], check=True,
                              capture_output=True).stdout
        return dict(receipts=_tree_digest(genuine.receipts), checkout=_tree_digest(genuine.root, skip='.git'),
                    head=head)

    @classmethod
    def assert_shared_unchanged(cls):
        if cls.shared_state() != cls.pristine:
            raise AssertionError('shared genuine worker output or committed checkout was mutated by a case')

    def setUp(self):
        genuine = type(self).genuine
        self.assert_shared_unchanged()
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root, self.receipts = genuine.root, genuine.receipts
        self.inventory, self.plan = copy.deepcopy(genuine.inventory), copy.deepcopy(genuine.plan)
        self.qualified = copy.deepcopy(genuine.qualified)
        self.base = self.download()  # isolated per-case copy, exactly the artifact-download layout
        self.lane = next(self.base.glob('*/synthetic'))

    def verify(self, **kwargs):
        return proof.verify_qualified(self.inventory, self.plan, self.base, self.root, 'synthetic', **kwargs)

    def refuses(self, pattern, **kwargs):
        with self.assertRaisesRegex(proof.ProofError, pattern):
            self.verify(**kwargs)

    def test_genuine_bundle_reaches_diagnostics_only_through_strict_acceptance(self):
        self.refuses('lane did not provision the signer')
        # Raw lane evidence never contributes acceptance: an unrelated tree changes nothing.
        unrelated = Path(self.temp.name) / 'unrelated'
        (unrelated / 'attempt-1').mkdir(parents=True)
        (unrelated / 'attempt-1/receipt.json').write_text(json.dumps({'outcome': 'passed'}))
        self.refuses('lane did not provision the signer', evidence=unrelated)

    def test_original_must_equal_the_verified_sealed_public_evidence(self):
        original = self.lane / execution.ORIGINAL
        raw = original.read_bytes()
        public = json.loads(raw)
        for name, mutate in (('empty object (old positive fixture)', lambda: original.write_text('{}')),
                             ('missing', original.unlink),
                             ('replaced valid receipt', lambda: original.write_text(json.dumps(dict(public, attempt=2)))),
                             ('one appended byte', lambda: original.write_bytes(raw + b'\n')),
                             ('symlink to the real bytes', lambda: (original.unlink(), original.symlink_to(
                                 self.lane / 'receipt.json')))):
            with self.subTest(case=name):
                mutate()
                self.refuses('original-native-receipt.json differs')
                original.unlink(missing_ok=True)
                original.write_bytes(raw)
        self.refuses('lane did not provision the signer')

    def test_tampered_or_swapped_lane_evidence_refuses_at_strict_acceptance(self):
        def alter(manifest, contents):
            name = next(n for n in contents if n.endswith('/resolved.json'))
            contents[name] = contents[name].replace(b'"', b'"', 1) + b' '
            next(m for m in manifest['members'] if m['name'] == name)['size'] += 1
        self.rewrite(self.base, alter, refresh=False)
        self.refuses('strict qualified-receipt verification refused')
        self.rewrite(self.base, lambda manifest, contents: None, refresh=True)  # self-consistent forged hashes
        self.refuses('strict qualified-receipt verification refused')

    def test_forged_qualified_receipt_refuses(self):
        path = self.lane / 'receipt.json'
        receipt = json.loads(path.read_text())
        receipt['executed_registrations'] = receipt['executed_registrations'] + ['signer/clients']
        receipt['qualification_hash'] = selection.digest({k: v for k, v in receipt.items() if k != 'qualification_hash'})
        path.write_text(json.dumps(receipt))
        self.refuses('strict qualified-receipt verification refused')

    def test_old_verifier_accepted_shape_refuses(self):
        # The reviewer's F1 shape: summary-only qualified receipt beside an `{}`
        # original (exactly what the 719616996 verifier printed proved: true for).
        for name in list(self.lane.iterdir()):
            name.unlink()
        (self.lane / 'receipt.json').write_text(json.dumps(
            {'receipt_kind': execution.KIND, 'job_conclusion': 'success', 'profile': 'synthetic',
             'source_identity': self.plan['source_identity'], 'executed_registrations': ['test_nip5f_tcp']}))
        (self.lane / execution.ORIGINAL).write_text('{}')
        self.refuses('strict qualified-receipt verification refused')

    def test_ambiguous_wrong_plan_or_profile_artifacts_refuse(self):
        shutil.copytree(self.lane.parent, self.base / ('dependency-receipt-%s-synthetic-2' % self.plan['plan_id']))
        self.refuses('exactly one synthetic lane receipt')
        shutil.rmtree(self.base / ('dependency-receipt-%s-synthetic-2' % self.plan['plan_id']))
        renamed = self.base / 'dependency-receipt-other-synthetic-1'
        self.lane.parent.rename(renamed)
        self.refuses('exactly one synthetic lane receipt')
        renamed.rename(self.lane.parent)
        with self.assertRaisesRegex(proof.ProofError, 'exactly one other lane receipt'):
            proof.verify_qualified(self.inventory, self.plan, self.base, self.root, 'other')

    def test_plan_without_exact_committed_sha_refuses(self):
        plan = dict(self.plan, source_identity={'kind': 'worktree'})
        with self.assertRaisesRegex(proof.ProofError, 'exact committed tested SHA'):
            proof.verify_qualified(self.inventory, plan, self.base, self.root, 'synthetic')


if __name__ == '__main__':
    unittest.main()
