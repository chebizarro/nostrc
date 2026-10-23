"""Admission negatives, not fabricated service/profile acceptance."""
import copy
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import test_selection_services as services
import test_selection_realization as realization

spec = importlib.util.spec_from_file_location('provisioned_nip5f', ROOT / 'tests/runtime/test_provisioned_nip5f.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class WorkerSignerAdmission(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.build = Path(temporary.name)
        self.profile = {'platform': 'linux', 'architecture': os.uname().machine,
            'capabilities': ['signer_endpoint'],
            'configure_args': ['-DENABLE_TCP_IPC=ON', '-DNIP55L_TEST_TRUST_ENV=ON', '-DNIP55L_SECRET_BACKEND=libsecret'],
            'environment': {'NOSTRC_TEST_NIP5F_APPROVAL': services.SIGNER_CONSENT}}
        self.record = {'profile': self.profile, 'repo_root': str(ROOT), 'build_dir': str(self.build),
                       'source_identity': {'kind': 'committed', 'sha': 'a' * 40}}
        self.cache = '\n'.join(['ENABLE_TCP_IPC:BOOL=ON', 'NIP55L_TEST_TRUST_ENV:BOOL=ON',
                               'NIP55L_SECRET_BACKEND:STRING=libsecret', 'CMAKE_HOME_DIRECTORY:INTERNAL=' + str(ROOT), ''])
        (self.build / 'CMakeCache.txt').write_text(self.cache)
        self.platform = patch.object(services.sys, 'platform', 'linux')
        self.platform.start(); self.addCleanup(self.platform.stop)

    def test_contract_is_not_readiness_and_never_launches(self):
        with patch.object(services.subprocess, 'Popen') as launch:
            proof = services.signer_contract(self.record)
        launch.assert_not_called()
        self.assertFalse(proof['readiness'])
        self.assertFalse(proof['complete'])
        self.assertFalse(proof['complete_gate_eligible'])
        self.assertFalse(proof['remember'])
        self.assertEqual(len(proof['inputs']), 4)

    def test_environment_claim_cannot_replace_authored_consent(self):
        self.profile['environment'] = {}
        with patch.dict(os.environ, NOSTRC_TEST_NIP5F_APPROVAL=services.SIGNER_CONSENT):
            with self.assertRaisesRegex(services.ServiceError, 'authored one-shot'):
                services.signer_contract(self.record)

    def test_missing_or_different_consent_refuses(self):
        for value in ('', 'allow', 'all', True):
            with self.subTest(value=value):
                self.profile['environment']['NOSTRC_TEST_NIP5F_APPROVAL'] = value
                with self.assertRaisesRegex(services.ServiceError, 'authored one-shot'):
                    services.signer_contract(self.record)

    def test_configuration_must_be_authored_not_just_cached(self):
        self.profile['configure_args'].pop(0)
        with self.assertRaisesRegex(services.ServiceError, 'explicit authored configuration'):
            services.signer_contract(self.record)

    def test_duplicate_or_conflicting_authored_flag_refuses(self):
        self.profile['configure_args'].append('-DENABLE_TCP_IPC=OFF')
        with self.assertRaisesRegex(services.ServiceError, 'explicit authored configuration'):
            services.signer_contract(self.record)

    def test_realized_configuration_mismatch_refuses(self):
        (self.build / 'CMakeCache.txt').write_text(self.cache.replace('ENABLE_TCP_IPC:BOOL=ON', 'ENABLE_TCP_IPC:BOOL=OFF'))
        with self.assertRaisesRegex(services.ServiceError, 'realized configuration mismatch'):
            services.signer_contract(self.record)

    def test_other_source_build_refuses(self):
        (self.build / 'CMakeCache.txt').write_text(self.cache.replace(str(ROOT), '/unrelated'))
        with self.assertRaisesRegex(services.ServiceError, 'source root mismatch'):
            services.signer_contract(self.record)

    def test_no_architecture_alias_or_host_promotion(self):
        self.profile['architecture'] = 'unsupported-worker-alias'
        with self.assertRaisesRegex(services.ServiceError, 'exact native Linux'):
            services.signer_contract(self.record)
        self.profile['architecture'] = os.uname().machine
        with patch.object(services.sys, 'platform', 'darwin'):
            with self.assertRaisesRegex(services.ServiceError, 'exact native Linux'):
                services.signer_contract(self.record)

    def test_realization_keeps_profile_declaration_guard(self):
        self.profile['capabilities'] = []
        with self.assertRaisesRegex(realization.RealizationError, 'not declared'):
            realization._runtime_capability_evidence(self.record, ['signer_endpoint'])

    def test_realization_records_provision_contract_not_setup(self):
        proof = realization._runtime_capability_evidence(self.record, ['signer_endpoint'])
        self.assertEqual(proof['signer_endpoint'], services.signer_contract(self.record))
        self.assertFalse(proof['signer_endpoint']['readiness'])

    def test_no_passing_or_partial_audit_can_launch_services(self):
        proof = services.signer_contract(self.record)
        resolved = {'runtime_capability_evidence': {'signer_endpoint': proof}}
        for audit in ({}, {'passed': False, 'after_build': True}, {'passed': True, 'after_build': False}):
            with self.subTest(audit=audit), patch.object(services.subprocess, 'Popen') as launch:
                with self.assertRaisesRegex(services.ServiceError, 'complete post-build input audit'):
                    with services.provisioned_signer(self.record, resolved, audit, self.build / 'evidence'):
                        self.fail('unaudited signer admitted')
                launch.assert_not_called()

    def test_passing_booleans_do_not_replace_full_audit_input_bindings(self):
        proof = services.signer_contract(self.record)
        self.record['descriptors'] = [{'name': 'test_nip5f_tcp', 'requires': ['signer_endpoint']}]
        resolved = {'runtime_capability_evidence': {'signer_endpoint': proof},
                    'tests': ['test_nip5f_tcp'], 'command_executions': {}}
        with patch.object(services.subprocess, 'Popen') as launch:
            with self.assertRaisesRegex(services.ServiceError, 'exact full-audit binding'):
                with services.provisioned_signer(self.record, resolved,
                    {'passed': True, 'after_build': True, 'input_hashes': {}}, self.build / 'evidence'):
                    self.fail('passing capability/audit booleans admitted')
            launch.assert_not_called()

    def test_test_phase_cannot_silently_change_authored_consent(self):
        with self.assertRaisesRegex(services.ServiceError, 'test-phase consent differs'):
            services.signer_contract(self.record, dict(os.environ, NOSTRC_TEST_NIP5F_APPROVAL='all'))

    def test_changed_contract_refuses_before_launch(self):
        proof = copy.deepcopy(services.signer_contract(self.record))
        proof['remember'] = True
        with patch.object(services.subprocess, 'Popen') as launch:
            with self.assertRaisesRegex(services.ServiceError, 'contract mismatch'):
                with services.provisioned_signer(self.record, {'runtime_capability_evidence': {'signer_endpoint': proof}}, {}, self.build / 'evidence'):
                    self.fail('changed consent admitted')
            launch.assert_not_called()

    def test_fixture_cannot_start_without_explicit_consent(self):
        with self.assertRaisesRegex(RuntimeError, 'explicit unidentified'):
            fixture.prepare_fixture(ROOT, self.build, self.build / 'evidence', consent=False)
        self.assertFalse((self.build / 'evidence').exists())

    def test_prebuilt_route_cannot_fallback_to_compilation(self):
        with patch.object(fixture.platform, 'system', return_value='Linux'), patch.object(fixture.subprocess, 'run') as run:
            with self.assertRaisesRegex(RuntimeError, 'required built fixture executable'):
                fixture.prepare_fixture(ROOT, self.build, self.build / 'evidence', consent=True, prebuilt=True)
            run.assert_not_called()


    def test_tools_are_independent_bound_executables(self):
        proof = services.signer_contract(self.record)
        self.assertEqual(set(proof['executables']), {'dbus-daemon', 'gdbus', 'python'})
        for value in proof['executables'].values():
            self.assertTrue(Path(value['path']).is_absolute())
            self.assertEqual(len(value['sha256']), 64)
        self.assertNotIn('gnome_keyring', proof['executables'])
        self.assertNotIn('nak', proof['executables'])

    def test_generated_artifacts_require_the_declared_producer(self):
        self.record['targets'] = {'approver': {'name': 'test_nip5f_approver', 'artifacts': [], 'sources': []}}
        value = str(self.build / 'nips/nip5f/test_nip5f_approver')
        desc = {'requires': ['signer_endpoint']}
        with self.assertRaisesRegex(realization.RealizationError, 'declared producer closure'):
            realization._signer_generated_input(self.record, 'test_nip5f_tcp', desc, ['approver'], value)
        self.record['targets']['approver']['artifacts'] = [value]
        self.assertTrue(realization._signer_generated_input(self.record, 'test_nip5f_tcp', desc, ['approver'], value))
        self.assertFalse(realization._signer_generated_input(self.record, 'unrelated', desc, ['approver'], value))
        self.assertFalse(realization._signer_generated_input(self.record, 'test_nip5f_tcp', desc, ['approver'], str(self.build / 'unknown')))

    def test_schema_is_not_inferred_from_an_arbitrary_utility(self):
        schema = str(self.build / 'apps/grotto/gschemas.compiled')
        self.record['targets'] = {'schema': {'name': 'other', 'artifacts': [],
                                             'sources': [{'path': schema + '.rule', 'generated': True}]}}
        desc = {'requires': ['signer_endpoint']}
        with self.assertRaisesRegex(realization.RealizationError, 'declared producer closure'):
            realization._signer_generated_input(self.record, 'test_nip5f_tcp', desc, ['schema'], schema)
        self.record['targets']['schema']['name'] = 'grotto-schemas'
        self.assertTrue(realization._signer_generated_input(self.record, 'test_nip5f_tcp', desc, ['schema'], schema))


from realization_support import Project, r as fixture_realization


class WorkerNonSignerSelection(Project):
    def test_non_signer_subset_of_fixture_profile_does_not_provision_or_promote(self):
        self.profile['capabilities'].append('signer_endpoint')
        realized = self.configure()
        self.build_all()
        resolved = fixture_realization.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        audit = self.audit(realized)
        self.assertTrue(audit['passed'], audit['gaps'])
        self.assertNotIn('signer_endpoint', resolved['required_capabilities'])
        self.assertFalse(resolved['complete_gate_eligible'])
        self.assertFalse(audit['complete_gate_eligible'])
        with patch.object(services, 'provisioned_signer') as provision:
            receipt = fixture_realization.run_resolved(resolved, self.build, self.evidence,
                                                       input_audit=audit, build=False)
            provision.assert_not_called()
        self.assertEqual(receipt['outcome'], 'passed')
        self.assertFalse(receipt['complete'])
        self.assertFalse(receipt['complete_gate_eligible'])


if __name__ == '__main__':
    unittest.main()
