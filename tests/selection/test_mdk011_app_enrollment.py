"""Exact MDK011 app registration enrollment, not execution, hosted or W11 acceptance."""
import copy
import unittest
from selector_support import ROOT, s
from realization_support import r
import mdk011_app_enrollment_support as wave

PROFILE, PEER = 'groundhog.mdk011', 'groundhog.mdk08'


class Mdk011AppEnrollment(unittest.TestCase):
    def policy(self):
        # Exactly this layer's result: newer exact layers are projected away first.
        from coreasan_app_enrollment_support import before_policy as before_newer_policy
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
        before = wave.before_policy(policy)
        self.assertEqual(wave.before_policy(before), before)
        changed = {field for field in policy if policy[field] != before[field]}
        self.assertEqual(changed, {'checks'})
        changed = {ident for ident in policy['checks'] if policy['checks'][ident] != before['checks'][ident]}
        self.assertEqual(changed, set(data['maps']['checks']) | {'selection.contract'})
        import post1640_integration_support as newer
        self.assertEqual(s.digest(newer.before_policy(policy)), newer.witness()['base_policy_hash'])
        self.assertEqual(s.digest(newer.before_policy(before)), newer.witness()['base_policy_hash'])

    def test_projection_equals_existing_mdk08_claims_and_nothing_else(self):
        policy, data = self.policy(), wave.witness()
        before = wave.before_policy(policy)
        for ident, record in data['maps']['checks'].items():
            check = policy['checks'][ident]
            self.assertEqual(check['ctest_tests'][PROFILE], check['ctest_tests'][PEER], ident)
            actual = copy.deepcopy(check)
            actual['profiles'].remove(PROFILE)
            del actual['ctest_tests'][PROFILE]
            self.assertEqual(actual, record['before'], ident)
            self.assertTrue(set(check.get('requires', [])) <= set(policy['profiles'][PROFILE]['capabilities']))
        self.assertEqual(policy['profiles'], before['profiles'])
        self.assertEqual(policy['diagnostic_nonmembers'], before['diagnostic_nonmembers'])
        self.assertEqual(policy['diagnostic_nonmember_subsets'], before['diagnostic_nonmember_subsets'])
        self.assertEqual(policy['coverage_gaps'], before['coverage_gaps'])

    def test_hosted_unclaimed_set_reproduces_historically_and_is_exactly_claimed_now(self):
        policy, data = self.policy(), wave.witness()
        historical = self.claims(wave.before_policy(policy), PROFILE)
        current = self.claims(policy, PROFILE)
        registrations = set(data['registrations'])
        self.assertEqual(len(registrations), 46)
        # The original hosted audit finding: none were claimed before.
        self.assertFalse(registrations & set(historical))
        self.assertEqual(set(current) - set(historical), registrations)
        self.assertEqual({current[name] for name in registrations}, set(data['maps']['checks']))
        peer = self.claims(policy, PEER)
        for name in registrations:
            self.assertEqual(current[name], peer[name], name)
            facts = data['descriptor_sha256'][name]
            self.assertEqual(facts[PROFILE], facts[PEER], name)
        # Support fixtures stay support, never primary claims.
        self.assertFalse(set(data['support_closure']) & set(current))

    def test_every_hosted_missing_edge_resolves_without_ownership_changes(self):
        policy, data = self.policy(), wave.witness()
        claims = self.claims(policy, PROFILE)
        historical = wave.before_policy(policy)
        self.assertEqual(len(data['missing_impact_edges']), 316)
        for test, path, kind, owners in data['missing_impact_edges']:
            self.assertIn(test, data['registrations'])
            current_owners, effect = r._owners(policy, path)
            self.assertEqual(sorted(current_owners), owners, path)
            self.assertEqual(r._owners(historical, path), (current_owners, effect), path)
            self.assertNotEqual(effect, 'global', path)
            check = policy['checks'][claims[test]]
            covered = r._impact_prerequisites(policy, check['nodes'])
            self.assertTrue(set(owners) & covered, (test, path))

    def test_altered_or_partial_enrollment_refuses(self):
        policy = self.policy()
        ident = sorted(wave.witness()['maps']['checks'])[0]
        changed = copy.deepcopy(policy)
        changed['checks'][ident]['ctest_tests'][PROFILE].append('invented-registration')
        with self.assertRaisesRegex(ValueError, 'MDK011 app enrollment check changed'):
            wave.before_policy(changed)
        partial = copy.deepcopy(policy)
        partial['checks']['selection.contract']['runtime_inputs'].remove(wave.witness()['runtime_inputs'][0])
        with self.assertRaisesRegex(ValueError, 'partially stripped'):
            wave.before_policy(partial)
        stripped = wave.before_policy(policy)
        stripped['checks'][ident]['profiles'].append(PROFILE)
        with self.assertRaisesRegex(ValueError, 'historical check changed'):
            wave.before_policy(stripped)


if __name__ == '__main__':
    unittest.main()
