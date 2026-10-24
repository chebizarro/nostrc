"""Exact post-H integration inverse; not execution, hosted, signer or W11 acceptance."""
import copy
import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock
from selector_support import ROOT, s
import w11_post_h_integration_support as wave
import w11_batch2_integration_support as batch2

H = 'aebb6eff6dd879033fb38bb016ea3127232aa37e'
# Fixed composed set: path -> (before = H bytes, after = current bytes, edits).
RECORDS = {
    'scripts/test_selection_services.py': ('4d234d8b45c8e2bf61d4e28b07fa73fd35056c9f88d18ed11a748185bac4febe',
                                           'd786771ce8a9', 3),
    'scripts/test_selection_execution.py': ('16f8c1357e8772785ee717fcf972a6117d2ea30a0f6c7a134ef03fee978668bd',
                                            '7dc389951919', 4),
    'tests/selection/test_post1640_integration.py': (
        '236dec6d6109ab68c06fb476b15e7d46a8afc5b658790147dd208b33d8312a2e', 'dfc0990ead48', 4),
}


class PostHIntegration(unittest.TestCase):
    def policy(self):
        return s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_h_era_witnesses_are_byte_identical_to_pushed_h(self):
        for path in (batch2.WITNESS, 'tests/selection/fixtures/w11_matrix_integration_20261024.json',
                     'tests/selection/fixtures/post1640_integration_20261024.json',
                     'tests/selection/fixtures/worker_prerequisite_20261024.json'):
            with self.subTest(path=path):
                pushed = subprocess.run(['git', '-C', str(ROOT), 'show', f'{H}:{path}'], capture_output=True, check=True).stdout
                self.assertEqual((ROOT / path).read_bytes(), pushed)

    def test_records_are_exact_lossless_handoffs_to_the_h_era_layer(self):
        data = wave.witness()
        self.assertEqual(set(data['sources']), set(RECORDS))
        old = batch2.witness()['sources']
        for path, (before_sha, after_prefix, count) in RECORDS.items():
            record = data['sources'][path]
            with self.subTest(path=path):
                raw = (ROOT / path).read_bytes()
                self.assertEqual(hashlib.sha256(raw).hexdigest(), record['after_sha256'])
                self.assertTrue(record['after_sha256'].startswith(after_prefix))
                self.assertEqual((record['before_sha256'], len(record['edits'])), (before_sha, count))
                before = wave.before_source(path, raw)
                self.assertEqual(hashlib.sha256(before).hexdigest(), before_sha)
                self.assertEqual(wave.before_source(path, before), before)
                # H bytes are exactly the H-era record's after bytes; restored states are
                # exactly what that layer accepts unchanged, nothing generic.
                self.assertEqual(before_sha, old[path]['after_sha256'])
                self.assertEqual(record['restored_sha256'], [old[path]['before_sha256'], *old[path].get('restored_sha256', [])])
                for restored in record['restored_sha256']:
                    self.assertNotIn(restored, (before_sha, record['after_sha256']))
                self.assertEqual(hashlib.sha256(batch2.before_source(path, before)).hexdigest(), old[path]['before_sha256'])

    def test_whole_chains_reach_older_witnesses_from_current_bytes(self):
        import runtime_integration_support as runtime
        import post1640_integration_support as post1640
        import coreasan_app_enrollment_support as coreasan
        import w11_matrix_integration_support as matrix
        for path in ('scripts/test_selection_services.py',):
            raw = (ROOT / path).read_bytes()
            self.assertEqual(hashlib.sha256(runtime.before_runtime_source(path, raw)).hexdigest(),
                             runtime.witness()['sources'][path]['before_sha256'])
            self.assertEqual(hashlib.sha256(coreasan.before_source(path, raw)).hexdigest(),
                             matrix.witness()['sources'][path]['before_sha256'])
        path = 'scripts/test_selection_execution.py'
        raw = (ROOT / path).read_bytes()
        previous = post1640.before_source(path, raw)
        self.assertEqual(hashlib.sha256(previous).hexdigest(), post1640.witness()['sources'][path]['before_sha256'])
        self.assertEqual(post1640.before_source(path, previous), previous)  # post-1640 restored state passes

    def test_unknown_truncated_or_flipped_bytes_refuse_for_every_record(self):
        import runtime_integration_support as runtime
        for path, record in wave.witness()['sources'].items():
            raw = (ROOT / path).read_bytes()
            before = wave.before_source(path, raw)
            drifted = [raw + b'\n# unknown bytes\n', before + b'\n# unknown bytes\n', raw[:-1]]
            for restored in record['restored_sha256']:
                drifted.append(b'# not restored bytes ' + restored.encode())
            for edit in record['edits']:
                start = raw.index(edit['after'].encode())
                middle = start + len(edit['after'].encode()) // 2
                drifted.append(raw[:middle] + bytes([raw[middle] ^ 0x01]) + raw[middle + 1:])
            for index, candidate in enumerate(drifted):
                with self.subTest(path=path, case=index):
                    with self.assertRaisesRegex(ValueError, 'post-H integration source changed: ' + path):
                        wave.before_source(path, candidate)
                    if path.startswith('scripts/'):
                        with self.assertRaises(ValueError):
                            runtime.before_runtime_source(path, candidate)

    def test_tampered_partial_duplicate_or_malformed_records_refuse(self):
        original = wave.witness()
        path = 'scripts/test_selection_services.py'
        raw = (ROOT / path).read_bytes()
        tampers = {
            'changed before text': (lambda r: r['edits'][0].__setitem__('before', r['edits'][0]['before'] + '#'),
                                    'post-H historical source changed'),
            'dropped edit': (lambda r: r['edits'].pop(), 'post-H historical source changed'),
            'duplicated edit': (lambda r: r['edits'].append(copy.deepcopy(r['edits'][0])), 'ambiguous post-H integration inverse'),
            'ambiguous after text': (lambda r: r['edits'][0].__setitem__('after', '\n'), 'ambiguous post-H integration inverse'),
            'changed after hash': (lambda r: r.__setitem__('after_sha256', '0' * 64), 'post-H integration source changed'),
            'missing restored field': (lambda r: r.pop('restored_sha256'), 'malformed post-H integration record'),
            'extra wildcard field': (lambda r: r.__setitem__('pattern', '*'), 'malformed post-H integration record'),
            'empty edits': (lambda r: r.__setitem__('edits', []), 'malformed post-H integration record'),
            'edit not an object': (lambda r: r['edits'].__setitem__(0, ['a', 'b']), 'malformed post-H integration edit'),
            'restored not a list': (lambda r: r.__setitem__('restored_sha256', 'any'), 'malformed post-H integration record'),
        }
        for name, (mutate, message) in tampers.items():
            data = copy.deepcopy(original)
            mutate(data['sources'][path])
            with self.subTest(tamper=name), mock.patch.object(wave, 'witness', return_value=data):
                with self.assertRaisesRegex(ValueError, message):
                    wave.before_source(path, raw)
        with tempfile.TemporaryDirectory() as directory:
            copied = Path(directory) / 'witness.json'
            copied.write_bytes((ROOT / wave.WITNESS).read_bytes().replace(b'"edits"', b'"edits" ', 1))
            with mock.patch.object(wave, 'WITNESS', str(copied)):
                with self.assertRaisesRegex(ValueError, 'post-H integration witness changed'):
                    wave.before_source(path, raw)

    def test_exact_policy_inverse_reaches_h_and_is_idempotent(self):
        import groundhog_app_enrollment_support as groundhog
        import post1640_integration_support as oldest
        policy, data = self.policy(), wave.witness()
        self.assertEqual(s.digest(policy), data['enrolled_policy_hash'])
        before = wave.before_policy(policy)
        self.assertEqual(s.digest(before), data['base_policy_hash'])
        self.assertEqual(data['base_policy_hash'], batch2.witness()['enrolled_policy_hash'])
        self.assertEqual(wave.before_policy(before), before)
        self.assertEqual({f for f in policy if policy[f] != before[f]}, {'checks'})
        self.assertEqual({i for i in policy['checks'] if policy['checks'][i] != before['checks'][i]}, {'selection.contract'})
        inputs = policy['checks']['selection.contract']['runtime_inputs']
        self.assertEqual(inputs[-len(data['runtime_inputs']):], data['runtime_inputs'])
        self.assertEqual(before['checks']['selection.contract']['runtime_inputs'], inputs[:-len(data['runtime_inputs'])])
        # The whole older chain still reaches its own reviewed bases.
        self.assertEqual(s.digest(batch2.before_policy(policy)), batch2.witness()['base_policy_hash'])
        self.assertEqual(s.digest(groundhog.before_policy(policy)), groundhog.witness()['base_policy_hash'])
        self.assertEqual(s.digest(oldest.before_policy(policy)), oldest.witness()['base_policy_hash'])

    def test_partial_or_duplicate_enrollment_refuses(self):
        data, policy = wave.witness(), self.policy()
        inputs = lambda p: p['checks']['selection.contract']['runtime_inputs']
        for path in data['runtime_inputs']:
            partial = copy.deepcopy(policy)
            inputs(partial).remove(path)
            with self.subTest(partial=path), self.assertRaisesRegex(ValueError, 'partially stripped'):
                wave.before_policy(partial)
            duplicate = copy.deepcopy(policy)
            inputs(duplicate).append(path)
            with self.subTest(duplicate=path), self.assertRaisesRegex(ValueError, 'missing/duplicated: ' + path):
                wave.before_policy(duplicate)

    def test_enrolled_inputs_exist_under_selection_contract_ownership(self):
        policy = self.policy()
        for path in wave.witness()['runtime_inputs']:
            relative = path.removeprefix('{source}/')
            with self.subTest(path=path):
                self.assertTrue((ROOT / relative).is_file())
                classified = s.classify(policy, relative)
                self.assertEqual((classified['effect'], classified['nodes'], classified['unclassified']),
                                 ('global', ['selection.contract'], False))


if __name__ == '__main__':
    unittest.main()
