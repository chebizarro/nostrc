"""Whole supported ordinary/root census; no subset or workflow acceptance claim."""
import copy
import hashlib
import json
import unittest

from selector_support import ROOT, change, s
from realization_support import Project, r
from supported_profile_support import (
    NEW_INPUTS, before_supported_batch_contract, before_supported_batch_policy,
    census_inputs, witness,
)


class SupportedProfileMembership(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        cls.data = witness()

    def test_actual_inventory_valid_and_historical_bytes_immutable(self):
        s.validate_inventory(self.p)
        for path, expected in self.data['historical_fixture_hashes'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), expected, path)
        for path in NEW_INPUTS:
            self.assertIn(path, self.p['checks']['selection.contract']['runtime_inputs'])
            self.assertTrue((ROOT / path.removeprefix('{source}/')).is_file())

    def test_complete_primary_census_without_fixture_or_omission_laundering(self):
        expected = {'groundhog.linux': (422, 386, 33),
                    'grotto.linux': (461, 422, 33), 'core.linux': (441, 434, 0),
                    'grotto.linux.asan_ubsan': (338, 334, 0),
                    'groundhog.linux.sanitizers': (413, 62, 0),
                    'groundhog.mdk08': (571, 560, 0), 'groundhog.mdk011': (441, 435, 0),
                    'core.linux.asan': (272, 268, 0), 'core.macos.asan': (271, 267, 0),
                    'libgo.linux.asan': (33, 33, 0), 'libgo.macos.asan': (33, 32, 0)}
        self.assertEqual(set(self.data['profiles']), set(expected))
        for profile, record in self.data['profiles'].items():
            claims = {}
            for ident, check in self.p['checks'].items():
                if check['kind'] != 'ctest' or profile not in check['profiles']:
                    continue
                for name in r._membership(check, profile):
                    self.assertNotIn(name, claims, (profile, name))
                    claims[name] = ident
            self.assertEqual(set(claims), set(record['primary']), profile)
            self.assertEqual((record['registration_count'], len(claims), len(record['omissions'])),
                             expected[profile])
            self.assertFalse(set(claims) & set(record['omissions']))
            self.assertFalse(set(claims) & set(record['unsupported_registrations']))
            for name, facts in record['primary'].items():
                self.assertIn(name, facts['closure'])
                self.assertFalse(set(facts['closure']) & set(record['omissions']))
                check = self.p['checks'][claims[name]]
                if claims[name] in record['added_checks']:
                    self.assertTrue(set(facts['build_targets']) <= set(check['build_targets']), name)
                self.assertTrue(set(facts['requires']) <= set(check.get('requires', [])), name)
                for support in set(facts['closure']) - {name}:
                    self.assertNotIn(support, claims, (name, support))

    def test_every_real_compiled_runtime_and_fixture_input_has_check_local_impact(self):
        for profile, record in self.data['profiles'].items():
            claims = {name: check for check in self.p['checks'].values()
                      if check['kind'] == 'ctest' and profile in check['profiles']
                      for name in r._membership(check, profile)}
            for name, facts in record['primary'].items():
                covered = r._impact_prerequisites(self.p, claims[name]['nodes'])
                for path in census_inputs(self.data, facts):
                    self.assertTrue((ROOT / path).is_file(), path)
                    classified = s.classify(self.p, path)
                    self.assertFalse(classified.get('unclassified'), path)
                    if classified['effect'] != 'global':
                        self.assertEqual(classified['effect'], 'impact', path)
                        self.assertTrue(set(classified['nodes']) & covered, (profile, name, path))

        for profile, checks in self.data['command_checks'].items():
            for ident, facts in checks.items():
                check = self.p['checks'][ident]
                self.assertIn(profile, check['profiles'])
                covered = r._impact_prerequisites(self.p, check['nodes'])
                for path in census_inputs(self.data, facts):
                    classified = s.classify(self.p, path)
                    if classified['effect'] != 'global':
                        self.assertEqual(classified['effect'], 'impact', path)
                        self.assertTrue(set(classified['nodes']) & covered, (profile, ident, path))

    def test_no_invented_production_graph_profile_or_sanitizer_authority(self):
        frozen = {'nodes': '91dafbea554a51ec5e1eb3cf6658d1048e8eccfaa0fc52b9ed30136b95d4e6ff',
                  'path_rules': 'b97f07836ab765abcc51c445dc31081273bfe7232fcc458f26216fcba0679229',
                  'profiles': '88b5dae9f73c4957d3fe1232f6a7bd6b89d4dae5957d36d7b0a9decc27fe1599',
                  'diagnostic_nonmembers': 'b8f7de593bad6947ae73e2e5eacaf91e5e44814e3a24ed9906776a2d0343034b'}
        previous = before_supported_batch_policy(self.p)
        for field, expected in frozen.items():
            self.assertEqual(s.digest(previous[field]), expected, field)
        structural = self.data['structural_changes']
        self.assertEqual(set(structural['nodes']), {'testing.common_inputs', 'mdk08.peer_inputs',
                                                  'mdk09.peer_inputs', 'mdk011.peer_inputs'})
        for ident, node in structural['nodes'].items():
            self.assertEqual(node['depends_on'], [])
            self.assertEqual(self.p['nodes'][ident], node)
        self.assertEqual({k: v for k, v in self.p['nodes'].items() if k not in structural['nodes']},
                         previous['nodes'])
        self.assertEqual(self.p['profiles'], previous['profiles'])
        self.assertEqual(self.p['diagnostic_nonmembers'], previous['diagnostic_nonmembers'])
        self.assertEqual(self.p['nodes']['groundhog.unclassified']['depends_on'], [])
        self.assertFalse(self.p['enforcement_ready'])
        for profile, record in self.data['contracts'].items():
            current = self.p['diagnostic_nonmember_subsets'][profile]
            old = before_supported_batch_contract(current, profile)
            self.assertEqual(s.digest(old), record['before_hash'])
            self.assertEqual(set(record['source_changes']),
                             {'gnome/groundhog/TestInventory.cmake', 'nips/nip5f/TestInventory.cmake'})
            self.assertEqual(current['registrations'], old['registrations'])
            self.assertEqual(len(current['registrations']), 33)
            self.assertEqual(set(current['review']['source_inputs']), set(old['review']['source_inputs']))
            self.assertEqual(len(current['review']['source_inputs']), 193)
            for path, values in record['source_changes'].items():
                self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), values['after'])

    def test_unknown_contract_and_policy_mutations_are_not_projected_away(self):
        before_supported_batch_policy(self.p)
        for profile in self.data['contracts']:
            for mutate in (
                lambda c: c['primary_registrations'].pop(),
                lambda c: c['registrations'].pop(),
                lambda c: c['review']['source_inputs'].update({'unknown.cmake': '0' * 64}),
                lambda c: c.update(candidate_contract_hash='0' * 64),
            ):
                bad = copy.deepcopy(self.p['diagnostic_nonmember_subsets'][profile])
                mutate(bad)
                with self.assertRaises(s.ContractError):
                    before_supported_batch_contract(bad, profile)
        bad = copy.deepcopy(self.p)
        ident = self.data['profiles']['groundhog.linux']['added_checks'][0]
        bad['checks'][ident]['nodes'].append('libgo')
        with self.assertRaisesRegex(s.ContractError, 'census check changed'):
            before_supported_batch_policy(bad)

    def test_graphics_tools_and_headless_aliases_remain_separate(self):
        record = self.data['profiles']['groundhog.linux']['primary']
        for base in ('groundhog-about', 'groundhog-group-ui', 'groundhog-mls-ui',
                     'groundhog-notifier', 'groundhog-requests', 'groundhog-conversation-menu'):
            self.assertNotIn('display', record[base]['requires'])
            self.assertIn('display', record[base + '-gui']['requires'])
        for name in ('groundhog-nip46-nak', 'groundhog-nip46-inbox-nak', 'groundhog-nip46-pair-nak'):
            self.assertIn('nak', record[name]['requires'])
        for name in ('groundhog-store-key-keyring', 'groundhog-nip46-credentials-keyring',
                     'groundhog-nip46-pair-nak'):
            self.assertIn('gnome_keyring', record[name]['requires'])
        self.assertEqual(record['test_nip5f_tcp']['requires'], ['signer_endpoint'])
        self.assertNotIn('signer_endpoint', record['test_nip5f_loopback']['requires'])
        for profile in ('groundhog.linux', 'grotto.linux'):
            self.assertFalse({'nak', 'gnome_keyring', 'signer_endpoint'} &
                             set(self.p['profiles'][profile]['capabilities']))

    def test_semantic_selection_savings_no_new_live_peer_or_unrelated_standalone_work(self):
        historical = before_supported_batch_policy(self.p)
        for path in ('apps/gnostr/src/main_app.c', 'nostr-gtk/data/nostr-gtk.gresource.xml',
                     'tests/test_json_filter.c'):
            self.assertTrue((ROOT / path).is_file())
            plan = s.make_plan(self.p, [change(path)], head=self.data['base_sha'])
            old = s.make_plan(historical, [change(path)], head=self.data['base_sha'])
            live = lambda result: {c for c in result['selected_checks']
                                   if c.startswith(('mdk08.', 'mdk011.', 'prepared011.'))}
            self.assertEqual(live(plan), live(old), path)
            self.assertFalse(any(c.startswith(('mdk08.', 'mdk011.'))
                                 for c in plan['selected_checks']), path)
        plan = s.make_plan(self.p, [change('nips/nip49/src/nip49.c')],
                           head=self.data['base_sha'], profiles=['grotto.linux'])
        self.assertNotIn('libgo.channel', plan['selected_checks'])
        self.assertFalse(any(c.startswith('supported.libhanami') for c in plan['selected_checks']))


class SupportedCapabilityDescriptors(Project):
    def test_real_cmake_descriptor_capabilities_refuse_before_execution(self):
        # Real CMake descriptor export, not hand-filled descriptor dictionaries.
        (self.root / 'groundhog-inventory.cmake').write_bytes(
            (ROOT / 'gnome/groundhog/TestInventory.cmake').read_bytes())
        cmake = self.root / 'CMakeLists.txt'
        text = cmake.read_text().replace('nostrc_finalize_test_inventory()', '''
add_executable(test-groundhog-shell main.c)
add_executable(test-groundhog-about main.c)
add_executable(test-groundhog-nip46-nak main.c)
add_executable(test-groundhog-store-key-keyring main.c)
add_test(NAME groundhog-shell COMMAND test-groundhog-shell)
add_test(NAME groundhog-about COMMAND test-groundhog-about)
add_test(NAME groundhog-about-gui COMMAND test-groundhog-about --gui)
add_test(NAME groundhog-nip46-nak COMMAND test-groundhog-nip46-nak)
add_test(NAME groundhog-store-key-keyring COMMAND test-groundhog-store-key-keyring)
include(groundhog-inventory.cmake)
nostrc_finalize_test_inventory()
''')
        cmake.write_text(text)
        self.profile['capabilities'] = []
        real = self.configure()
        tests, descriptors = r._test_maps(real)
        for name, required in [('groundhog-shell', ['display']), ('groundhog-about', []),
                               ('groundhog-about-gui', ['display']),
                               ('groundhog-nip46-nak', ['nak']),
                               ('groundhog-store-key-keyring', ['gnome_keyring'])]:
            self.assertEqual(descriptors[name]['requires'], required)
            self.inventory['checks']['probe'] = {'kind': 'ctest', 'profiles': ['synthetic'],
                'nodes': ['application'], 'ctest_tests': {'synthetic': [name]}}
            plan = dict(self.plan, selected_checks=['probe'])
            if required:
                with self.assertRaisesRegex(r.RealizationError, 'capabilit'):
                    r.resolve_checks(self.inventory, plan, real, 'synthetic')
            else:
                self.assertEqual(r.resolve_checks(self.inventory, plan, real, 'synthetic')['tests'], [name])

    def test_independent_build_contract_source_edge_negative(self):
        (self.root / 'component.c').write_text('int component(void) { return 42; }\n')
        cmake = self.root / 'CMakeLists.txt'
        cmake.write_text(cmake.read_text() + '\nadd_library(aggregate STATIC component.c)\n')
        self.inventory['path_rules'].append({'exact': 'component.c', 'nodes': ['other']})
        self.inventory['checks']['aggregate'] = {
            'kind': 'build_contract', 'profiles': ['synthetic'], 'nodes': ['other'],
            'build_targets': ['aggregate'], 'working_directory': '{source}', 'environment': {},
            'command': ['cmake', '--build', '{build}', '--target', 'aggregate']}
        real = self.configure()
        self.assertTrue(r.check_inputs(self.inventory, real)['passed'])
        bad = copy.deepcopy(self.inventory)
        bad['checks']['aggregate']['nodes'] = ['core']
        gaps = r.check_inputs(bad, real)['gaps']
        self.assertTrue(any(g['kind'] == 'missing_impact_edge' and
                            g.get('check') == 'aggregate' and
                            g.get('input') == 'component.c' for g in gaps))

    def test_independent_runtime_edge_and_unclaimed_registration_negatives(self):
        real = self.configure()
        self.assertTrue(r.check_inputs(self.inventory, real)['passed'])
        policy = copy.deepcopy(self.inventory)
        del policy['checks']['gui']
        gaps = r.check_inputs(policy, real)['gaps']
        self.assertIn({'kind': 'unclaimed_registration', 'test': 'gui'}, gaps)
        policy = copy.deepcopy(self.inventory)
        policy['nodes']['application']['depends_on'].remove('vectors')
        gaps = r.check_inputs(policy, real)['gaps']
        self.assertTrue(any(g['kind'] == 'missing_impact_edge' and
                            g.get('input') == 'vector.txt' for g in gaps))


if __name__ == '__main__':
    unittest.main()
