"""Authored runtime enrollment and original MDK structural seams; no live Docker credit."""
import copy
import json
from pathlib import Path
import unittest
from unittest import mock

from selector_support import ROOT, s, change
from test_mdk_selected_route import Harness
import test_selection_execution as normal
import test_selection_realization as r
import test_selection_mdk as mdk


class HistoricalIntegrationInverse(unittest.TestCase):
    def test_exact_finite_inverse_and_unknown_source_metadata_refusal(self):
        import hashlib
        import runtime_integration_support as wave
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        before = wave.before_runtime_policy(policy)
        self.assertEqual(wave.before_runtime_policy(before), before)
        for field in ('diagnostic_nonmembers', 'diagnostic_nonmember_subsets'):
            self.assertEqual(policy[field], before[field])
        for path, record in wave.witness()['sources'].items():
            raw = (ROOT / path).read_bytes()
            restored = wave.before_runtime_source(path, raw)
            self.assertEqual(hashlib.sha256(restored).hexdigest(), record['before_sha256'])
            self.assertEqual(wave.before_runtime_source(path, restored), restored)
            with self.assertRaises(ValueError):
                wave.before_runtime_source(path, raw + b'\n# unreviewed edit\n')
        changed = copy.deepcopy(policy)
        changed['profiles']['core.linux.asan']['build_type'] = 'Unknown'
        with self.assertRaises(ValueError): wave.before_runtime_policy(changed)
        changed = copy.deepcopy(policy)
        changed['checks']['selection.contract']['runtime_inputs'].remove(wave.witness()['runtime_inputs'][0])
        with self.assertRaises(ValueError): wave.before_runtime_policy(changed)

    def test_pinned_worker_prerequisites_are_source_enrolled_without_old_authority_refresh(self):
        from profile_contract_support import source_job_blocks
        import runtime_integration_support as wave
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        ident = '.github/workflows/dependency-worker.yml:execute'
        record = policy['workflow_jobs'][ident]
        actual = source_job_blocks(ROOT)[ident]
        for field in ('source_sha256', 'workflow_sha256', 'matrix_source'):
            self.assertEqual(record[field], actual[field])
        from post608c_integration_support import before_jobs
        record = before_jobs({ident: record})[ident]
        self.assertEqual(record['source_sha256'], 'd339f832c12d71ff9957319a904d7286e46d56cde7b837cbe1add82fe621010c')
        self.assertEqual(record['workflow_sha256'], 'b1bcf046f4bfb6326bc62803c62a163249a0d074840c29e412eb891a0fd9c214')
        self.assertEqual(record['matrix_source'], '')
        self.assertEqual(record['profiles'], ['static.policy'])
        self.assertEqual(record['coverage_gap'], 'workflow_profile_union')
        self.assertIn('{source}/tests/selection/test_worker_signer_prerequisites.py',
                      policy['checks']['selection.contract']['runtime_inputs'])
        previous = wave.before_runtime_policy(policy)
        self.assertEqual(previous['workflow_jobs'][ident], wave.witness()['maps']['workflow_jobs'][ident]['before'])
        changed = copy.deepcopy(policy)
        changed['workflow_jobs'][ident]['source_sha256'] = '0' * 64
        with self.assertRaises(ValueError): wave.before_runtime_policy(changed)


class RuntimeEnrollmentTests(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_instrumented_signer_is_real_configuration_not_capability_alias(self):
        for pid in ('core.linux.asan', 'grotto.linux.asan_ubsan'):
            profile = self.policy['profiles'][pid]
            self.assertIn('signer_endpoint', profile['capabilities'])
            self.assertEqual(profile['instrumentation'], 'asan_ubsan')
            for flag in ('-DBUILD_APPS=ON', '-DENABLE_TCP_IPC=ON', '-DNIP55L_TEST_TRUST_ENV=ON',
                         '-DNIP55L_SECRET_BACKEND=libsecret'):
                self.assertEqual(profile['configure_args'].count(flag), 1)
            self.assertEqual(profile['environment']['NOSTRC_TEST_NIP5F_APPROVAL'],
                             'unidentified-get-public-key-once')
        self.assertEqual(self.policy['profiles']['core.linux.asan']['environment']['ASAN_OPTIONS'],
                         'detect_leaks=0:detect_odr_violation=0:strict_string_checks=1')
        self.assertEqual(self.policy['profiles']['grotto.linux.asan_ubsan']['environment']['ASAN_OPTIONS'],
                         'detect_leaks=1:halt_on_error=1')
        self.assertNotIn(normal.FIELD, self.policy['profiles']['core.macos.asan'])

    def test_mdk_preserves_claims_and_authors_actual_tools_domain_and_wrapper(self):
        checks = ('supported.groundhog.nip46.credentials.keyring.gnome_keyring',
                  'supported.groundhog.nip46.inbox.nak.nak', 'supported.groundhog.nip46.nak.nak',
                  'supported.groundhog.nip46.pair.nak.display.gnome_keyring.nak',
                  'supported.groundhog.store.key.keyring.gnome_keyring', 'supported.nips.nip5f.signer_endpoint')
        for pid in ('groundhog.mdk08', 'groundhog.mdk011'):
            profile = self.policy['profiles'][pid]
            self.assertEqual(profile['execution_domain'], mdk.EXECUTION_DOMAIN)
            self.assertTrue({'docker', 'nak', 'gnome_keyring', 'signer_endpoint'} <= set(profile['capabilities']))
            self.assertEqual(profile['configure_args'].count('-DGROUNDHOG_DOCKER:FILEPATH={source_root}/scripts/mdk-docker'), 1)
            for ident in checks:
                self.assertIn(pid, self.policy['checks'][ident]['profiles'])
        self.assertEqual(self.policy['profiles']['groundhog.linux.arm64.prepared011']['execution_domain'],
                         'prepared_peer_linux_v1')
        self.assertNotIn(normal.FIELD, self.policy['profiles']['groundhog.linux.sanitizers'])
        self.assertFalse(self.policy['enforcement_ready'])
        # The ten runtime-era gaps plus only the exact legacy macOS NIP-5F TCP
        # nonvalidation record (nostrc-0cfsg.36.9); no other gap appears.
        gaps = [g['id'] for g in self.policy['coverage_gaps']]
        self.assertEqual(len(gaps), 11)
        self.assertEqual(gaps[-1], 'core_macos_asan_nip5f_tcp_unconfigured')

    def test_homed_normal_contract_requires_every_owned_unit_not_root_alias(self):
        profile = self.policy['profiles']['homed.linux.units']
        self.assertEqual(profile[normal.FIELD], normal.MARKER)
        self.assertEqual(profile['contexts'], ['local'])
        plan = s.make_plan(self.policy, [change('gnome/nostr-homed/src/common/nostr_cache.c')],
                           head='a' * 40, base='b' * 40)
        lane = next(lane for lane in plan['lanes'] if lane['profile'] == 'homed.linux.units')
        self.assertEqual(set(lane['checks']), {'homed.units', 'homed.default_build'})
        self.assertNotIn('homed.linux.units', self.policy['workflow_jobs'][profile['legacy_workflow']]['profiles'])


class MDKGraphTests(Harness):
    def graph(self, version):
        self.cache(version)
        proof = self.contract()
        cases = ('control groundhog-invites-mdk mdk-invites-groundhog mdk-invites-groundhog-gui '
                 'adopted-welcome white-noise-welcome white-noise-media media-policy white-noise-voice '
                 'white-noise-dm adopted-commits routing-rotation concurrent-commits polls white-noise-reactions').split()
        tests, descriptors = [], []
        fixtures = {'08': 'groundhog-mdk-image', '011': 'groundhog-mdk011-image', '09': 'groundhog-mdk09-image'}
        for v, data in proof['versions'].items():
            tests.append(dict(name=mdk.IMAGE_TESTS[v], command=[str(self.root / mdk.WRAPPER), 'build', '-t', data['tag'], data['context']],
                              properties=[dict(name='FIXTURES_SETUP', value=[fixtures[v]])]))
            descriptors.append(dict(name=mdk.IMAGE_TESTS[v], requires=['docker']))
        names = {'groundhog-mdk-interop': '08'} if version == '08' else {
            **{'groundhog-mdk011-interop-' + case: '011' for case in cases},
            'groundhog-mdk011-interop-mdk09-probe': '09'}
        target = 'test-groundhog-mdk-interop' if version == '08' else 'test-groundhog-mdk011-interop'
        artifact = str(self.build / target)
        for name, v in names.items():
            driver = [str(self.root / mdk.WRAPPER), 'run', '--rm', '-i', '-e', 'MDK_DRIVER_LOG', '-e', 'MDK_DRIVER_DEADLINE_S']
            if v == '011':
                driver += ['-e', 'MDK_DRIVER_ARTIFACT_DIR=/artifacts', '-v', str(self.build / 'mdk011-interop-artifacts') + ':/artifacts', '--user', '0:0']
            driver += ['--network', 'host', proof['versions'][v]['tag']]
            import shlex
            case = name.removeprefix('groundhog-mdk011-interop-')
            args = [] if v == '08' else (['--gui', '-p', '/groundhog/mdk011-interop-gui/mdk-invites-groundhog']
                    if case == 'mdk-invites-groundhog-gui' else ['-p', '/groundhog/mdk011-interop/' + case])
            key = 'GH_MDK09_DRIVER=' if v == '09' else 'GH_MDK_DRIVER='
            tests.append(dict(name=name, command=[artifact, *args], properties=[
                dict(name='ENVIRONMENT', value=[key + shlex.join(driver)]),
                dict(name='FIXTURES_REQUIRED', value=[fixtures[v]])]))
            descriptors.append(dict(name=name, requires=['docker']))
        self.record.update(tests=tests, descriptors=descriptors,
                           targets={'target': dict(name=target, artifacts=[artifact])})
        return proof

    def test_original_producers_consumers_and_both_version_families(self):
        for version in ('08', '011'):
            with self.subTest(version=version):
                normal.validate_mdk_graph(self.record, self.graph(version))

    def test_arbitrary_docker_registration_or_changed_commands_are_not_admitted(self):
        proof = self.graph('08')
        baseline = copy.deepcopy(self.record)
        for mutate in (lambda real: real['descriptors'].append(dict(name='arbitrary-docker', requires=['docker'])),
                       lambda real: real['tests'][0]['command'].__setitem__(0, '/usr/bin/docker'),
                       lambda real: real['tests'][0]['command'].__setitem__(3, 'unowned:latest'),
                       lambda real: real['tests'][-1]['command'].append('--skip-security'),
                       lambda real: real['tests'][-1]['properties'].clear()):
            with self.subTest(mutate=mutate):
                changed = copy.deepcopy(baseline); mutate(changed)
                with self.assertRaises(ValueError): normal.validate_mdk_graph(changed, proof)

    def test_runtime_capability_dispatch_uses_typed_contract_and_structure_without_daemon(self):
        proof = self.graph('08')
        with mock.patch.object(mdk, 'contract', return_value=proof) as contract:
            self.assertEqual(r._runtime_capability_evidence(self.record, ['docker']), {'docker': proof})
            contract.assert_called_once()
        changed = copy.deepcopy(self.record); changed['profile']['capabilities'].remove('docker')
        with self.assertRaises(r.RealizationError): r._runtime_capability_evidence(changed, ['docker'])
