"""Exact core.linux.asan app ownership repair, not omission, execution, hosted or W11 acceptance."""
import copy
import unittest
from selector_support import ROOT, s
from realization_support import r
import coreasan_app_enrollment_support as wave

PROFILE, PEER = 'core.linux.asan', 'core.linux'


class CoreAsanAppEnrollment(unittest.TestCase):
    def policy(self):
        # Exactly this layer's result: the independent worker-prerequisite layer
        # (disjoint workflow_jobs/inputs) is projected away first.
        from worker_prerequisite_support import before_policy as before_worker_policy
        return before_worker_policy(s.load_inventory(ROOT / 'tests/selection/inventory.json'))

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
        before = wave.before_policy(policy)
        self.assertEqual(s.digest(before), data['base_policy_hash'])
        self.assertEqual(wave.before_policy(before), before)
        self.assertEqual({f for f in policy if policy[f] != before[f]}, {'checks'})
        changed = {i for i in policy['checks'] if policy['checks'][i] != before['checks'][i]}
        self.assertEqual(changed, set(data['maps']['checks']) | {'selection.contract'})
        import mdk011_app_enrollment_support as older
        import post1640_integration_support as oldest
        self.assertEqual(older.before_policy(policy), older.before_policy(before))
        self.assertEqual(s.digest(oldest.before_policy(policy)), oldest.witness()['base_policy_hash'])

    def test_projection_equals_existing_core_claims_and_nothing_else(self):
        policy, data = self.policy(), wave.witness()
        for ident, record in data['maps']['checks'].items():
            check = policy['checks'][ident]
            self.assertEqual(check['ctest_tests'][PROFILE], check['ctest_tests'][PEER], ident)
            actual = copy.deepcopy(check)
            actual['profiles'].remove(PROFILE)
            del actual['ctest_tests'][PROFILE]
            self.assertEqual(actual, record['before'], ident)
            self.assertTrue(set(check.get('requires', [])) <= set(policy['profiles'][PROFILE]['capabilities']))
        before = wave.before_policy(policy)
        for field in ('profiles', 'nodes', 'path_rules', 'diagnostic_nonmembers',
                      'diagnostic_nonmember_subsets', 'coverage_gaps'):
            self.assertEqual(policy[field], before[field], field)
        root_asan = {i for i in policy['checks'] if i.startswith('root.asan.')}
        self.assertTrue(root_asan)
        for ident in root_asan:
            self.assertEqual(policy['checks'][ident], before['checks'][ident], ident)

    def test_hosted_unclaimed_apps_reproduce_historically_and_are_exactly_claimed_now(self):
        policy, data = self.policy(), wave.witness()
        historical = self.claims(wave.before_policy(policy), PROFILE)
        current = self.claims(policy, PROFILE)
        registrations = set(data['registrations'])
        self.assertEqual(len(registrations), 45)
        self.assertFalse(registrations & set(historical))
        self.assertEqual(set(current) - set(historical), registrations)
        self.assertEqual({current[name] for name in registrations}, set(data['maps']['checks']))
        peer = self.claims(policy, PEER)
        for name in registrations:
            self.assertEqual(current[name], peer[name], name)
            facts = data['descriptor_sha256'][name]
            self.assertEqual(facts[PROFILE], facts[PEER], name)
        self.assertFalse(set(data['support_closure']) & set(current))

    def test_three_legacy_asan_exclusions_stay_unclaimed_and_blocking(self):
        policy, data = self.policy(), wave.witness()
        legacy = data['legacy_exclusions_not_enrolled']
        self.assertEqual(legacy['registrations'],
                         ['MockRelayUnit', 'concurrency_subscription_shutdown_asan', 'test_json_envelope'])
        self.assertEqual(data['hosted']['unclaimed_registrations'], len(data['registrations']) + 3)
        self.assertFalse(set(legacy['registrations']) & set(self.claims(policy, PROFILE)))
        for ident, entries in policy['diagnostic_nonmember_subsets'].items():
            self.assertNotIn(PROFILE, ident)
        gaps = [g for g in policy['coverage_gaps'] if g.get('id') == legacy['coverage_gap']]
        self.assertEqual(len(gaps), 1)
        self.assertIn('three legacy exclusions remain not_executed', gaps[0]['reason'])
        workflow = (ROOT / '.github/workflows/nostrc-ci.yml').read_text()
        self.assertIn('-E "test_json_envelope|concurrency_subscription_shutdown_asan|MockRelayUnit"', workflow)

    def test_every_hosted_missing_edge_resolves_without_ownership_changes(self):
        policy, data = self.policy(), wave.witness()
        claims = self.claims(policy, PROFILE)
        historical = wave.before_policy(policy)
        self.assertEqual(len(data['missing_impact_edges']), 291)
        for test, path, kind, owners in data['missing_impact_edges']:
            self.assertIn(test, data['registrations'])
            current_owners, effect = r._owners(policy, path)
            self.assertEqual(sorted(current_owners), owners, path)
            self.assertEqual(r._owners(historical, path), (current_owners, effect), path)
            self.assertNotEqual(effect, 'global', path)
            covered = r._impact_prerequisites(policy, policy['checks'][claims[test]]['nodes'])
            self.assertTrue(set(owners) & covered, (test, path))

    def test_altered_or_partial_enrollment_refuses(self):
        policy, data = self.policy(), wave.witness()
        ident = sorted(data['maps']['checks'])[0]
        changed = copy.deepcopy(policy)
        changed['checks'][ident]['ctest_tests'][PROFILE].append('MockRelayUnit')
        with self.assertRaisesRegex(ValueError, 'core ASan app enrollment check changed'):
            wave.before_policy(changed)
        partial = copy.deepcopy(policy)
        partial['checks']['selection.contract']['runtime_inputs'].remove(data['runtime_inputs'][0])
        with self.assertRaisesRegex(ValueError, 'partially stripped'):
            wave.before_policy(partial)
        stripped = wave.before_policy(policy)
        stripped['checks'][ident]['profiles'].append(PROFILE)
        with self.assertRaisesRegex(ValueError, 'projection without enrollment'):
            wave.before_policy(stripped)
        stripped = wave.before_policy(policy)
        stripped['checks'][ident]['ctest_tests'][PROFILE] = ['test-bc-blob-store']
        with self.assertRaisesRegex(ValueError, 'projection without enrollment'):
            wave.before_policy(stripped)
        # An older-layer mutation of the same check is left to the older strict inverse.
        import mdk011_app_enrollment_support as older
        mutated = wave.before_policy(policy)
        mutated['checks'][ident]['ctest_tests']['groundhog.mdk011'].append('invented-registration')
        self.assertEqual(wave.before_policy(mutated), mutated)
        with self.assertRaisesRegex(ValueError, 'MDK011 app enrollment check changed'):
            older.before_policy(mutated)


if __name__ == '__main__':
    unittest.main()
