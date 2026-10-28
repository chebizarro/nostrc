"""Whole supported ordinary/root census; no subset or workflow acceptance claim."""
import copy
import unittest

from selector_support import ROOT, change, s
from realization_support import Project, r
from supported_profile_support import witness


class SupportedProfileMembership(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        cls.data = witness()


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
            self.assertEqual(self.p['profiles'][profile]['capabilities'],
                             ['display', 'private_bus', 'nak', 'gnome_keyring', 'signer_endpoint'])
            self.assertIn('signer_endpoint', self.p['profiles'][profile]['capabilities'])
            self.assertFalse(r._signer_diagnostic_boundary({'profile': self.p['profiles'][profile]})['complete'])

    def test_semantic_selection_savings_no_new_live_peer_or_unrelated_standalone_work(self):
        for path in ('apps/gnostr/src/main_app.c', 'nostr-gtk/data/nostr-gtk.gresource.xml',
                     'tests/test_json_filter.c',
                     'gnome/groundhog/data/org.nostr.Groundhog-autostart.desktop.in'):
            self.assertTrue((ROOT / path).is_file())
            plan = s.make_plan(self.p, [change(path)], head=self.data['base_sha'])
            live = lambda result: {c for c in result['selected_checks']
                                   if c.startswith(('mdk08.', 'mdk011.', 'prepared011.'))}
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
