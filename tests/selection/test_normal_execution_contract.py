"""Small real builds/seals plus synthetic supervisor results; NOT kernel/hosted proof."""
import copy
import importlib.util
import json
from pathlib import Path
import platform
import unittest
from unittest import mock

from realization_support import Project, ROOT
from selector_support import s, change
import test_selection_realization as r
import test_selection_services as services
import test_selection_execution as normal


class NormalFixture(Project):
    # Lane layout hook: the default is the original synthetic layout; the
    # portable-transport tests use the worker's <run>/0/lane-<id> layout.
    LANE = ('lane',)
    ADAPTER_UP = 0

    def setUp(self):
        super().setUp()
        self.work = Path(self.temp.name).resolve().joinpath(*self.LANE)
        self.work.mkdir(parents=True)
        self.build, self.evidence = self.work / 'build', self.work / 'evidence'
        original = r._validate_native_profile
        def native(profile, *args, **kwargs):
            return original(dict(profile, platform={'Darwin': 'macos', 'Linux': 'linux'}[platform.system()],
                architecture={'aarch64': 'arm64'}.get(platform.machine(), platform.machine()),
                compiler='clang' if platform.system() == 'Darwin' else 'gcc'), *args, **kwargs)
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(r, '_validate_native_profile', side_effect=native).start()
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
        self.identity = dict(kind='committed', sha=self.git('rev-parse', 'HEAD').decode().strip())
        self.plan = s.make_plan(self.inventory, [change('main.c')], head=self.identity['sha'], base='b' * 40,
                                mode='broad', source_identity=self.identity)
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
        spec = importlib.util.spec_from_file_location('normal_adapter', ROOT / 'scripts/test-plan.py')
        self.cli = importlib.util.module_from_spec(spec); spec.loader.exec_module(self.cli)
        self.original = self.cli.normalize_receipt(self.inventory, self.plan, self.resolved, self.raw)
        self.public_path = self.work / 'native-receipt.json'
        self.adapter_path = self.work.parents[self.ADAPTER_UP] / 'adapter-receipt.json'
        self.adapter = dict(adapter='test-gate', custody_mode='pidns', status='partial', errors=[], bypasses={},
            executions=[dict(plan_id=self.plan['plan_id'], source_identity=self.identity, status='partial',
                lanes=[dict(profile='synthetic', status='passed', evidence=str(self.work),
                    retained_evidence=str(self.work / 'native-final.json'), normalized_receipt=str(self.public_path),
                    mdk_recovery=[])])])
        self.seal()

    def seal(self):
        # Exercise real retained byte verification. The outer supervisor result
        # is synthetic: these tests cannot establish a Linux wait barrier.
        for name in ('native-seal.json', 'native-final.json'):
            (self.work / name).unlink(missing_ok=True)
        import shutil
        shutil.rmtree(self.work / 'source-inputs', ignore_errors=True)
        (self.work / 'receipt.json').write_text(json.dumps(self.original))
        (self.work / 'resolved.json').write_text(json.dumps(self.resolved))
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

    def test_signer_uses_canonical_source_expansion_and_test_phase_projection(self):
        profile = dict(environment={'ASAN_OPTIONS': 'detect_leaks=1', 'LSAN_OPTIONS': 'wrong'},
                       phase_environments={'configure': {}, 'build': {},
                                           'test': {'LSAN_OPTIONS': 'suppressions={source_root}/tests/lsan.supp'}})
        self.assertEqual(normal._sanitizer_environment(profile, '/fresh/source path'),
                         {'ASAN_OPTIONS': 'detect_leaks=1',
                          'LSAN_OPTIONS': 'suppressions=/fresh/source path/tests/lsan.supp'})
