"""Exact P3 W11 matrix integration enrollment/inverse, not measurement or W11 acceptance."""
import copy
import hashlib
import unittest
from selector_support import ROOT, s
import w11_matrix_integration_support as wave

PATH = 'scripts/w11-evidence.py'


class W11MatrixIntegration(unittest.TestCase):
    def policy(self):
        # Exactly this layer's result: the newer groundhog.linux app ownership layer is projected away first.
        from groundhog_app_enrollment_support import before_policy as before_newer_policy
        return before_newer_policy(s.load_inventory(ROOT / 'tests/selection/inventory.json'))

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


    # The leaf composes every sibling's exact source inverse: P3's evidence script,
    # .36.21's services readiness delta and .36.20's realization generation binding.
    # Each hands off exactly the bytes its next older layer records as its result.
    COMPOSED = {
        'scripts/w11-evidence.py': 'runtime',
        'scripts/test_selection_services.py': 'runtime',
        'scripts/test_selection_realization.py': 'post608c',
    }

    def current(self, path):
        # This leaf's after bytes: the newer W11 batch2 layer is projected away first.
        from w11_batch2_integration_support import before_source as before_batch2_source
        from w11_post_h_integration_support import before_source as before_post_h_source
        return before_batch2_source(path, before_post_h_source(path, (ROOT / path).read_bytes()))

    def older(self, path):
        import post608c_integration_support as post608c
        import runtime_integration_support as runtime
        layer = {'runtime': runtime, 'post608c': post608c}[self.COMPOSED[path]]
        return layer.witness()['sources'][path]

    def test_composed_source_records_are_exact_lossless_handoffs(self):
        import runtime_integration_support as runtime
        data = wave.witness()
        self.assertEqual(set(data['sources']), set(self.COMPOSED))
        for path, record in data['sources'].items():
            with self.subTest(path=path):
                raw = self.current(path)
                self.assertEqual(hashlib.sha256(raw).hexdigest(), record['after_sha256'])
                for edit in record['edits']:
                    self.assertEqual(raw.count(edit['after'].encode()), 1)
                before = wave.before_source(path, raw)
                self.assertEqual(hashlib.sha256(before).hexdigest(), record['before_sha256'])
                self.assertEqual(wave.before_source(path, before), before)
                self.assertEqual(record['before_sha256'], self.older(path)['after_sha256'])
                self.assertNotEqual(record['before_sha256'], record['after_sha256'])
                # Whole chain from the frozen current bytes to the oldest runtime witness.
                self.assertEqual(hashlib.sha256(runtime.before_runtime_source(path, raw)).hexdigest(),
                                 runtime.witness()['sources'][path]['before_sha256'])
        realization = data['sources']['scripts/test_selection_realization.py']
        self.assertEqual((realization['after_sha256'], realization['before_sha256'], len(realization['edits'])),
                         ('6b9da1ba7843c4589d1aad7a4cdc37a2dd38234ff88b12a03f87869033cf5099',
                          '941772636a5e46398c46577a50b22fa86e0dbd96c4034f2a0887e03b83e91a4e', 11))
        services = data['sources']['scripts/test_selection_services.py']
        self.assertEqual((services['after_sha256'], services['before_sha256'], len(services['edits'])),
                         ('1f54ff13b8471758a635527cb639d46664d665f527fada543257c1bcb7c811c7',
                          '652754d063bb3f35da08e4bf599a43897943c01609e5b1bcdbeadc9b936abe2c', 8))

    def test_tampered_or_unknown_bytes_refuse_for_every_composed_record(self):
        import runtime_integration_support as runtime
        for path in self.COMPOSED:  # fixed set: a missing record cannot pass vacuously
            record = wave.witness()['sources'][path]
            raw = self.current(path)
            before = wave.before_source(path, raw)
            drifted = [raw + b'\n# unknown bytes\n', before + b'\n# unknown bytes\n', raw[:-1]]
            for edit in record['edits']:
                start = raw.index(edit['after'].encode())
                middle = start + len(edit['after'].encode()) // 2
                drifted.append(raw[:middle] + bytes([raw[middle] ^ 0x01]) + raw[middle + 1:])
            for index, candidate in enumerate(drifted):
                with self.subTest(path=path, case=index):
                    with self.assertRaisesRegex(ValueError, 'W11 matrix integration source changed: ' + path):
                        wave.before_source(path, candidate)
                    with self.assertRaises(ValueError):
                        runtime.before_runtime_source(path, candidate)

    def test_tampered_witness_records_refuse(self):
        from unittest import mock
        original = wave.witness()
        path = 'scripts/test_selection_realization.py'
        raw = self.current(path)
        tampers = {
            'changed before text': (lambda r: r['edits'][0].__setitem__('before', r['edits'][0]['before'] + '#'),
                                    'W11 matrix historical source changed'),
            'dropped edit': (lambda r: r['edits'].pop(), 'W11 matrix historical source changed'),
            'ambiguous after text': (lambda r: r['edits'][0].__setitem__('after', '\n'),
                                     'ambiguous W11 matrix integration inverse'),
            'changed after hash': (lambda r: r.__setitem__('after_sha256', '0' * 64),
                                   'W11 matrix integration source changed'),
        }
        for name, (mutate, message) in tampers.items():
            data = copy.deepcopy(original)
            mutate(data['sources'][path])
            with self.subTest(tamper=name), mock.patch.object(wave, 'witness', return_value=data):
                with self.assertRaisesRegex(ValueError, message):
                    wave.before_source(path, raw)
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as directory:
            copied = Path(directory) / 'witness.json'
            copied.write_bytes((ROOT / wave.WITNESS).read_bytes().replace(b'"edits"', b'"edits" ', 1))
            with mock.patch.object(wave, 'WITNESS', str(copied)):
                with self.assertRaisesRegex(ValueError, 'W11 matrix integration witness changed'):
                    wave.before_source(path, raw)


if __name__ == '__main__':
    unittest.main()
