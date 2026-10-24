"""Small real builds/seals plus synthetic supervisor results; NOT kernel/hosted proof."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import tempfile
import unittest
from unittest import mock

from realization_support import Project, ROOT
from selector_support import s, change
import test_selection_realization as r
import test_selection_services as services
import test_selection_execution as normal


class FixtureDirectory:
    """A restored per-case fixture copy at the genuine fixture's own path."""

    def __init__(self, path):
        self.name = str(path)

    def cleanup(self):
        shutil.rmtree(self.name, ignore_errors=True)


def forget_fixture(cls, genuine):
    """Drop only the exact class-cached snapshot this class cleanup belongs to."""
    if cls.__dict__.get('_genuine_fixture') is genuine:
        del cls._genuine_fixture


def fixture_digest(root):
    """Exact names, kinds, modes, nanosecond mtimes, bytes and link targets under root."""
    digest = hashlib.sha256()
    for directory, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(dirs + files):
            path = Path(directory) / name
            info = path.lstat()
            entry = [os.fsencode(path.relative_to(root)), oct(info.st_mode).encode()]
            if path.is_symlink():
                entry.append(os.fsencode(os.readlink(path)))
            elif path.is_file():
                entry += [str(info.st_mtime_ns).encode(), hashlib.sha256(path.read_bytes()).digest()]
            digest.update(b'\0'.join(entry) + b'\n')
    return digest.hexdigest()


class NormalFixture(Project):
    # Lane layout hook: the default is the original synthetic layout; the
    # portable-transport tests use the worker's <run>/0/lane-<id> layout.
    LANE = ('lane',)
    ADAPTER_UP = 0

    def setUp(self):
        """One genuine fixture per concrete class, a fresh exact copy per case.

        The first case of each class runs the original construction below (real
        Git history, configure, build, CTest run and seal). That tree is kept as an
        immutable snapshot; every case (including the first) then starts from its
        own copy restored at the identical absolute path, with bytes, modes, link
        targets and nanosecond mtimes preserved (proved by digest), its own deep-copied
        instance state, native-profile patch and freshly loaded CLI module. Cases
        still mutate, re-run, re-seal and verify only their own copy; no runtime
        cache or receipt is shared or trusted across cases."""
        cls = type(self)
        genuine = cls.__dict__.get('_genuine_fixture')
        if genuine is None:
            self.construct_fixture()
            genuine = self.snapshot_fixture()
            cls._genuine_fixture = genuine
            # The snapshot store is removed by a class cleanup, so forget exactly this
            # cached state with it: running the same class again constructs afresh.
            cls.addClassCleanup(forget_fixture, cls, genuine)
        self.restore_fixture(genuine)

    def patch_native_profile(self):
        original = r._validate_native_profile
        def native(profile, *args, **kwargs):
            return original(dict(profile, platform={'Darwin': 'macos', 'Linux': 'linux'}[platform.system()],
                architecture={'aarch64': 'arm64'}.get(platform.machine(), platform.machine()),
                compiler='clang' if platform.system() == 'Darwin' else 'gcc'), *args, **kwargs)
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(r, '_validate_native_profile', side_effect=native).start()

    def load_cli(self):
        spec = importlib.util.spec_from_file_location('normal_adapter', ROOT / 'scripts/test-plan.py')
        self.cli = importlib.util.module_from_spec(spec); spec.loader.exec_module(self.cli)

    def snapshot_fixture(self):
        root = Path(self.temp.name)
        store = Path(tempfile.mkdtemp(prefix='normal-fixture-'))
        type(self).addClassCleanup(shutil.rmtree, store, True)
        shutil.copytree(root, store / 'tree', symlinks=True)
        state = {key: value for key, value in vars(self).items()
                 if not key.startswith('_') and key not in ('temp', 'cli')}  # TestCase bookkeeping is not fixture state
        return dict(root=root, tree=store / 'tree', digest=fixture_digest(root), state=copy.deepcopy(state))

    def restore_fixture(self, genuine):
        root = genuine['root']
        constructing = hasattr(self, 'temp')
        if not constructing:
            self.temp = FixtureDirectory(root)
            self.addCleanup(self.temp.cleanup)
        shutil.rmtree(root, ignore_errors=not constructing)
        shutil.copytree(genuine['tree'], root, symlinks=True)
        self.assertEqual(fixture_digest(root), genuine['digest'], 'restored fixture differs from the genuine snapshot')
        self.__dict__.update(copy.deepcopy(genuine['state']))
        if not constructing:
            self.patch_native_profile()
        self.load_cli()

    def construct_fixture(self):
        super().setUp()
        self.work = Path(self.temp.name).resolve().joinpath(*self.LANE)
        self.work.mkdir(parents=True)
        self.build, self.evidence = self.work / 'build', self.work / 'evidence'
        self.patch_native_profile()
        self.profile.update(platform='linux', architecture='x86_64', compiler='gcc', build_type='Debug',
            instrumentation='none', feature_variants=[], setup='synthetic-normal', timeout_seconds=60, capabilities=[],
            local_availability='host',
            build_parallelism=2, test_parallelism=1, normal_execution=copy.deepcopy(normal.MARKER))
        self.profile['configure_args'] += ['-DCMAKE_BUILD_TYPE=Debug']
        self.profile['environment']['TEST_DISPLAY'] = '1'
        for node, record in self.inventory['nodes'].items():
            record.update(owner_paths=[node], rationale='synthetic ownership')
        for rule in self.inventory['path_rules']:
            rule.setdefault('effect', 'impact'); rule.setdefault('reason', 'synthetic source owner')
        for check in self.inventory['checks'].values():
            check.update(requirement='blocking', cost='ordinary', expected_outcome='pass')
        self.inventory['checks'] = {'fixture.' + name.replace('-', '_'): check
                                    for name, check in self.inventory['checks'].items()}
        self.inventory.update(schema_version=1, profiles={'synthetic': self.profile}, release_components={})
        self.inventory['checks']['default'] = dict(kind='build_contract', profiles=['synthetic'],
            nodes=['application', 'core', 'vectors', 'testing'], requirement='blocking', cost='ordinary',
            expected_outcome='pass', build_scope='default', build_targets=[],
            command=['cmake', '--build', '{build}', '--config', 'Debug'], working_directory='{source}',
            environment={}, runtime_inputs=[])
        self.prepare_source()
        self.identity = dict(kind='committed', sha=self.git('rev-parse', 'HEAD').decode().strip())
        self.plan = self.make_plan()
        self.load_cli()
        self.execute()
        self.public_path = self.work / 'native-receipt.json'
        self.adapter_path = self.work.parents[self.ADAPTER_UP] / 'adapter-receipt.json'
        self.adapter = dict(adapter='test-gate', custody_mode='pidns', status='partial', errors=[], bypasses={},
            executions=[dict(plan_id=self.plan['plan_id'], source_identity=self.identity, status='partial',
                lanes=[dict(profile='synthetic', status='passed', evidence=str(self.work),
                    retained_evidence=str(self.work / 'native-final.json'), normalized_receipt=str(self.public_path),
                    mdk_recovery=[])])])
        self.seal()

    def prepare_source(self):
        """Hook: committed source history before identity/plan (default: none)."""

    def execute(self):
        """Default: direct engine sequence (existing full-normal fixture)."""
        self.real = r.configure_profile(self.root, self.build, 'synthetic', self.profile, self.identity,
                                        s.digest(self.inventory), inventory=self.inventory, diagnostic_boundary='linux_pidns_v1')
        self.resolved = r.resolve_checks(self.inventory, self.plan, self.real, 'synthetic')
        pre = r.check_inputs(self.inventory, self.real)
        self.assertTrue(pre['passed'], pre['gaps'])
        r.build_resolved(self.resolved, self.build)
        post = r.check_inputs(self.inventory, self.real, after_build=True)
        self.assertTrue(post['passed'], post['gaps'])
        self.raw = r.run_resolved(self.resolved, self.build, self.evidence, input_audit=post,
                                  input_audit_before_build=pre, build=False)
        self.assertEqual(self.raw['outcome'], 'passed')
        self.original = self.cli.normalize_receipt(self.inventory, self.plan, self.resolved, self.raw)

    def execute_production(self):
        """The pidns gate's inner production sequence: test-plan.py main(resolve) then main(run)."""
        plan_path = Path(self.temp.name).resolve() / 'selected-plan.json'
        plan_path.write_text(json.dumps(self.plan))
        # Same `common` as test-gate.sh:622-627: the pidns boundary goes to BOTH resolve and run.
        common = ['--repo', str(self.root), '--profile', 'synthetic', '--plan', str(plan_path),
                  '--build-dir', str(self.build), '--evidence-dir', str(self.evidence),
                  '--diagnostic-boundary', 'linux_pidns_v1']
        resolutions, results = [], []
        self.assertEqual(self.cli.main(['resolve', *common, '--configure',
                                        '--output', str(self.work / 'resolved.json')], result_observer=resolutions.append), 0)
        self.assertEqual(self.cli.main(['run', *common, '--resolved', str(self.work / 'resolved.json'), '--phase', 'all',
                                        '--timeout', str(self.profile['timeout_seconds']),
                                        '--output', str(self.work / 'receipt.json')], result_observer=results.append), 0)
        self.resolved, self.original = resolutions[-1], results[-1]
        self.raw = self.original['backend_receipt']
        self.real = r.load_realization(self.build, self.identity)

    def make_plan(self):
        return s.make_plan(self.inventory, [change('main.c')], head=self.identity['sha'], base='b' * 40,
                           mode='broad', source_identity=self.identity)

    def seal(self):
        # Exercise real retained byte verification. The outer supervisor result
        # is synthetic: these tests cannot establish a Linux wait barrier.
        for name in ('native-seal.json', 'native-final.json'):
            (self.work / name).unlink(missing_ok=True)
        import shutil
        shutil.rmtree(self.work / 'source-inputs', ignore_errors=True)
        # The CLI's own trusted writer; the seal requires its exact bytes.
        s.write_json(self.work / 'receipt.json', self.original)
        s.write_json(self.work / 'resolved.json', self.resolved)
        anchor = services.seal_native_lane(self.work, self.root, self.original, self.resolved)
        (self.work / 'native-final.json').write_text(json.dumps(services.attest_native_lane(self.work, anchor)))
        self.public_path.write_text(json.dumps(services.native_public_receipt(self.work)))
        self.adapter_path.write_text(json.dumps(self.adapter))

    def qualify(self):
        return normal.qualify(self.inventory, self.plan, str(self.public_path), str(self.adapter_path))


class NormalExecutionTests(NormalFixture):
    def test_independent_normal_receipt_and_aggregation_keep_original_bytes(self):
        before = self.public_path.read_bytes()
        result = self.qualify()
        self.assertEqual(result['receipt_kind'], normal.KIND)
        self.assertEqual(result['executed_registrations'], sorted(t['name'] for t in self.real['tests']))
        normal.validate_receipt(self.inventory, self.plan, result)
        aggregate = s.aggregate(self.inventory, self.plan, [result], complete=True)
        self.assertTrue(aggregate['passed'], aggregate['errors'])
        self.assertTrue(aggregate['complete_gate_eligible'])
        self.assertEqual(self.public_path.read_bytes(), before)
        forged = copy.deepcopy(result); forged['qualification_hash'] = '0' * 64
        self.assertFalse(s.aggregate(self.inventory, self.plan, [forged], complete=True)['passed'])

    def test_post_exit_barrier_and_recovery_cannot_be_inferred_from_a_flag(self):
        for mutate in (lambda a: a.update(custody_mode='legacy'), lambda a: a.update(status='failed'),
                       lambda a: a['executions'][0]['lanes'][0].update(status='failed'),
                       lambda a: a['executions'][0]['lanes'][0].update(mdk_recovery=[dict(closed_in_namespace=False, recovery_exit=0)])):
            with self.subTest(mutate=mutate):
                changed = copy.deepcopy(self.adapter); mutate(changed)
                self.adapter_path.write_text(json.dumps(changed))
                with self.assertRaises(ValueError): self.qualify()
        self.adapter_path.write_text(json.dumps(self.adapter))
        (self.work / 'native-final.json').unlink()
        with self.assertRaises((OSError, RuntimeError, ValueError)): self.qualify()

    def test_resealed_missing_audits_skips_and_omissions_still_refuse(self):
        original = copy.deepcopy(self.original)
        mutations = [lambda raw: raw.pop('input_audit_before_build'),
                     lambda raw: raw['input_audit'].update(header_entries=0),
                     lambda raw: raw['input_audit']['gaps'].append(dict(kind='unowned_input')),
                     lambda raw: raw['observed_executions'].update(gui='skipped'),
                     lambda raw: raw.update(baseline_exclusions=[dict(authority='reviewed_diagnostic_nonmember_subset_v1')]),
                     lambda raw: raw['tests'].pop(),
                     lambda raw: raw.update(ctest_process={}),
                     lambda raw: raw.update(hidden=dict(authority='reviewed_diagnostic_nonmembers_v1', complete=False))]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                self.original = copy.deepcopy(original); mutate(self.original['backend_receipt'])
                self.seal()
                with self.assertRaises((KeyError, ValueError, RuntimeError)): self.qualify()

    def test_retained_source_and_process_bytes_drift_refuse(self):
        self.qualify()
        logfile = Path(self.raw['evidence_dir']) / 'ctest.log'
        logfile.write_bytes(logfile.read_bytes() + b'modified\n')
        with self.assertRaises((RuntimeError, ValueError)): self.qualify()

    def test_no_authored_contract_or_wrong_source_can_borrow_success(self):
        policy = copy.deepcopy(self.inventory)
        policy['profiles']['synthetic'].pop(normal.FIELD)
        with self.assertRaises(ValueError): normal.qualify(policy, self.plan, str(self.public_path), str(self.adapter_path))
        plan = copy.deepcopy(self.plan); plan['source_identity']['sha'] = 'f' * 40
        with self.assertRaises(ValueError): normal.qualify(self.inventory, plan, str(self.public_path), str(self.adapter_path))


class NormalContractSemantics(unittest.TestCase):
    def test_legacy_minimal_cli_does_not_require_unused_optional_normal_module(self):
        import builtins
        from selector_support import policy
        original = builtins.__import__
        def missing_extension(name, *args, **kwargs):
            if name == 'test_selection_execution':
                raise ModuleNotFoundError('normal extension deliberately absent')
            return original(name, *args, **kwargs)
        legacy = policy()
        with mock.patch('builtins.__import__', side_effect=missing_extension):
            s.validate_inventory(legacy)
            legacy['profiles']['linux']['normal_execution'] = copy.deepcopy(normal.MARKER)
            with self.assertRaisesRegex(ModuleNotFoundError, 'deliberately absent'):
                s.validate_inventory(legacy)


    def test_expected_incompatibility_cannot_become_unexpected_success(self):
        check = dict(expected_outcome=dict(case='mdk09_incompatibility', exit_code=77))
        normal._expected_check_outcome(check, dict(outcome='expected_incompatibility',
                                                  exception_case='mdk09_incompatibility', exit_code=77))
        for result in (dict(outcome='passed'), dict(outcome='expected_incompatibility',
                       exception_case='mdk09_incompatibility', exit_code=0), dict(outcome='skipped')):
            with self.subTest(result=result), self.assertRaises(ValueError):
                normal._expected_check_outcome(check, result)
        with self.assertRaises(ValueError):
            normal._expected_check_outcome(dict(expected_outcome='pass'),
                dict(outcome='expected_incompatibility', exception_case='mdk09_incompatibility', exit_code=77))

    def test_committed_source_boundary_full_unchanged_selected_only_new(self):
        full = {normal.FIELD: copy.deepcopy(normal.MARKER)}
        selected = dict(full, **{normal.SELECTED_FIELD: copy.deepcopy(normal.SELECTED_MARKER)})
        committed = dict(kind='committed', sha='a' * 40)
        dirty = dict(kind='dirty', sha='a' * 40, snapshot_digest='d' * 64)
        old = 'normal full-profile qualification requires a fresh committed broad plan'
        for profile in (full, selected):
            self.assertEqual(normal._scope({}, dict(effective_mode='broad', source_identity=committed), {}, profile, None), 'full')
            with self.assertRaisesRegex(ValueError, old):      # exactly the 1640 full condition/message
                normal._scope({}, dict(effective_mode='broad', source_identity=dirty), {}, profile, None)
        for identity in (committed, dirty):
            with self.assertRaisesRegex(ValueError, old):      # no selected contract: affected stays refused as before
                normal._scope({}, dict(effective_mode='affected', requested_mode='affected', source_identity=identity), {}, full, None)
        with self.assertRaisesRegex(ValueError, 'selected-normal requires a committed source'):
            normal._scope({}, dict(effective_mode='affected', requested_mode='affected', source_identity=dirty), {}, selected, None)
        with self.assertRaisesRegex(ValueError, 'portable normal proof requires a committed source'):
            normal.verify_source_closure(None, '.', dict(source_identity=dirty))   # dirty never gets transport authority

    def test_unselected_mdk_lifecycle_must_leave_no_trace(self):
        descriptors = {'gui': {'requires': ['display']}, 'groundhog-mdk011-interop-image': {'requires': ['docker']}}
        clean = dict(required_capabilities=['display'], runtime_capability_evidence={})
        normal._require_no_mdk_lifecycle(clean, ['gui'], descriptors, [])
        residues = {
            'runtime state': (dict(clean, mdk_runtime={'status': 'passed'}), ['gui'], []),
            'docker evidence': (dict(clean, runtime_capability_evidence={'docker': {}}), ['gui'], []),
            'docker capability': (dict(clean, required_capabilities=['display', 'docker']), ['gui'], []),
            'journal/recovery': (clean, ['gui'], [dict(closed_in_namespace=True)]),
            'image fixture executed': (clean, ['gui', 'groundhog-mdk011-interop-image'], []),
        }
        for label, (raw, closure, recovery) in residues.items():
            with self.subTest(label), self.assertRaisesRegex(ValueError, 'unselected original MDK lifecycle'):
                normal._require_no_mdk_lifecycle(raw, closure, descriptors, recovery)

    def test_signer_uses_canonical_source_expansion_and_test_phase_projection(self):
        profile = dict(environment={'ASAN_OPTIONS': 'detect_leaks=1', 'LSAN_OPTIONS': 'wrong'},
                       phase_environments={'configure': {}, 'build': {},
                                           'test': {'LSAN_OPTIONS': 'suppressions={source_root}/tests/lsan.supp'}})
        self.assertEqual(normal._sanitizer_environment(profile, '/fresh/source path'),
                         {'ASAN_OPTIONS': 'detect_leaks=1',
                          'LSAN_OPTIONS': 'suppressions=/fresh/source path/tests/lsan.supp'})
