"""Exact post-1f7 integration inverse; not execution, hosted, signer or W11 acceptance."""
import copy
import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock
from selector_support import ROOT, s
import w11_post_1f7_integration_support as wave
import w11_post_h_integration_support as post_h
import w11_batch2_integration_support as batch2

B = '1f7dc22b6e860331d590d49fb91f9687dd269335'
PATH = 'tests/selection/test_normal_execution_contract.py'
BEFORE, AFTER, EDITS = ('807b93574b9b3e74376c976b79a461d6391f93822b24c95d1b8252501fc02054',
                        '4581b59e73386f52b9e2968e6e0577d998a38c4db0e85e406c18c930dddb9c37', 7)


class PostOneF7Integration(unittest.TestCase):
    def policy(self):
        return s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_every_frozen_json_witness_is_byte_identical_to_1f7(self):
        names = subprocess.run(['git', '-C', str(ROOT), 'ls-tree', '-r', '--name-only', B, 'tests/selection/fixtures'],
                               capture_output=True, check=True, text=True).stdout.split()
        frozen = [n for n in names if n.endswith('.json')]
        self.assertGreaterEqual(len(frozen), 49)
        self.assertNotIn(wave.WITNESS, frozen)
        for name in frozen:
            with self.subTest(name=name):
                pushed = subprocess.run(['git', '-C', str(ROOT), 'show', f'{B}:{name}'], capture_output=True, check=True).stdout
                self.assertEqual((ROOT / name).read_bytes(), pushed)

    def test_single_record_is_an_exact_lossless_handoff_to_the_h_era_record(self):
        data = wave.witness()
        self.assertEqual(set(data['sources']), {PATH})
        record, old = data['sources'][PATH], batch2.witness()['sources'][PATH]
        raw = (ROOT / PATH).read_bytes()
        self.assertEqual((record['before_sha256'], record['after_sha256'], len(record['edits'])), (BEFORE, AFTER, EDITS))
        self.assertEqual(hashlib.sha256(raw).hexdigest(), AFTER)
        before = wave.before_source(PATH, raw)
        self.assertEqual(hashlib.sha256(before).hexdigest(), BEFORE)
        self.assertEqual(wave.before_source(PATH, before), before)
        pushed = subprocess.run(['git', '-C', str(ROOT), 'show', f'{B}:{PATH}'], capture_output=True, check=True).stdout
        self.assertEqual(before, pushed)
        self.assertEqual(BEFORE, old['after_sha256'])
        self.assertNotIn(PATH, post_h.witness()['sources'])
        self.assertEqual(record['restored_sha256'], [old['before_sha256'], *old.get('restored_sha256', [])])
        for restored in record['restored_sha256']:
            self.assertNotIn(restored, (BEFORE, AFTER))
        self.assertEqual(hashlib.sha256(batch2.before_source(PATH, before)).hexdigest(), old['before_sha256'])
        self.assertIn('d7058481136be071003ecc989c59a79c75193254', data['commits']['nostrc-0cfsg.36.26'])
        self.assertEqual(data['base'], B)

    def test_earliest_hook_routes_every_chain_through_this_layer(self):
        import post1640_integration_support as post1640
        import runtime_integration_support as runtime
        raw = (ROOT / PATH).read_bytes()
        # post-H's entry projects this layer first; batch2 then reaches its own before.
        self.assertEqual(hashlib.sha256(post_h.before_source(PATH, raw)).hexdigest(), BEFORE)
        previous = post1640.before_source(PATH, raw)
        self.assertEqual(hashlib.sha256(previous).hexdigest(), post1640.witness()['sources'][PATH]['before_sha256'])
        self.assertEqual(post1640.before_source(PATH, previous), previous)  # post-1640 restored state passes
        runtime.before_runtime_source(PATH, raw)  # the runtime chain accepts the exact current bytes

    def test_unknown_truncated_or_flipped_bytes_refuse(self):
        import runtime_integration_support as runtime
        record = wave.witness()['sources'][PATH]
        raw = (ROOT / PATH).read_bytes()
        before = wave.before_source(PATH, raw)
        drifted = [raw + b'\n# unknown bytes\n', before + b'\n# unknown bytes\n', raw[:-1]]
        for edit in record['edits']:
            start = raw.index(edit['after'].encode())
            middle = start + len(edit['after'].encode()) // 2
            drifted.append(raw[:middle] + bytes([raw[middle] ^ 0x01]) + raw[middle + 1:])
        for index, candidate in enumerate(drifted):
            with self.subTest(case=index):
                with self.assertRaisesRegex(ValueError, 'post-1f7 integration source changed: ' + PATH):
                    wave.before_source(PATH, candidate)
                with self.assertRaisesRegex(ValueError, 'post-1f7 integration source changed'):
                    post_h.before_source(PATH, candidate)
                with self.assertRaises(ValueError):
                    runtime.before_runtime_source(PATH, candidate)

    def test_tampered_or_malformed_records_and_witness_refuse(self):
        original = wave.witness()
        raw = (ROOT / PATH).read_bytes()
        tampers = {
            'changed before text': (lambda r: r['edits'][0].__setitem__('before', r['edits'][0]['before'] + '#'),
                                    'post-1f7 historical source changed'),
            'dropped edit': (lambda r: r['edits'].pop(), 'post-1f7 historical source changed'),
            'duplicated edit': (lambda r: r['edits'].append(copy.deepcopy(r['edits'][0])), 'ambiguous post-1f7 integration inverse'),
            'ambiguous after text': (lambda r: r['edits'][0].__setitem__('after', '\n'), 'ambiguous post-1f7 integration inverse'),
            'changed after hash': (lambda r: r.__setitem__('after_sha256', '0' * 64), 'post-1f7 integration source changed'),
            'missing restored field': (lambda r: r.pop('restored_sha256'), 'malformed post-1f7 integration record'),
            'extra wildcard field': (lambda r: r.__setitem__('pattern', '*'), 'malformed post-1f7 integration record'),
            'empty edits': (lambda r: r.__setitem__('edits', []), 'malformed post-1f7 integration record'),
            'edit not an object': (lambda r: r['edits'].__setitem__(0, ['a', 'b']), 'malformed post-1f7 integration edit'),
        }
        for name, (mutate, message) in tampers.items():
            data = copy.deepcopy(original)
            mutate(data['sources'][PATH])
            with self.subTest(tamper=name), mock.patch.object(wave, 'witness', return_value=data):
                with self.assertRaisesRegex(ValueError, message):
                    wave.before_source(PATH, raw)
        missing = copy.deepcopy(original)
        del missing['sources'][PATH]
        with mock.patch.object(wave, 'witness', return_value=missing):
            with self.assertRaisesRegex(ValueError, 'W11 batch2 integration source changed'):
                post_h.before_source(PATH, raw) and batch2.before_source(PATH, post_h.before_source(PATH, raw))
        with tempfile.TemporaryDirectory() as directory:
            copied = Path(directory) / 'witness.json'
            copied.write_bytes((ROOT / wave.WITNESS).read_bytes().replace(b'"edits"', b'"edits" ', 1))
            with mock.patch.object(wave, 'WITNESS', str(copied)):
                with self.assertRaisesRegex(ValueError, 'post-1f7 integration witness changed'):
                    wave.before_source(PATH, raw)

    def test_exact_policy_inverse_reaches_1f7_and_older_bases(self):
        import groundhog_app_enrollment_support as groundhog
        import post1640_integration_support as oldest
        policy, data = self.policy(), wave.witness()
        self.assertEqual(s.digest(policy), data['enrolled_policy_hash'])
        before = wave.before_policy(policy)
        self.assertEqual(s.digest(before), data['base_policy_hash'])
        self.assertEqual(data['base_policy_hash'], post_h.witness()['enrolled_policy_hash'])
        self.assertEqual(wave.before_policy(before), before)
        self.assertEqual({f for f in policy if policy[f] != before[f]}, {'checks'})
        self.assertEqual({i for i in policy['checks'] if policy['checks'][i] != before['checks'][i]}, {'selection.contract'})
        inputs = policy['checks']['selection.contract']['runtime_inputs']
        self.assertEqual(inputs[-3:], data['runtime_inputs'])
        self.assertEqual(s.digest(post_h.before_policy(policy)), post_h.witness()['base_policy_hash'])
        self.assertEqual(s.digest(batch2.before_policy(policy)), batch2.witness()['base_policy_hash'])
        self.assertEqual(s.digest(groundhog.before_policy(policy)), groundhog.witness()['base_policy_hash'])
        self.assertEqual(s.digest(oldest.before_policy(policy)), oldest.witness()['base_policy_hash'])

    def test_missing_partial_or_duplicate_enrollment_refuses(self):
        data, policy = wave.witness(), self.policy()
        inputs = lambda p: p['checks']['selection.contract']['runtime_inputs']
        for path in data['runtime_inputs']:
            partial = copy.deepcopy(policy)
            inputs(partial).remove(path)
            with self.subTest(partial=path), self.assertRaisesRegex(ValueError, 'post-1f7 enrollment partially stripped'):
                wave.before_policy(partial)
            with self.subTest(partial_via_hook=path), self.assertRaisesRegex(ValueError, 'post-1f7 enrollment partially stripped'):
                post_h.before_policy(partial)
            duplicate = copy.deepcopy(policy)
            inputs(duplicate).append(path)
            with self.subTest(duplicate=path), self.assertRaisesRegex(ValueError, 'missing/duplicated: ' + path):
                wave.before_policy(duplicate)
        for path in data['runtime_inputs']:
            relative = path.removeprefix('{source}/')
            classified = s.classify(policy, relative)
            self.assertTrue((ROOT / relative).is_file(), path)
            self.assertEqual((classified['effect'], classified['nodes'], classified['unclassified']),
                             ('global', ['selection.contract'], False))


if __name__ == '__main__':
    unittest.main()
