"""Exact post-419 integration inverse; not execution, hosted, signer or W11 acceptance."""
import copy
import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock
from selector_support import ROOT, s
import w11_post_419_integration_support as wave
import w11_post_1f7_integration_support as post_1f7
import w11_post_h_integration_support as post_h

H3 = '4198376a6a3ab6c8df364b3ccce1cf485bb2dd18'
PATH = 'tests/selection/test_namespace_policy_contract.py'
BEFORE, AFTER, EDITS = ('2be108049190ed6314cd32d5f4d06c6219b4bd1106d35dbb1c94894e67fdcee8',
                        '621faca956b342d680ac152ae24b4598a8fac2baf9382d354a2699c69ac4958b', 1)


class PostFourOneNineIntegration(unittest.TestCase):
    def policy(self):
        return s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_every_frozen_json_witness_is_byte_identical_to_h3(self):
        names = subprocess.run(['git', '-C', str(ROOT), 'ls-tree', '-r', '--name-only', H3, 'tests/selection/fixtures'],
                               capture_output=True, check=True, text=True).stdout.split()
        frozen = [n for n in names if n.endswith('.json')]
        self.assertGreaterEqual(len(frozen), 50)
        self.assertNotIn(wave.WITNESS, frozen)
        for name in frozen:
            with self.subTest(name=name):
                pushed = subprocess.run(['git', '-C', str(ROOT), 'show', f'{H3}:{name}'], capture_output=True, check=True).stdout
                self.assertEqual((ROOT / name).read_bytes(), pushed)

    def test_single_record_is_an_exact_lossless_handoff_to_the_post_h_record(self):
        data = wave.witness()
        self.assertEqual(set(data['sources']), {PATH})
        record, old = data['sources'][PATH], post_h.witness()['sources'][PATH]
        raw = (ROOT / PATH).read_bytes()
        self.assertEqual((record['before_sha256'], record['after_sha256'], len(record['edits'])), (BEFORE, AFTER, EDITS))
        self.assertEqual(hashlib.sha256(raw).hexdigest(), AFTER)
        before = wave.before_source(PATH, raw)
        self.assertEqual(hashlib.sha256(before).hexdigest(), BEFORE)
        self.assertEqual(wave.before_source(PATH, before), before)
        pushed = subprocess.run(['git', '-C', str(ROOT), 'show', f'{H3}:{PATH}'], capture_output=True, check=True).stdout
        self.assertEqual(before, pushed)
        self.assertEqual(BEFORE, old['after_sha256'])
        self.assertNotIn(PATH, post_1f7.witness()['sources'])
        self.assertEqual(record['restored_sha256'], [old['before_sha256'], *old.get('restored_sha256', [])])
        for restored in record['restored_sha256']:
            self.assertNotIn(restored, (BEFORE, AFTER))
        self.assertEqual(hashlib.sha256(post_h.before_source(PATH, before)).hexdigest(), old['before_sha256'])
        self.assertIn('c2320daf019cf8373538df77b76900940f3af4a0', data['commits']['nostrc-0cfsg.36.26'])
        self.assertEqual(data['base'], H3)
        self.assertNotIn('tests/selection/static_frontend_host.py', data['sources'])  # unpinned: no fake record

    def test_earliest_hook_routes_every_chain_through_this_layer(self):
        import post1640_integration_support as post1640
        raw = (ROOT / PATH).read_bytes()
        self.assertEqual(hashlib.sha256(post_1f7.before_source(PATH, raw)).hexdigest(), BEFORE)
        self.assertEqual(hashlib.sha256(post_h.before_source(PATH, raw)).hexdigest(),
                         post_h.witness()['sources'][PATH]['before_sha256'])
        previous = post1640.before_source(PATH, raw)
        self.assertEqual(hashlib.sha256(previous).hexdigest(), post1640.witness()['sources'][PATH]['before_sha256'])
        self.assertEqual(post1640.before_source(PATH, previous), previous)  # post-1640 restored state passes

    def test_unknown_truncated_or_flipped_bytes_refuse(self):
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
                for layer in (wave, post_1f7, post_h):
                    with self.assertRaisesRegex(ValueError, 'post-419 integration source changed: ' + PATH):
                        layer.before_source(PATH, candidate)

    def test_tampered_malformed_or_missing_records_and_witness_refuse(self):
        original = wave.witness()
        raw = (ROOT / PATH).read_bytes()
        tampers = {
            'changed before text': (lambda r: r['edits'][0].__setitem__('before', r['edits'][0]['before'] + '#'),
                                    'post-419 historical source changed'),
            'dropped edit': (lambda r: r['edits'].pop(), 'malformed post-419 integration record'),
            'duplicated edit': (lambda r: r['edits'].append(copy.deepcopy(r['edits'][0])), 'ambiguous post-419 integration inverse'),
            'ambiguous after text': (lambda r: r['edits'][0].__setitem__('after', '\n'), 'ambiguous post-419 integration inverse'),
            'changed after hash': (lambda r: r.__setitem__('after_sha256', '0' * 64), 'post-419 integration source changed'),
            'missing restored field': (lambda r: r.pop('restored_sha256'), 'malformed post-419 integration record'),
            'extra wildcard field': (lambda r: r.__setitem__('pattern', '*'), 'malformed post-419 integration record'),
            'edit not an object': (lambda r: r['edits'].__setitem__(0, ['a', 'b']), 'malformed post-419 integration edit'),
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
            with self.assertRaisesRegex(ValueError, 'post-H integration source changed'):
                post_h.before_source(PATH, raw)
        with tempfile.TemporaryDirectory() as directory:
            copied = Path(directory) / 'witness.json'
            copied.write_bytes((ROOT / wave.WITNESS).read_bytes().replace(b'"edits"', b'"edits" ', 1))
            with mock.patch.object(wave, 'WITNESS', str(copied)):
                with self.assertRaisesRegex(ValueError, 'post-419 integration witness changed'):
                    wave.before_source(PATH, raw)

    def test_exact_policy_inverse_reaches_h3_and_older_bases(self):
        import w11_batch2_integration_support as batch2
        import groundhog_app_enrollment_support as groundhog
        import post1640_integration_support as oldest
        policy, data = self.policy(), wave.witness()
        self.assertEqual(s.digest(policy), data['enrolled_policy_hash'])
        before = wave.before_policy(policy)
        self.assertEqual(s.digest(before), data['base_policy_hash'])
        self.assertEqual(data['base_policy_hash'], post_1f7.witness()['enrolled_policy_hash'])
        self.assertEqual(wave.before_policy(before), before)
        self.assertEqual({f for f in policy if policy[f] != before[f]}, {'checks'})
        self.assertEqual({i for i in policy['checks'] if policy['checks'][i] != before['checks'][i]}, {'selection.contract'})
        self.assertEqual(policy['checks']['selection.contract']['runtime_inputs'][-3:], data['runtime_inputs'])
        self.assertEqual(s.digest(post_1f7.before_policy(policy)), post_1f7.witness()['base_policy_hash'])
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
            for layer in (wave, post_1f7):
                with self.subTest(partial=path, via=layer.__name__), self.assertRaisesRegex(ValueError, 'post-419 enrollment partially stripped'):
                    layer.before_policy(partial)
            duplicate = copy.deepcopy(policy)
            inputs(duplicate).append(path)
            with self.subTest(duplicate=path), self.assertRaisesRegex(ValueError, 'missing/duplicated: ' + path):
                wave.before_policy(duplicate)
            relative = path.removeprefix('{source}/')
            classified = s.classify(policy, relative)
            self.assertTrue((ROOT / relative).is_file(), path)
            self.assertEqual((classified['effect'], classified['nodes'], classified['unclassified']),
                             ('global', ['selection.contract'], False))


if __name__ == '__main__':
    unittest.main()
