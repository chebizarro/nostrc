"""Portable normal-receipt transport (nostrc-0cfsg.36.8).

Small real synthetic builds/seals in the worker's <run>/0/lane-<id> layout with
synthetic supervisor/AppArmor records. Proves the worker->artifact->aggregate
contract logic only: NOT Linux pidns, AppArmor kernel, hosted or MDK proof.
"""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import unittest
from unittest import mock

from realization_support import ROOT
from selector_support import s
from test_normal_execution_contract import NormalFixture
import test_selection_execution as normal


def split(path):
    data = Path(path).read_bytes()
    start = len(normal.MAGIC) + 8
    length = int.from_bytes(data[len(normal.MAGIC):start], 'big')
    manifest = json.loads(data[start:start + length])
    contents, offset = {}, start + length
    for member in manifest['members']:
        if member['kind'] == 'file':
            contents[member['name']] = data[offset:offset + member['size']]
            offset += member['size']
    return manifest, contents


def join(manifest, contents):
    raw = (json.dumps(manifest, sort_keys=True, separators=(',', ':')) + '\n').encode()
    body = b''.join(contents[m['name']] for m in manifest['members'] if m['kind'] == 'file')
    return normal.MAGIC + len(raw).to_bytes(8, 'big') + raw + body, raw


class TransportTests(NormalFixture):
    LANE = ('dependency', 'evidence', 'run1', '0', 'lane-synthetic')
    ADAPTER_UP = 1

    def setUp(self):
        super().setUp()
        self.dependency = Path(self.temp.name).resolve() / 'dependency'
        self.apparmor = self.dependency / 'namespace-apparmor'
        self.receipts = self.dependency / 'receipts' / 'synthetic'
        self.receipts.mkdir(parents=True)
        self.plan_path = self.dependency / 'plan.json'
        self.plan_path.write_text(json.dumps(self.plan))
        self.publish_raw()

    def publish_raw(self):
        # The execute step's existing exact copy of the retained native receipt.
        (self.receipts / 'receipt.json').write_bytes(self.public_path.read_bytes())

    def worker(self, policy=None):
        return normal.worker_qualify(self.root, self.plan_path, 'synthetic', self.dependency / 'evidence',
                                     self.apparmor, self.receipts, policy=policy or self.inventory)

    def provision_apparmor(self, status='removed', after=b'unconfined\n'):
        self.apparmor.mkdir(exist_ok=True)
        (self.apparmor / 'setup.json').write_text(json.dumps(dict(name='nostrc-job-1')))
        (self.apparmor / 'teardown.json').write_text(json.dumps(dict(kind='namespace_apparmor_teardown_v1', status=status)))
        (self.apparmor / 'profiles-before.txt').write_bytes(b'unconfined\n')
        (self.apparmor / 'profiles-after.txt').write_bytes(after)
        self.adapter['namespace_apparmor_profile'] = 'nostrc-job-1'
        self.adapter_path.write_text(json.dumps(self.adapter))

    def download(self, name=None, profile='synthetic'):
        """actions/upload-artifact of receipts/ then download-artifact pattern: regular files per artifact dir."""
        base = Path(self.temp.name).resolve() / 'aggregate' / 'receipts'
        target = base / (name or 'dependency-receipt-%s-synthetic-1' % self.plan['plan_id']) / profile
        shutil.copytree(self.receipts, target)
        return base

    def aggregate(self, base, repo=None):
        paths = sorted(base.glob('**/receipt.json'))
        with normal.transported_receipts(paths, repo=repo or self.root, plan=self.plan) as receipts:
            return paths, s.aggregate(self.inventory, self.plan, receipts, complete=True)

    def assert_raw_retained(self):
        self.assertEqual((self.receipts / 'receipt.json').read_bytes(), self.public_path.read_bytes())
        for name in (normal.ARCHIVE, normal.ORIGINAL, '.' + normal.ARCHIVE + '.partial', '.receipt.json.normal'):
            self.assertFalse((self.receipts / name).exists(), name)

    def rewrite(self, base, mutate, *, refresh=True):
        """Mutate the downloaded bundle; optionally make receipt hashes self-consistent again."""
        lane = next(base.glob('*/synthetic'))
        archive = lane / normal.ARCHIVE
        manifest, contents = split(archive)
        mutate(manifest, contents)
        data, raw = join(manifest, contents)
        archive.write_bytes(data)
        if refresh:
            receipt = json.loads((lane / 'receipt.json').read_text())
            import hashlib
            receipt['transport'].update(archive_bytes=len(data), archive_sha256=hashlib.sha256(data).hexdigest(),
                                        manifest_sha256=hashlib.sha256(raw).hexdigest(), members=len(manifest['members']))
            receipt['qualification_hash'] = s.digest({k: v for k, v in receipt.items() if k != 'qualification_hash'})
            (lane / 'receipt.json').write_text(json.dumps(receipt))

    # ------------------------------------------------------------------ cases --
    def test_worker_publishes_one_portable_receipt_and_aggregate_never_reopens_worker_paths(self):
        raw = self.public_path.read_bytes()
        result = self.worker()
        self.assertEqual(result['receipt_kind'], normal.KIND)
        self.assertEqual(json.loads((self.receipts / 'receipt.json').read_text()), result)
        self.assertEqual((self.receipts / normal.ORIGINAL).read_bytes(), raw)
        self.assertEqual(self.public_path.read_bytes(), raw)
        self.assertEqual(sorted(p.name for p in self.receipts.iterdir()),
                         sorted(['receipt.json', normal.ORIGINAL, normal.ARCHIVE]))
        base = self.download()
        # The original worker tree is gone: only the downloaded bundle can satisfy reads.
        shutil.move(str(self.dependency), str(Path(self.temp.name) / 'worker-gone'))
        paths, aggregate = self.aggregate(base)
        self.assertEqual(len(paths), 1)
        self.assertTrue(aggregate['passed'], aggregate['errors'])
        self.assertTrue(aggregate['complete_gate_eligible'])
        # No caller fallback: outside a loaded bundle a transported receipt is refused.
        with self.assertRaisesRegex(ValueError, 'verified portable bundle'):
            normal.validate_receipt(self.inventory, self.plan, result)

    def test_nested_raw_duplicate_and_untransported_receipts_refuse(self):
        self.worker()
        base = self.download()
        lane = next(base.glob('*/synthetic'))
        (lane / 'nested').mkdir()
        shutil.copyfile(lane / normal.ORIGINAL, lane / 'nested' / 'receipt.json')
        paths, aggregate = self.aggregate(base)
        self.assertEqual(len(paths), 2)
        self.assertFalse(aggregate['passed'])
        self.assertTrue(any('duplicate/unexpected lane' in e for e in aggregate['errors']), aggregate['errors'])
        shutil.rmtree(lane / 'nested')
        # An in-place (path-bound) normal receipt is never accepted by the portable aggregate.
        receipt = json.loads((lane / 'receipt.json').read_text())
        in_place = {k: v for k, v in receipt.items() if k != 'transport'}
        in_place['qualification_hash'] = s.digest({k: v for k, v in in_place.items() if k != 'qualification_hash'})
        (lane / 'receipt.json').write_text(json.dumps(in_place))
        self.assertFalse(self.aggregate(base)[1]['passed'])

    def test_tampered_missing_extra_duplicate_traversal_and_symlink_members_refuse(self):
        self.worker()
        log = 'run/0/lane-synthetic/' + str(Path(self.raw['evidence_dir']).relative_to(self.work) / 'ctest.log')
        def edit(name, data):
            def mutate(manifest, contents):
                contents[name] = data
                member = next(m for m in manifest['members'] if m['name'] == name)
                import hashlib
                member.update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
            return mutate
        def drop(name):
            def mutate(manifest, contents):
                manifest['members'] = [m for m in manifest['members'] if m['name'] != name]
                contents.pop(name, None)
            return mutate
        def add(member, data=b''):
            def mutate(manifest, contents):
                manifest['members'].append(member)
                if member['kind'] == 'file':
                    contents[member['name']] = data
            return mutate
        import hashlib
        extra = lambda name: dict(name=name, kind='file', size=1, sha256=hashlib.sha256(b'x').hexdigest(), executable=False)
        cases = {
            'tampered member': (edit(log, b'forged output\n'), True),
            'missing member': (drop(log), True),
            'extra sealed member': (add(extra('run/0/lane-synthetic/build/extra'), b'x'), True),
            'cross-lane member': (add(dict(name='run/0/lane-other', kind='dir')), True),
            'duplicate member': (lambda m, c: m['members'].append(dict(m['members'][1])), True),
            'traversal member': (add(extra('run/0/lane-synthetic/../escape'), b'x'), True),
            'absolute symlink': (add(dict(name='run/0/lane-synthetic/alias', kind='symlink', target='/etc/passwd')), True),
            'escaping symlink': (add(dict(name='run/0/lane-synthetic/alias', kind='symlink', target='../../../../x')), True),
            'unrefreshed archive bytes': (edit(log, b'forged output\n'), False),
        }
        for label, (mutate, refresh) in cases.items():
            with self.subTest(label):
                base = self.download(name='dependency-receipt-%s-synthetic-%d' % (self.plan['plan_id'], len(label)))
                self.rewrite(base, mutate, refresh=refresh)
                paths, aggregate = self.aggregate(base)
                self.assertEqual(len(paths), 1)
                self.assertFalse(aggregate['passed'], label)
                shutil.rmtree(base.parent)

    def test_oversized_inputs_refuse_before_allocation(self):
        self.worker()
        base = self.download()
        lane = next(base.glob('*/synthetic'))
        with mock.patch.dict(normal.LIMITS, archive_bytes=1024):
            self.assertFalse(self.aggregate(base)[1]['passed'])
        data = bytearray((lane / normal.ARCHIVE).read_bytes())
        data[len(normal.MAGIC):len(normal.MAGIC) + 8] = (1 << 62).to_bytes(8, 'big')  # huge manifest length
        (lane / normal.ARCHIVE).write_bytes(bytes(data))
        receipt = json.loads((lane / 'receipt.json').read_text())
        import hashlib
        receipt['transport']['archive_sha256'] = hashlib.sha256(bytes(data)).hexdigest()
        receipt['qualification_hash'] = s.digest({k: v for k, v in receipt.items() if k != 'qualification_hash'})
        (lane / 'receipt.json').write_text(json.dumps(receipt))
        paths, aggregate = self.aggregate(base)
        self.assertFalse(aggregate['passed'])
        self.assertTrue(any('manifest exceeds limit' in e for e in aggregate['errors']), aggregate['errors'])

    def test_wrong_artifact_plan_and_profile_placement_refuse(self):
        self.worker()
        for name, profile in (('dependency-receipt-%s-other-1' % self.plan['plan_id'], 'synthetic'),
                              ('dependency-receipt-%s-synthetic-1' % ('0' * 16), 'synthetic'),
                              ('dependency-receipt-%s-synthetic-1' % self.plan['plan_id'], 'other')):
            with self.subTest(name=name, profile=profile):
                base = self.download(name=name, profile=profile)
                self.assertFalse(self.aggregate(base)[1]['passed'])
                shutil.rmtree(base.parent)

    def test_source_closure_is_checked_against_the_exact_aggregate_checkout(self):
        self.worker()
        base = self.download()
        source = self.root / 'shared.h'
        original = source.read_bytes()
        source.write_text('#define SHARED 43\n')       # dirty checkout
        self.assertFalse(self.aggregate(base)[1]['passed'])
        self.git('commit', '-qam', 'later commit')        # different HEAD than tested
        self.assertFalse(self.aggregate(base)[1]['passed'])
        self.git('reset', '-q', '--hard', 'HEAD~1')
        self.assertEqual(source.read_bytes(), original)
        self.assertTrue(self.aggregate(base)[1]['passed'])

    def test_sealed_source_claims_must_equal_the_tested_commit(self):
        # A self-consistent seal over source bytes that differ from the committed
        # tested SHA: the bundle's own hashes cannot authenticate it.
        import hashlib
        source = self.root / 'shared.h'
        original, forged = source.read_bytes(), b'#define SHARED 43\n'
        def rebind(value):
            if isinstance(value, dict):
                if str(source) in value.get('input_hashes', {}):
                    value['input_hashes'][str(source)] = hashlib.sha256(forged).hexdigest()
                    if 'audit_hash' in value:
                        value['audit_hash'] = s.digest({k: v for k, v in value.items() if k != 'audit_hash'})
                for child in value.values():
                    rebind(child)
            elif isinstance(value, list):
                for child in value:
                    rebind(child)
        rebind(self.original)
        source.write_bytes(forged)
        self.seal()
        source.write_bytes(original)
        self.publish_raw()
        with self.assertRaisesRegex(ValueError, 'sealed source input differs'):
            self.worker()                                   # worker self-check refuses first
        self.assert_raw_retained()
        with mock.patch.object(normal, 'verify_source_closure'):
            self.worker()                                   # bypass only the worker self-check
        base = self.download()
        paths, aggregate = self.aggregate(base)
        self.assertFalse(aggregate['passed'])
        self.assertTrue(any('sealed source input differs' in e for e in aggregate['errors']), aggregate['errors'])

    def rebind_audits(self, path, data):
        import hashlib
        def visit(value):
            if isinstance(value, dict):
                if 'input_hashes' in value and isinstance(value['input_hashes'], dict) and value.get('passed') is True:
                    value['input_hashes'][str(path)] = hashlib.sha256(data).hexdigest()
                    value['audit_hash'] = s.digest({k: v for k, v in value.items() if k != 'audit_hash'})
                for child in value.values():
                    visit(child)
            elif isinstance(value, list):
                for child in value:
                    visit(child)
        visit(self.original)

    def worker_then_aggregate_refuses(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.worker()                                   # worker self-check refuses first
        self.assert_raw_retained()
        with mock.patch.object(normal, 'verify_source_closure'):
            self.worker()                                   # bypass only the worker self-check
        paths, aggregate = self.aggregate(self.download())
        self.assertFalse(aggregate['passed'])
        self.assertTrue(any(message in e for e in aggregate['errors']), aggregate['errors'])

    def test_incomplete_sealed_source_claims_refuse(self):
        import test_selection_services as services
        original = services._source_claims
        def incomplete(result, source):
            claims = original(result, source)
            claims.pop(sorted(claims)[0])
            return claims
        with mock.patch.object(services, '_source_claims', side_effect=incomplete):
            self.seal()
        self.publish_raw()
        self.worker_then_aggregate_refuses('sealed source claims differ from the sealed raw input audits')

    def test_untracked_source_claim_refuses(self):
        extra = self.root / 'untracked.h'
        extra.write_bytes(b'#define UNTRACKED 1\n')
        self.rebind_audits(extra, extra.read_bytes())
        self.seal(); self.publish_raw()
        self.worker_then_aggregate_refuses('not a tracked file of the tested commit')

    def test_unadmitted_lane_entries_and_symlink_chains_refuse(self):
        self.worker()
        lane = 'run/0/lane-synthetic'
        cases = {
            'unadmitted top-level link': lambda m, c: m['members'].append(dict(name=lane + '/a', kind='symlink', target='../..')),
            'unadmitted top-level file': lambda m, c: (m['members'].append(dict(name=lane + '/note', kind='file', size=0,
                sha256='e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855', executable=False)), c.update({lane + '/note': b''})),
            'resolved symlink chain': lambda m, c: m['members'].extend([
                dict(name=lane + '/build/zz-a', kind='symlink', target='../..'),
                dict(name=lane + '/build/zz-b', kind='symlink', target='zz-a/../..')]),
        }
        for index, (label, mutate) in enumerate(cases.items()):
            with self.subTest(label):
                base = self.download(name='dependency-receipt-%s-synthetic-%d' % (self.plan['plan_id'], index + 1))
                self.rewrite(base, mutate)
                paths, aggregate = self.aggregate(base)
                self.assertFalse(aggregate['passed'])
                self.assertTrue(any('unadmitted' in e or 'resolves outside' in e for e in aggregate['errors']), aggregate['errors'])
                shutil.rmtree(base.parent)

    def test_lifecycle_teardown_and_packaging_failures_leave_raw_failure_evidence(self):
        failures = {
            'mdk recovery outside namespace': lambda: (self.adapter['executions'][0]['lanes'][0].update(
                mdk_recovery=[dict(closed_in_namespace=False, recovery_exit=0)]), self.adapter_path.write_text(json.dumps(self.adapter))),
            'apparmor teardown failed': lambda: self.provision_apparmor(status='failed'),
            'apparmor policy delta': lambda: self.provision_apparmor(after=b'unconfined\nleftover\n'),
            'provisioned but not bound': lambda: (self.provision_apparmor(), self.adapter.pop('namespace_apparmor_profile'),
                                                  self.adapter_path.write_text(json.dumps(self.adapter))),
        }
        pristine = copy.deepcopy(self.adapter)
        for label, fail in failures.items():
            with self.subTest(label):
                self.adapter = copy.deepcopy(pristine); self.adapter_path.write_text(json.dumps(self.adapter))
                shutil.rmtree(self.apparmor, ignore_errors=True)
                fail()
                with self.assertRaises((ValueError, KeyError)):
                    self.worker()
                self.assert_raw_retained()
        # Packaging refusal (sealed absolute symlink is not portable) and a failed
        # relocated self-check both keep the raw receipt and no published bundle.
        self.adapter = copy.deepcopy(pristine); self.adapter_path.write_text(json.dumps(self.adapter))
        shutil.rmtree(self.apparmor, ignore_errors=True)
        with mock.patch.object(normal, 'verify_source_closure', side_effect=ValueError('self-check failed')):
            with self.assertRaisesRegex(ValueError, 'self-check failed'):
                self.worker()
        self.assert_raw_retained()
        target = next(p for p in (self.work / 'build').iterdir() if p.is_file())
        (self.work / 'build' / 'absolute-alias').symlink_to(target)
        self.seal(); self.publish_raw()
        with self.assertRaisesRegex(ValueError, 'absolute/escaping'):
            self.worker()
        self.assert_raw_retained()

    def test_apparmor_teardown_evidence_travels_and_is_requalified(self):
        self.provision_apparmor()
        result = self.worker()
        self.assertEqual(sorted(result['apparmor_evidence']), sorted(normal.APPARMOR_FILES))
        base = self.download()
        shutil.move(str(self.dependency), str(Path(self.temp.name) / 'worker-gone'))
        self.assertTrue(self.aggregate(base)[1]['passed'])

    def test_omission_legacy_prepared_and_macos_profiles_cannot_qualify(self):
        self.original['backend_receipt'].update(baseline_exclusions=[dict(authority='reviewed_diagnostic_nonmember_subset_v1')])
        self.seal(); self.publish_raw()
        with self.assertRaises(ValueError):
            self.worker()
        self.assert_raw_retained()
        policy = copy.deepcopy(self.inventory)
        policy['profiles']['synthetic'].pop(normal.FIELD)
        self.assertIsNone(self.worker(policy))              # no authored contract: raw untouched
        self.assert_raw_retained()
        for change in (dict(execution_domain='prepared_peer_linux_v1'), dict(platform='macos'), dict(architecture='arm64')):
            with self.subTest(change=change):
                policy = copy.deepcopy(self.inventory); policy['profiles']['synthetic'].update(change)
                with self.assertRaises(ValueError):
                    self.worker(policy)
                self.assert_raw_retained()


class WorkflowWiring(unittest.TestCase):
    def test_worker_qualifies_after_teardown_and_aggregate_owns_portable_loading(self):
        worker = (ROOT / '.github/workflows/dependency-worker.yml').read_text()
        execute = worker.index('- name: Resolve, audit, build and execute exact selected checks')
        teardown = worker.index("- name: Remove only this job's AppArmor profile after its consumers exited")
        qualify = worker.index('- name: Qualify normal lane after pidns exit and AppArmor teardown')
        upload = worker.index('- name: Retain incomplete evidence on failure or cancellation')
        self.assertLess(execute, teardown); self.assertLess(teardown, qualify); self.assertLess(qualify, upload)
        step = worker[qualify:upload]
        self.assertIn("if: success() && runner.os == 'Linux'", step)
        self.assertIn('scripts/test_selection_execution.py worker-qualify', step)
        self.assertIn('--receipts-dir "$RUNNER_TEMP/dependency/receipts/$PROFILE"', step)
        gate = (ROOT / '.github/workflows/dependency-tests.yml').read_text()
        checkout = gate[gate.index('name: Dependency gate'):]
        self.assertIn('submodules: recursive', checkout[:checkout.index('dependency_contract.py aggregate')])
        contract = (ROOT / '.github/workflows/dependency_contract.py').read_text()
        self.assertIn('with transported_receipts(paths, repo=ROOT, plan=plan) as receipts:', contract)
        self.assertIn('glob("**/receipt.json")', contract)


if __name__ == '__main__':
    unittest.main()
