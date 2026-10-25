"""Signer impact and entry-level inventory regressions, not runtime acceptance."""
import copy
import json
import unittest

from selector_support import ROOT, GitCase, change, plan, policy, s


class SignerSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        cls.fixture = s.read_json(ROOT / 'tests/selection/fixtures/nip55l_affected.json')

    def assert_signer_boundary(self, result, expected):
        self.assertEqual(result['effective_mode'], 'affected')
        self.assertEqual(result['broadening_reasons'], [])
        self.assertEqual(result['selected_checks'], expected)
        tests = {name for lane in result['lanes'] for ident in lane['checks']
                 for name in self.p['checks'][ident].get('ctest_tests', {}).get(lane['profile'], [])}
        self.assertTrue({'groundhog-identity', 'groundhog-auth-policy',
                         'gnostr-test-signer-nip55l-identity', 'nostr_seal_signer',
                         'nostr_seal_unit', 'nostr-dav-publish-rollback',
                         'nostr-share-share', 'nostr-settings-lists'} <= tests)
        self.assertTrue(any(name.startswith('signer/') for name in tests))
        self.assertTrue(any(name.startswith('nip55l_') for name in tests))
        self.assertTrue(any(name.startswith('nostr_signer_webext_host_') for name in tests))
        self.assertFalse(any(name.startswith('groundhog-') and
                             any(word in name for word in ('mls', 'mdk', 'nip29', 'voice', 'poll'))
                             for name in tests))
        self.assertFalse(any(node in result['witnesses'] for node in
                             ('groundhog.unclassified', 'groundhog.mls_service',
                              'groundhog.mls_ui', 'groundhog.poll_ui', 'groundhog.voice')))
        # This is link compatibility only; it does not execute the MLS suite.
        compat = self.p['checks']['groundhog.identity_mls_compat']
        self.assertEqual(compat['kind'], 'build_contract')
        self.assertEqual(compat['profiles'], ['groundhog.identity_compat.linux'])
        self.assertNotIn('ctest_tests', compat)

    def test_nip55l_only_exact_set(self):
        for path in (self.fixture['signer_path'], 'nips/nip55l/src/core/signer_ops.c',
                     'nips/nip55l/dbus/org.nostr.Signer.xml'):
            with self.subTest(path=path):
                self.assert_signer_boundary(plan(self.p, paths=(path,)), self.fixture['expected_signer_checks'])

    def test_versioned_public_header_adds_only_version_contracts(self):
        expected = sorted(set(self.fixture['expected_signer_checks']) |
                          {'groundhog.packaging_version', 'selection.contract',
                           'supported.groundhog.contract.packaging.version'})
        self.assert_signer_boundary(plan(self.p, paths=('nips/nip55l/include/nostr/nip55l/signer_ops.h',)), expected)

    def test_g15_current_policy_replay_exact_set(self):
        before = copy.deepcopy(self.p)
        for ident in self.fixture['inventory_added_checks']:
            del before['checks'][ident]
        result = s.make_plan(self.p, self.fixture['changes'], head=self.fixture['head'],
                             base=self.fixture['base'], base_policies=[before])
        self.assert_signer_boundary(result, self.fixture['expected_g15_checks'])
        self.assertEqual(result['metadata_changed_checks'], ['grotto.permissions.repeat'])
        self.assertEqual(result['metadata_broad_profiles'], [])

    def test_real_adapter_sources_and_seal_registration_contract(self):
        gh = (ROOT / 'gnome/groundhog/CMakeLists.txt').read_text()
        self.assertIn('src/identity/gh-signer-nip55l.c', gh)
        self.assertIn('tests/identity/test_identity.c', gh)
        self.assertIn('tests/app/test_auth_policy.c', gh)
        seal = (ROOT / 'gnome/nostr-seal/CMakeLists.txt').read_text()
        for name in ('nostr_seal_unit', 'nostr_seal_config', 'nostr_seal_publish', 'nostr_seal_signer'):
            self.assertIn('add_test(NAME ' + name, seal)
        self.assertIn('add_dependencies(test-nostr-seal-signer nostr-signer-daemon nostr-seal)', seal)
        flags = self.p['profiles']['nostr_seal.linux']['configure_args']
        self.assertIn('-DENABLE_NOSTR_SEAL=ON', flags)
        self.assertIn('-DNIP55L_TEST_TRUST_ENV=ON', flags)
        self.assertIn('-DNIP55L_SECRET_BACKEND=libsecret', flags)
        self.assertIn('private_bus', self.p['checks']['nostr_seal.signer']['requires'])
        self.assertNotIn('coverage_gap', self.p['nodes']['gnome.nostr_seal'])

    def test_identity_api_changes_still_reach_mls_and_unknown_paths_stay_broad(self):
        result = plan(self.p, paths=('gnome/groundhog/src/identity/gh-signer.h',))
        self.assertIn('groundhog.mls_service', result['witnesses'])
        self.assertIn('groundhog.mls_service', result['selected_checks'])
        for path in ('gnome/groundhog/unknown-new-input.c', 'unknown-component/source.c',
                     'scripts/test_selection.py', 'scripts/contained-gate.py'):
            with self.subTest(path=path):
                self.assertEqual(plan(self.p, paths=(path,))['effective_mode'], 'broad')

    def test_ci_dispatch_and_actual_consumer_route(self):
        from test_ci_selected_execution import ci
        result = plan(self.p, paths=(self.fixture['signer_path'],), context='ci')
        dispatch = ci.project(result)
        self.assertFalse(dispatch['broad'])
        self.assertFalse(dispatch['run_reference'])
        self.assertTrue({'grotto', 'groundhog', 'nip55l'} <= set(dispatch['families']))
        workflow = (ROOT / '.github/workflows/nip55l-libsecret-ci.yml').read_text()
        self.assertIn('runner.load_handoff', workflow)
        self.assertIn('scripts/test-gate.sh "${args[@]}"', workflow)
        for profile in ('nostr_seal.linux', 'desktop.signer.linux', 'groundhog.identity_compat.linux'):
            self.assertIn(profile, workflow)
        self.assertIn('selection_artifact: ${{ needs.select.outputs.selection_artifact }}',
                      (ROOT / '.github/workflows/ci-selective.yml').read_text())


class InventoryEntryTests(GitCase):
    def setUp(self):
        super().setUp()
        self.p['profiles']['other'] = copy.deepcopy(self.p['profiles']['linux'])
        self.p['checks']['other.unit']['profiles'] = ['other']
        self.p['checks']['other.unit']['ctest_tests'] = {'other': ['other.unit']}
        self.save()
        self.base = self.commit()

    def save(self):
        self.write('tests/selection/inventory.json', json.dumps(self.p))

    def test_new_check_selects_only_its_entries_not_unrelated_profile_members(self):
        self.p['checks']['new.unit'] = dict(self.p['checks']['core.unit'], ctest_tests={'linux': ['new.unit']})
        self.save(); self.commit()
        result = self.select()
        self.assertEqual(result['effective_mode'], 'affected')
        self.assertEqual(result['selected_checks'], ['new.unit'])
        self.assertEqual(result['metadata_affected_profiles'], ['linux'])
        self.assertEqual(result['metadata_broad_profiles'], [])

    def test_profile_configuration_change_selects_full_named_profile_only(self):
        self.p['profiles']['linux']['configure_args'].append('-DFEATURE=ON')
        self.save(); self.commit()
        result = self.select()
        self.assertEqual(result['effective_mode'], 'affected')
        self.assertEqual(result['selected_checks'], ['app.live', 'app.unit', 'core.unit', 'fixture.unit'])
        self.assertEqual(result['metadata_broad_profiles'], ['linux'])
        self.assertNotIn('other.unit', result['selected_checks'])

    def add_shared_check(self):
        self.p['checks']['shared.unit'] = dict(self.p['checks']['core.unit'],
            profiles=['linux', 'other'], ctest_tests={'linux': ['shared.unit'], 'other': ['shared.unit']})
        self.save(); self.base = self.commit()

    def test_membership_edit_does_not_select_unchanged_profile_of_same_check(self):
        self.add_shared_check()
        self.p['checks']['shared.unit']['ctest_tests']['linux'] = ['shared.unit.v2']
        self.save(); self.commit()
        result = self.select()
        self.assertEqual(result['effective_mode'], 'affected')
        self.assertEqual(result['selected_checks'], ['shared.unit'])
        self.assertEqual(result['expected_lane_ids'], ['linux'])
        self.assertEqual(result['metadata_check_profiles'], {'shared.unit': ['linux']})
        self.assertEqual(result['metadata_affected_profiles'], ['linux'])

    def test_profile_edit_does_not_run_shared_check_in_other_profiles(self):
        self.add_shared_check()
        self.p['profiles']['linux']['configure_args'].append('-DFEATURE=ON')
        self.save(); self.commit()
        result = self.select()
        self.assertEqual(result['effective_mode'], 'affected')
        self.assertEqual(result['expected_lane_ids'], ['linux'])
        self.assertIn('shared.unit', result['selected_checks'])
        self.assertNotIn('other.unit', result['selected_checks'])

    def test_removed_check_broadens_its_old_profile_not_repository(self):
        del self.p['checks']['fixture.unit']
        self.save(); self.commit()
        result = self.select()
        self.assertEqual(result['effective_mode'], 'affected')
        self.assertEqual(result['selected_checks'], ['app.live', 'app.unit', 'core.unit'])
        self.assertEqual(result['metadata_broad_profiles'], ['linux'])

    def test_ownership_changes_and_removed_profiles_are_broad(self):
        self.p['nodes']['app']['depends_on'] = []
        self.save(); self.commit()
        self.assertEqual(self.select()['effective_mode'], 'broad')
        self.p['profiles'].pop('other')
        self.p['checks'].pop('other.unit')
        self.save(); self.commit()
        result = self.select()
        self.assertEqual(result['effective_mode'], 'broad')
        self.assertTrue(any('removed execution profiles' in reason for reason in result['broadening_reasons']))

    def test_inventory_without_base_is_broad(self):
        result = s.make_plan(self.p, [change('tests/selection/inventory.json')], head='a' * 40)
        self.assertEqual(result['effective_mode'], 'broad')
        self.assertTrue(result['broadening_reasons'])


class BuildImpactTests(unittest.TestCase):
    def test_build_prerequisite_does_not_select_runtime_but_broad_is_unchanged(self):
        p = policy()
        p['nodes']['app']['depends_on'] = []
        p['nodes']['app'].update(build_depends_on=['core'], build_dependency_reason='stable interface compatibility')
        p['checks']['app.unit']['nodes'].append('core')
        p['checks']['app.unit'].update(build_only_nodes=['core'], build_dependency_reason='compiled prerequisite')
        self.assertEqual(plan(p, paths=('core/source.c',))['selected_checks'], ['core.unit'])
        self.assertEqual(plan(p, paths=('app/source.c',))['selected_checks'], ['app.live', 'app.unit'])
        self.assertEqual(len(plan(p, paths=('core/source.c',), mode='broad')['selected_checks']), 5)
        p['checks']['app.unit']['build_only_nodes'] = ['other']
        with self.assertRaises(s.ContractError):
            s.validate_inventory(p)


if __name__ == '__main__':
    unittest.main()
