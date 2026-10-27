"""The newest inverse layer removes exactly its recorded policy items and nothing else."""
import copy
import hashlib
import unittest

from selector_support import ROOT, s
import inventory_option_drift_support as layer

PROFILES = ('core.linux', 'core.linux.asan', 'grotto.linux', 'grotto.linux.asan_ubsan',
            'groundhog.linux', 'groundhog.mdk011', 'groundhog.mdk08')
OPTIONS = ['-DBUILD_LIBVAULT=ON', '-DENABLE_NIP42=ON', '-DENABLE_NIP11=ON']


class InventoryOptionDrift(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_witness_is_pinned_and_lists_only_the_known_edits(self):
        data = layer.witness()
        self.assertEqual(hashlib.sha256((ROOT / layer.WITNESS).read_bytes()).hexdigest(), layer.SHA256)
        self.assertEqual([(r['section'], r['ident']) for r in data['records']],
                         [('checks', 'groundhog.default_build')] + [('profiles', p) for p in PROFILES])

    def test_projection_removes_exactly_the_recorded_items(self):
        previous = layer.before_policy(self.policy)
        build = self.policy['checks']['groundhog.default_build']['nodes']
        self.assertEqual([n for n in build if n not in previous['checks']['groundhog.default_build']['nodes']],
                         ['nip65', 'nip65.test_inputs'])
        for name in PROFILES:
            current = self.policy['profiles'][name]['configure_args']
            self.assertEqual(current[:3], OPTIONS)
            self.assertEqual(previous['profiles'][name]['configure_args'], current[3:])
        restored = copy.deepcopy(previous)
        restored['checks']['groundhog.default_build']['nodes'] = build
        for name in PROFILES:
            restored['profiles'][name]['configure_args'] = self.policy['profiles'][name]['configure_args']
        self.assertEqual(restored, self.policy)
        self.assertEqual(layer.before_policy(previous), previous)

    def test_other_differences_and_partial_edits_refuse_or_pass_through(self):
        forged = copy.deepcopy(self.policy)
        forged['checks']['groundhog.default_build']['nodes'].remove('nip65.test_inputs')
        with self.assertRaisesRegex(s.ContractError, 'partially present'):
            layer.before_policy(forged)
        forged = copy.deepcopy(self.policy)
        forged['profiles']['core.linux']['configure_args'][1] = '-DENABLE_NIP11=ON'
        with self.assertRaisesRegex(s.ContractError, 'partially present'):
            layer.before_policy(forged)
        unrelated = copy.deepcopy(self.policy)
        unrelated['profiles']['core.linux']['configure_args'].append('-DUNRELATED=ON')
        self.assertEqual(layer.before_policy(unrelated)['profiles']['core.linux']['configure_args'][-1],
                         '-DUNRELATED=ON')


if __name__ == '__main__':
    unittest.main()
