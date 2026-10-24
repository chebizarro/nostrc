"""Exact groundhog.linux 46-app ownership repair, not omission, execution, hosted or W11 acceptance."""
import copy
import unittest
from selector_support import ROOT, s
from realization_support import r
import groundhog_app_enrollment_support as wave

PROFILE, PEER = 'groundhog.linux', 'groundhog.mdk08'
DEFAULT, FAMILY = 'groundhog.default_build', 'grotto.default_build'


class GroundhogAppEnrollment(unittest.TestCase):
    def policy(self):
        # Exactly this layer's view: the newer W11 batch2 integration layer is projected away first.
        from w11_batch2_integration_support import before_policy as before_newer_policy
        return before_newer_policy(s.load_inventory(ROOT / 'tests/selection/inventory.json'))

    def claims(self, policy, profile):
        claims = {}
        for ident, check in policy['checks'].items():
            if check['kind'] == 'ctest' and profile in check['profiles']:
                for name in r._membership(check, profile):
                    self.assertNotIn(name, claims, (profile, name))
                    claims[name] = ident
        return claims

    def test_exact_inverse_and_composed_historical_projection(self):
        policy, data = self.policy(), wave.witness()
        self.assertEqual(s.digest(policy), data['enrolled_policy_hash'])
        before = wave.before_policy(policy)
        self.assertEqual(s.digest(before), data['base_policy_hash'])
        # The hosted failing audit was bound to exactly the base inventory.
        self.assertEqual(data['base_policy_hash'], '36bf71a0eab2ca5a2d98033f36ca8d72b7210f7c5a3dc57deefbeddad48e63eb')
        self.assertEqual(wave.before_policy(before), before)
        self.assertEqual({f for f in policy if policy[f] != before[f]}, {'checks'})
        changed = {i for i in policy['checks'] if policy['checks'][i] != before['checks'][i]}
        self.assertEqual(changed, set(data['maps']['checks']) | {'selection.contract'})
        inputs = policy['checks']['selection.contract']['runtime_inputs']
        self.assertEqual(inputs[-len(data['runtime_inputs']):], data['runtime_inputs'])
        for path in data['runtime_inputs']:
            self.assertTrue((ROOT / path.removeprefix('{source}/')).is_file(), path)
        import w11_matrix_integration_support as newer_matrix
        import coreasan_app_enrollment_support as coreasan
        import post1640_integration_support as oldest
        self.assertEqual(s.digest(newer_matrix.before_policy(policy)), newer_matrix.witness()['base_policy_hash'])
        self.assertEqual(coreasan.before_policy(policy), coreasan.before_policy(before))
        self.assertEqual(s.digest(oldest.before_policy(policy)), oldest.witness()['base_policy_hash'])

    def test_projection_equals_existing_mdk08_claims_and_nothing_else(self):
        policy, data = self.policy(), wave.witness()
        nine = set(data['maps']['checks']) - {DEFAULT}
        self.assertEqual(len(nine), 9)
        import mdk011_app_enrollment_support as mdk011
        self.assertEqual(nine, set(mdk011.witness()['maps']['checks']))
        for ident in nine:
            check = policy['checks'][ident]
            self.assertEqual(check['ctest_tests'][PROFILE], check['ctest_tests'][PEER], ident)
            actual = copy.deepcopy(check)
            actual['profiles'].remove(PROFILE)
            del actual['ctest_tests'][PROFILE]
            self.assertEqual(actual, data['maps']['checks'][ident]['before'], ident)
            self.assertTrue(set(check.get('requires', [])) <= set(policy['profiles'][PROFILE]['capabilities']))
        before = wave.before_policy(policy)
        for field in ('profiles', 'nodes', 'path_rules', 'diagnostic_nonmembers',
                      'diagnostic_nonmember_subsets', 'coverage_gaps', 'workflow_jobs'):
            self.assertEqual(policy[field], before[field], field)

    def test_hosted_unclaimed_apps_reproduce_historically_and_are_exactly_claimed_now(self):
        policy, data = self.policy(), wave.witness()
        historical = self.claims(wave.before_policy(policy), PROFILE)
        current = self.claims(policy, PROFILE)
        registrations = set(data['registrations'])
        self.assertEqual(len(registrations), data['hosted']['gap_counts']['unclaimed_registration'])
        self.assertFalse(registrations & set(historical))
        self.assertEqual(set(current) - set(historical), registrations)
        self.assertEqual({current[name] for name in registrations}, set(data['maps']['checks']) - {DEFAULT})
        peer = self.claims(policy, PEER)
        for name in registrations | set(data['support_closure']):
            facts = data['descriptor_sha256'][name]
            self.assertEqual(facts[PROFILE], facts[PEER], name)
        for name in registrations:
            self.assertEqual(current[name], peer[name], name)
        # Support fixtures are pulled in by fixture closure, never claimed as primaries.
        self.assertFalse(set(data['support_closure']) & (set(current) | set(peer)))
        # The hosted fresh universe grew by exactly these registrations and fixtures.
        universe = policy['diagnostic_nonmember_subsets'][PROFILE]['registration_universe']
        self.assertFalse((registrations | set(data['support_closure'])) & set(universe))

    def test_exact33_full_ownership_and_historical_193_review_unchanged(self):
        import test_selection_subset as subset
        policy = self.policy()
        before = wave.before_policy(policy)
        self.assertEqual(subset.full_profiles(policy), subset.full_profiles(before))
        self.assertIn(PROFILE, subset.full_profiles(policy))
        self.assertEqual(policy['checks']['supported.libgo'], before['checks']['supported.libgo'])
        record = policy['diagnostic_nonmember_subsets'][PROFILE]
        self.assertEqual(len(record['registrations']), 33)
        self.assertEqual(len(record['review']['source_inputs']), 193)
        self.assertEqual(record, before['diagnostic_nonmember_subsets'][PROFILE])
        # Claims of the 33 stay exactly the single full-ownership check.
        claims = self.claims(policy, PROFILE)
        self.assertEqual({claims[name] for name in subset.REGISTRATIONS}, {'supported.libgo'})

    def test_every_hosted_registration_edge_resolves_without_ownership_changes(self):
        policy, data = self.policy(), wave.witness()
        claims = self.claims(policy, PROFILE)
        historical = wave.before_policy(policy)
        self.assertEqual(len(data['missing_impact_edges']), data['hosted']['gap_counts']['registration_edges'])
        for test, path, kind, owners in data['missing_impact_edges']:
            self.assertIn(test, data['registrations'])
            current_owners, effect = r._owners(policy, path)
            self.assertEqual(sorted(current_owners), owners, path)
            self.assertEqual(r._owners(historical, path), (current_owners, effect), path)
            self.assertNotIn(effect, ('global', 'non_execution'), path)
            covered = r._impact_prerequisites(policy, policy['checks'][claims[test]]['nodes'])
            self.assertTrue(set(owners) & covered, (test, path))

    def test_every_hosted_default_build_edge_resolves_and_each_node_is_necessary(self):
        policy, data = self.policy(), wave.witness()
        historical = wave.before_policy(policy)
        edges = data['default_build_missing_edges']
        self.assertEqual(len(edges), data['hosted']['gap_counts']['default_build_edges'])
        nodes = policy['checks'][DEFAULT]['nodes']
        added = data['added_default_nodes']
        self.assertEqual(set(nodes) - set(historical['checks'][DEFAULT]['nodes']), set(added))
        # Exactly the app-enabled default-build family node set, not a new grouping.
        self.assertEqual([n for n in nodes if n != 'execution.groundhog'],
                         [n for n in policy['checks'][FAMILY]['nodes'] if n != 'execution.grotto'])
        for field in ('build_scope', 'build_targets', 'command', 'profiles', 'runtime_inputs', 'environment'):
            self.assertEqual(policy['checks'][DEFAULT][field], historical['checks'][DEFAULT][field], field)
        covered = r._impact_prerequisites(policy, nodes)
        before = r._impact_prerequisites(historical, historical['checks'][DEFAULT]['nodes'])
        for path, owners in edges:
            current, effect = r._owners(policy, path)
            self.assertEqual((sorted(current), effect), (owners, 'impact'), path)
            self.assertEqual(r._owners(historical, path), (current, effect), path)
            self.assertFalse(current & before, path)  # causal: really missing historically
            self.assertTrue(current & covered, path)
        for node in added:
            trial = r._impact_prerequisites(policy, [n for n in nodes if n != node])
            self.assertTrue(any(not set(owners) & trial for _, owners in edges), node)

    def test_selection_delta_is_only_the_default_build_for_app_inputs(self):
        from selector_support import change
        policy = self.policy()
        before = wave.before_policy(policy)
        compiled = 'apps/grotto/src/accounts_store.c'
        self.assertIn(compiled, {path for path, _ in wave.witness()['default_build_missing_edges']})
        for path in (compiled, 'apps/grelay/CMakeLists.txt', 'apps/blossom-cache/CMakeLists.txt'):
            current = s.make_plan(policy, [change(path)], head='a' * 40, base='b' * 40)
            old = s.make_plan(before, [change(path)], head='a' * 40, base='b' * 40)
            self.assertEqual(set(current['selected_checks']) - set(old['selected_checks']), {DEFAULT}, path)
            self.assertEqual(set(old['selected_checks']) - set(current['selected_checks']), set(), path)
            self.assertFalse(current['enforcement_ready'])
        # Unrelated Groundhog-only inputs keep their previous selection.
        path = 'gnome/groundhog/src/main.c'
        self.assertEqual(s.make_plan(policy, [change(path)], head='a' * 40, base='b' * 40)['selected_checks'],
                         s.make_plan(before, [change(path)], head='a' * 40, base='b' * 40)['selected_checks'])

    def test_altered_partial_duplicated_or_unknown_enrollment_refuses(self):
        policy, data = self.policy(), wave.witness()
        ident = sorted(set(data['maps']['checks']) - {DEFAULT})[0]
        changed = copy.deepcopy(policy)
        changed['checks'][ident]['ctest_tests'][PROFILE].append('signer/unknown-registration')
        with self.assertRaisesRegex(ValueError, 'groundhog app enrollment check changed'):
            wave.before_policy(changed)
        duplicated = copy.deepcopy(policy)
        duplicated['checks'][ident]['ctest_tests'][PROFILE].append(duplicated['checks'][ident]['ctest_tests'][PROFILE][0])
        with self.assertRaisesRegex(ValueError, 'groundhog app enrollment check changed'):
            wave.before_policy(duplicated)
        widened = copy.deepcopy(policy)
        widened['checks'][DEFAULT]['nodes'].append('version.contract')
        with self.assertRaisesRegex(ValueError, 'groundhog app enrollment check changed'):
            wave.before_policy(widened)
        partial = copy.deepcopy(policy)
        partial['checks']['selection.contract']['runtime_inputs'].remove(data['runtime_inputs'][0])
        with self.assertRaisesRegex(ValueError, 'partially stripped'):
            wave.before_policy(partial)
        twice = copy.deepcopy(policy)
        twice['checks']['selection.contract']['runtime_inputs'].append(data['runtime_inputs'][-1])
        with self.assertRaisesRegex(ValueError, 'missing/duplicated'):
            wave.before_policy(twice)
        stripped = wave.before_policy(policy)
        stripped['checks'][ident]['profiles'].append(PROFILE)
        with self.assertRaisesRegex(ValueError, 'projection without enrollment'):
            wave.before_policy(stripped)
        stripped = wave.before_policy(policy)
        stripped['checks'][DEFAULT]['nodes'].append(data['added_default_nodes'][0])
        with self.assertRaisesRegex(ValueError, 'nodes without enrollment'):
            wave.before_policy(stripped)
        # An older-layer mutation of the same check is left to the older strict inverse.
        import mdk011_app_enrollment_support as older
        mutated = wave.before_policy(policy)
        mutated['checks'][ident]['ctest_tests']['groundhog.mdk011'].append('invented-registration')
        self.assertEqual(wave.before_policy(mutated), mutated)
        with self.assertRaisesRegex(ValueError, 'core ASan app enrollment check changed|MDK011 app enrollment check changed'):
            older.before_policy(mutated)

    def test_changed_witness_bytes_refuse(self):
        import hashlib
        raw = (ROOT / wave.WITNESS).read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), wave.SHA256)
        self.assertNotEqual(hashlib.sha256(raw + b' ').hexdigest(), wave.SHA256)


if __name__ == '__main__':
    unittest.main()
