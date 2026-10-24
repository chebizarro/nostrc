"""Exact P3 W11 matrix integration enrollment/inverse, not measurement or W11 acceptance."""
import copy
import hashlib
import unittest
from selector_support import ROOT, s
import w11_matrix_integration_support as wave

PATH = 'scripts/w11-evidence.py'


class W11MatrixIntegration(unittest.TestCase):
    def policy(self):
        return s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_exact_source_inverse_reaches_runtime_witness_and_refuses_unreviewed(self):
        import runtime_integration_support as runtime
        record = wave.witness()['sources'][PATH]
        raw = (ROOT / PATH).read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), record['after_sha256'])
        before = wave.before_source(PATH, raw)
        self.assertEqual(hashlib.sha256(before).hexdigest(), record['before_sha256'])
        self.assertEqual(wave.before_source(PATH, before), before)
        # The original runtime witness records exactly these pre-P3 bytes as its result.
        self.assertEqual(runtime.witness()['sources'][PATH]['after_sha256'], record['before_sha256'])
        self.assertEqual(hashlib.sha256(runtime.before_runtime_source(PATH, raw)).hexdigest(),
                         runtime.witness()['sources'][PATH]['before_sha256'])
        for drifted in (raw + b'\n# unreviewed\n', before + b'\n# unreviewed\n'):
            with self.assertRaises(ValueError):
                wave.before_source(PATH, drifted)
            with self.assertRaises(ValueError):
                runtime.before_runtime_source(PATH, drifted)
        other = 'scripts/test-plan.py'
        self.assertEqual(wave.before_source(other, (ROOT / other).read_bytes()), (ROOT / other).read_bytes())

    def test_exact_enrollment_only_and_composed_bases(self):
        policy, data = self.policy(), wave.witness()
        before = wave.before_policy(policy)
        self.assertEqual(s.digest(before), data['base_policy_hash'])
        self.assertEqual(wave.before_policy(before), before)
        self.assertEqual({f for f in policy if policy[f] != before[f]}, {'checks', 'path_rules'})
        self.assertEqual({i for i in policy['checks'] if policy['checks'][i] != before['checks'][i]},
                         {'selection.contract'})
        inputs = policy['checks']['selection.contract']['runtime_inputs']
        self.assertEqual(inputs[-len(data['runtime_inputs']):], data['runtime_inputs'])
        for path in data['runtime_inputs']:
            relative = path.removeprefix('{source}/')
            self.assertEqual(inputs.count(path), 1)
            self.assertTrue((ROOT / relative).is_file(), relative)
            classified = s.classify(policy, relative)
            self.assertEqual(classified['effect'], 'global')
            self.assertIn('selection.contract', classified['nodes'])
        p3 = data['p3_test']
        self.assertEqual(hashlib.sha256((ROOT / p3['path']).read_bytes()).hexdigest(), p3['sha256'])
        self.assertNotEqual(p3['p3_sha256'], p3['sha256'])  # P3 original plus reviewed cancellation regressions
        record = data['sources']['scripts/w11-evidence.py']
        self.assertEqual(len({record['before_sha256'], record['p3_sha256'], record['after_sha256']}), 3)
        import post1640_integration_support as oldest
        self.assertEqual(s.digest(oldest.before_policy(policy)), oldest.witness()['base_policy_hash'])

    def test_exact_two_p3_evidence_files_classified_and_unknown_paths_still_broaden(self):
        from selector_support import plan
        policy, data = self.policy(), wave.witness()
        evidence = 'docs/reviews/dependency-aware-testing-w11-p3-evidence-2026-10-24.json'
        matrix = 'docs/reviews/dependency-aware-testing-w11-p3-matrix-2026-10-24.json'
        self.assertEqual([r['exact'] for r in data['added_path_rules']], [evidence, matrix])
        self.assertTrue(all('prefix' not in r for r in data['added_path_rules']))
        for path, nodes in ((evidence, []), (matrix, ['selection.contract'])):
            self.assertTrue((ROOT / path).is_file(), path)
            classified = s.classify(policy, path)
            self.assertFalse(classified['unclassified'], path)
            self.assertEqual((classified['effect'], classified['nodes']), ('global', nodes), path)
            self.assertEqual(sum(r.get('exact') == path for r in policy['path_rules']), 1, path)
            # Historically these bytes were exactly the hosted unclassified finding.
            self.assertTrue(s.classify(wave.before_policy(policy), path)['unclassified'], path)
        # The matrix manifest's actual consumer is the selection contract test.
        self.assertIn(matrix, (ROOT / 'tests/selection/test_w11_matrix.py').read_text())
        # Unrelated new non-prose docs paths (including near-name variants) remain unclassified
        # and broaden; prose stays on the existing allowlist.
        for unknown in ('docs/reviews/dependency-aware-testing-w11-p3-matrix-2026-10-25.json',
                        'docs/reviews/dependency-aware-testing-w11-p3-unrelated-2026-10-24.json',
                        'docs/reviews/new-evidence.json'):
            classified = s.classify(policy, unknown)
            self.assertTrue(classified['unclassified'], unknown)
            self.assertEqual(classified['effect'], 'global', unknown)
            self.assertNotEqual(plan(policy, paths=(unknown,))['effective_mode'], 'affected', unknown)
        self.assertEqual(s.classify(policy, 'docs/reviews/new-notes.md')['effect'], 'non_execution')

    def test_classification_without_enrollment_or_partial_refuses(self):
        policy, data = self.policy(), wave.witness()
        stripped = wave.before_policy(policy)
        stripped['path_rules'].append(copy.deepcopy(data['added_path_rules'][0]))
        with self.assertRaisesRegex(ValueError, 'classification without enrollment'):
            wave.before_policy(stripped)
        partial = copy.deepcopy(policy)
        partial['path_rules'].remove(data['added_path_rules'][1])
        with self.assertRaisesRegex(ValueError, 'classification partially stripped'):
            wave.before_policy(partial)

    def test_partial_or_duplicated_enrollment_refuses(self):
        data = wave.witness()
        for path in data['runtime_inputs']:
            for duplicate in (False, True):
                policy = self.policy()
                inputs = policy['checks']['selection.contract']['runtime_inputs']
                inputs.append(path) if duplicate else inputs.remove(path)
                with self.subTest(path=path, duplicate=duplicate), self.assertRaises(ValueError):
                    wave.before_policy(policy)


if __name__ == '__main__':
    unittest.main()
