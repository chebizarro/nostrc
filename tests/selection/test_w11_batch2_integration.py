"""Exact W11 batch2 integration inverse; not execution, hosted, signer or W11 acceptance."""
import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock
from selector_support import ROOT, s
import w11_batch2_integration_support as wave

GAP = 'core_macos_asan_nip5f_tcp_unconfigured'
JOB = '.github/workflows/dependency-worker.yml:execute'
# Fixed composed set (a missing record cannot pass vacuously): path -> (before, after, edits).
RECORDS = {
    'scripts/test-gate.sh': ('2bcdb1fbd755ef7561453d7850a30348bed283248f9a864cd56cc60e15d24554',
                             '70b8b3773b71c19e06fcfd2627248cfc8dbcbbace3925fb25760877825a72449', 1),
    'tests/selection/test_normal_execution_contract.py': (
        '48b32386fe3fb4fe6c561319c8b7a46ac61f18644746b63731130b9c068c8aba',
        '807b93574b9b3e74376c976b79a461d6391f93822b24c95d1b8252501fc02054', 1),
    'scripts/test_selection_services.py': ('1f54ff13b8471758a635527cb639d46664d665f527fada543257c1bcb7c811c7',
                                           '4d234d8b45c8e2bf61d4e28b07fa73fd35056c9f88d18ed11a748185bac4febe', 17),
    'scripts/test_selection_execution.py': ('9452ba91778426ee6e06c79f3c54f5cddf1fdc3586ce3cec95533d8514f6c400',
                                            '16f8c1357e8772785ee717fcf972a6117d2ea30a0f6c7a134ef03fee978668bd', 9),
    'scripts/test_selection_realization.py': ('6b9da1ba7843c4589d1aad7a4cdc37a2dd38234ff88b12a03f87869033cf5099',
                                              '363f5013f9d9fa201ef236264e11cd4553d1444da42024d3c92560b1d6839d81', 4),
    'nips/nip5f/CMakeLists.txt': ('5b73b22a6357188a482762aeaa0f1930dbb6afe0a4abc49f67906189a8593c80',
                                  'd85dfaa2b4a907b02a2f16bdef81e051dbf00ffb8684ce3bbdf2c92d03e6228e', 1),
    'nips/nip5f/tests/test_nip5f_tcp.c': ('618c01fdb6d46b0eae8902f1cf543472cac0e5691cbef4945502210a476acbe0',
                                          '0686a0c8b1e424f146b08f1a5d5a0588156098067571bd4bd3e545198515242e', 3),
    'apps/gnostr/CMakeLists.txt': ('51f7faeccb78182a5aa159d833e557beb70711c44dd8d167e75f953486cbfcd7',
                                   '6a812f72b33c7451438645a77df63ea67ce2eeefbcdc94b068e650a95cf1020d', 9),
    # The .36.18 additive publication gate (worker-prerequisite layer pins its before bytes).
    '.github/workflows/dependency-worker.yml': (
        '16a92e574e6fc03ca55591c017e911d0e418cfd20431eb501fc736a6715fc1d9',
        'be06df3608b0e5a6eb1372650daba2403a205c981c5ace28af1f3f6379621b1f', 5),
    # This integration's projection of the byte-pinned historical post-1640 test.
    'tests/selection/test_post1640_integration.py': (
        '001d95311301f200271d536e1e0f0a82d607125ea5add1602404b3fb01cf4b9d',
        '236dec6d6109ab68c06fb476b15e7d46a8afc5b658790147dd208b33d8312a2e', 7),
    # Byte-pinned worker-prerequisite test projecting the newer publication gate.
    'tests/selection/test_worker_signer_prerequisites.py': (
        'cc07963f1b00094c48c7ee6794c18030c12c73e0bfcc19119f4a3d4da71e51f1',
        '1c5a723112d2ecd647f9cd799764f96d27dfa4f3b93b1a0c5ff30a49c2843cf4', 1),
}
RESTORED = ('scripts/test_selection_execution.py', 'tests/selection/test_normal_execution_contract.py',
            '.github/workflows/dependency-worker.yml')
# sha256(json.dumps(edits, sort_keys=True)) of the original .36.25 handoff records
# (nostrc-0cfsg-36-25-source-inverse.json sha256 26a61685...): reused verbatim.
HANDOFF_EDITS = {
    'scripts/test-gate.sh': 'd581339acd7d5ad63721e204732e2c51eec4b6ebbf4d49b25222bd507a0dae45',
    'tests/selection/test_normal_execution_contract.py':
        'bde0045e0b42453ecd658f459ba4b7e4a705fbe03ba168571e1e75b4632aee77',
}



def current_bytes(path):
    """This layer's after bytes: the newer post-H layer is projected away first."""
    from w11_post_h_integration_support import before_source as before_post_h_source
    return before_post_h_source(path, (ROOT / path).read_bytes())

def older_after(path):
    """The exact bytes each record must hand to its next older strict pin."""
    import closeout_support
    import post1640_integration_support as post1640
    import runtime_integration_support as runtime
    import w11_matrix_integration_support as matrix
    import w11_worker_support as worker
    if path in ('scripts/test_selection_services.py', 'scripts/test_selection_realization.py'):
        return matrix.witness()['sources'][path]['after_sha256']
    if path in ('scripts/test_selection_execution.py', 'tests/selection/test_normal_execution_contract.py'):
        return post1640.witness()['sources'][path]['after_sha256']
    if path == 'scripts/test-gate.sh':
        return runtime.witness()['sources'][path]['after_sha256']
    if path == 'nips/nip5f/CMakeLists.txt':
        return worker.witness()['source_sha256'][path]
    if path in ('tests/selection/test_post1640_integration.py', '.github/workflows/dependency-worker.yml',
                'tests/selection/test_worker_signer_prerequisites.py'):
        import worker_prerequisite_support as prerequisite
        return prerequisite.witness()['sources'][path]['after_sha256']
    if path == 'apps/gnostr/CMakeLists.txt':
        return closeout_support.core_source_witness()['source_edits'][path]['after_sha256']
    return wave.witness()['maps']['coverage_gaps'][GAP]['before']['binding']['source_sha256']


class W11Batch2Integration(unittest.TestCase):
    def policy(self):
        # Exactly this layer's view: the newer post-H layer is projected away first.
        from w11_post_h_integration_support import before_policy as before_newer_policy
        return before_newer_policy(s.load_inventory(ROOT / 'tests/selection/inventory.json'))

    def test_source_records_are_exact_lossless_handoffs_to_older_pins(self):
        data = wave.witness()
        self.assertEqual(set(data['sources']), set(RECORDS))
        for path, (before_sha, after_sha, count) in RECORDS.items():
            record = data['sources'][path]
            with self.subTest(path=path):
                raw = current_bytes(path)
                self.assertEqual((record['before_sha256'], record['after_sha256'], len(record['edits'])),
                                 (before_sha, after_sha, count))
                self.assertEqual(hashlib.sha256(raw).hexdigest(), after_sha)
                for edit in record['edits']:
                    self.assertEqual(raw.count(edit['after'].encode()), 1)
                before = wave.before_source(path, raw)
                self.assertEqual(hashlib.sha256(before).hexdigest(), before_sha)
                self.assertEqual(wave.before_source(path, before), before)
                self.assertEqual(before_sha, older_after(path))
        for path, digest in HANDOFF_EDITS.items():
            edits = data['sources'][path]['edits']
            self.assertEqual(hashlib.sha256(json.dumps(edits, sort_keys=True).encode()).hexdigest(), digest)
        # Only post-1640's exact restored bytes pass; that layer then returns them unchanged.
        import post1640_integration_support as post1640
        self.assertEqual({p for p, r in data['sources'].items() if 'restored_sha256' in r}, set(RESTORED))
        for path in RESTORED:
            restored = data['sources'][path]['restored_sha256']
            self.assertEqual(restored, [post1640.witness()['sources'][path]['before_sha256']])
            self.assertNotIn(restored[0], (RECORDS[path][0], RECORDS[path][1]))
            previous = post1640.before_source(path, current_bytes(path))
            self.assertEqual(hashlib.sha256(previous).hexdigest(), restored[0])
            self.assertEqual(wave.before_source(path, previous), previous)
            self.assertEqual(post1640.before_source(path, previous), previous)
            with self.assertRaisesRegex(ValueError, 'W11 batch2 integration source changed'):
                wave.before_source(path, previous + b'\n')
        services = data['sources']['scripts/test_selection_services.py']['authors']
        self.assertEqual(services, ['nostrc-0cfsg.36.25', 'nostrc-0cfsg.36.18'])

    def test_whole_chains_reach_oldest_witnesses_from_current_bytes(self):
        import runtime_integration_support as runtime
        import post1640_integration_support as post1640
        import coreasan_app_enrollment_support as coreasan
        from closeout_support import before_core_producer_source, core_source_witness
        from homed_profile_support import before_homed_source
        from w11_policy_support import before_policy_source
        for path in ('scripts/test-gate.sh', 'scripts/test_selection_services.py',
                     'scripts/test_selection_realization.py'):
            raw = current_bytes(path)
            self.assertEqual(hashlib.sha256(runtime.before_runtime_source(path, raw)).hexdigest(),
                             runtime.witness()['sources'][path]['before_sha256'], path)
        for path in ('scripts/test_selection_execution.py', 'tests/selection/test_normal_execution_contract.py'):
            raw = current_bytes(path)
            self.assertEqual(hashlib.sha256(post1640.before_source(path, raw)).hexdigest(),
                             post1640.witness()['sources'][path]['before_sha256'], path)
        # The coreasan hand-off routes the newest layer into the W11 matrix leaf.
        path = 'scripts/test_selection_realization.py'
        import w11_matrix_integration_support as matrix
        self.assertEqual(hashlib.sha256(coreasan.before_source(path, current_bytes(path))).hexdigest(),
                         matrix.witness()['sources'][path]['before_sha256'])
        path = 'apps/gnostr/CMakeLists.txt'
        record = core_source_witness()['source_edits'][path]
        previous = before_core_producer_source(path, current_bytes(path))
        self.assertEqual(hashlib.sha256(previous).hexdigest(), record['before_sha256'])
        self.assertEqual(before_core_producer_source(path, previous), previous)
        path = 'nips/nip5f/CMakeLists.txt'
        raw = before_policy_source(path, before_homed_source(path, current_bytes(path)))
        self.assertEqual(hashlib.sha256(raw).hexdigest(), RECORDS[path][0])

    def test_tampered_or_unknown_bytes_refuse_for_every_record(self):
        import runtime_integration_support as runtime
        from closeout_support import before_core_producer_source
        for path in RECORDS:
            record = wave.witness()['sources'][path]
            raw = current_bytes(path)
            before = wave.before_source(path, raw)
            drifted = [raw + b'\n# unknown bytes\n', before + b'\n# unknown bytes\n', raw[:-1]]
            for edit in record['edits']:
                start = raw.index(edit['after'].encode())
                middle = start + len(edit['after'].encode()) // 2
                drifted.append(raw[:middle] + bytes([raw[middle] ^ 0x01]) + raw[middle + 1:])
            for index, candidate in enumerate(drifted):
                with self.subTest(path=path, case=index):
                    with self.assertRaisesRegex(ValueError, 'W11 batch2 integration source changed: ' + path):
                        wave.before_source(path, candidate)
                    with self.assertRaises(ValueError):
                        runtime.before_runtime_source(path, candidate)
                    with self.assertRaises(ValueError):
                        before_core_producer_source(path, candidate)

    def test_tampered_witness_records_refuse(self):
        original = wave.witness()
        path = 'scripts/test_selection_services.py'
        raw = current_bytes(path)
        tampers = {
            'changed before text': (lambda r: r['edits'][0].__setitem__('before', r['edits'][0]['before'] + '#'),
                                    'W11 batch2 historical source changed'),
            'dropped edit': (lambda r: r['edits'].pop(), 'W11 batch2 historical source changed'),
            'duplicated edit': (lambda r: r['edits'].append(copy.deepcopy(r['edits'][0])),
                                'ambiguous W11 batch2 integration inverse'),
            'ambiguous after text': (lambda r: r['edits'][0].__setitem__('after', '\n'),
                                     'ambiguous W11 batch2 integration inverse'),
            'changed after hash': (lambda r: r.__setitem__('after_sha256', '0' * 64),
                                   'W11 batch2 integration source changed'),
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
                with self.assertRaisesRegex(ValueError, 'W11 batch2 integration witness changed'):
                    wave.before_source(path, raw)

    def test_exact_policy_inverse_reaches_groundhog_enrollment_and_is_idempotent(self):
        import groundhog_app_enrollment_support as groundhog
        import post1640_integration_support as oldest
        policy, data = self.policy(), wave.witness()
        self.assertEqual(s.digest(policy), data['enrolled_policy_hash'])
        before = wave.before_policy(policy)
        self.assertEqual(s.digest(before), data['base_policy_hash'])
        self.assertEqual(data['base_policy_hash'], groundhog.witness()['enrolled_policy_hash'])
        self.assertEqual(wave.before_policy(before), before)
        self.assertEqual({f for f in policy if policy[f] != before[f]}, {'checks', 'coverage_gaps', 'workflow_jobs'})
        self.assertEqual({i for i in policy['workflow_jobs'] if policy['workflow_jobs'][i] != before['workflow_jobs'][i]},
                         {JOB})
        self.assertEqual({i for i in policy['checks'] if policy['checks'][i] != before['checks'][i]},
                         {'selection.contract'})
        inputs = policy['checks']['selection.contract']['runtime_inputs']
        self.assertEqual(inputs[-len(data['runtime_inputs']):], data['runtime_inputs'])
        self.assertEqual(before['checks']['selection.contract']['runtime_inputs'],
                         inputs[:-len(data['runtime_inputs'])])
        self.assertEqual(s.digest(groundhog.before_policy(policy)), groundhog.witness()['base_policy_hash'])
        self.assertEqual(s.digest(oldest.before_policy(policy)), oldest.witness()['base_policy_hash'])
        # Frozen authority and historical review bindings are not rewritten.
        for field in ('diagnostic_nonmembers', 'diagnostic_nonmember_subsets', 'path_rules', 'profiles', 'nodes'):
            self.assertEqual(policy[field], before[field], field)
        for profile in ('grotto.linux', 'groundhog.linux'):
            review = policy['diagnostic_nonmember_subsets'][profile]['review']['source_inputs']
            for path in ('nips/nip5f/CMakeLists.txt', 'apps/gnostr/CMakeLists.txt'):
                self.assertEqual(review[path], RECORDS[path][0], (profile, path))

    def test_anchor_rebinding_is_exactly_the_changed_nip5f_source_and_registration(self):
        policy, data = self.policy(), wave.witness()
        record = data['maps']['coverage_gaps'][GAP]
        self.assertEqual(set(data['maps']), {'coverage_gaps', 'workflow_jobs'})
        self.assertEqual(set(data['maps']['coverage_gaps']), {GAP})
        live = [gap for gap in policy['coverage_gaps'] if gap['id'] == GAP]
        self.assertEqual(live, [record['after']])
        after, before = copy.deepcopy(record['after']), copy.deepcopy(record['before'])
        self.assertEqual(after.pop('binding').keys(), before.pop('binding').keys())
        self.assertEqual(after, before)
        changed = {k for k in record['after']['binding'] if record['after']['binding'][k] != record['before']['binding'][k]}
        self.assertEqual(changed, {'source_sha256', 'registration_sha256'})
        for key, path in (('source_sha256', 'nips/nip5f/tests/test_nip5f_tcp.c'),
                          ('registration_sha256', 'nips/nip5f/CMakeLists.txt')):
            self.assertEqual(record['after']['binding'][key], hashlib.sha256(current_bytes(path)).hexdigest())
            self.assertEqual(record['after']['binding'][key], RECORDS[path][1])
            self.assertEqual(record['before']['binding'][key], RECORDS[path][0])

    def test_missing_partial_duplicate_tampered_and_mixed_enrollment_refuse(self):
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
        index = next(i for i, gap in enumerate(policy['coverage_gaps']) if gap['id'] == GAP)
        tampered = copy.deepcopy(policy)
        tampered['coverage_gaps'][index]['binding']['selection_metadata_sha256'] = '0' * 64
        restored = copy.deepcopy(policy)
        restored['coverage_gaps'][index] = copy.deepcopy(data['maps']['coverage_gaps'][GAP]['before'])
        duplicated = copy.deepcopy(policy)
        duplicated['coverage_gaps'].append(copy.deepcopy(policy['coverage_gaps'][index]))
        missing = copy.deepcopy(policy)
        del missing['coverage_gaps'][index]
        for name, candidate in (('tampered', tampered), ('mixed restored', restored),
                                ('duplicated gap', duplicated), ('missing gap', missing)):
            with self.subTest(case=name), self.assertRaisesRegex(ValueError, 'W11 batch2 coverage gap changed'):
                wave.before_policy(candidate)
        unenrolled = copy.deepcopy(policy)
        for path in data['runtime_inputs']:
            inputs(unenrolled).remove(path)
        with self.assertRaisesRegex(ValueError, 'rebinding without enrollment'):
            wave.before_policy(unenrolled)
        # An older view without this enrollment or rebinding passes unchanged.
        old = wave.before_policy(policy)
        self.assertEqual(wave.before_policy(old), old)

    def test_execute_job_map_is_exact_live_hashes_to_worker_layer_after(self):
        from profile_contract_support import source_job_blocks, validate_job_accounting
        import worker_prerequisite_support as prerequisite
        policy, data = self.policy(), wave.witness()
        record = data['maps']['workflow_jobs'][JOB]
        # Live accounting holds for the live inventory; this layer's records are
        # compared after the newer post-H job map is projected away.
        validate_job_accounting(s.load_inventory(ROOT / 'tests/selection/inventory.json'), ROOT)
        from w11_post_h_integration_support import before_jobs as before_post_h_jobs
        live = before_post_h_jobs(source_job_blocks(ROOT))
        fields = ('source_sha256', 'workflow_sha256', 'matrix_source')
        self.assertEqual({k: live[JOB][k] for k in fields}, {k: record['after'][k] for k in fields})
        self.assertEqual(policy['workflow_jobs'][JOB], record['after'])
        self.assertEqual(record['before'], prerequisite.witness()['maps']['workflow_jobs'][JOB]['after'])
        self.assertEqual({k for k in record['after'] if record['after'][k] != record['before'][k]},
                         {'source_sha256', 'workflow_sha256'})
        self.assertEqual(record['after']['workflow_sha256'], RECORDS['.github/workflows/dependency-worker.yml'][1])
        projected = wave.before_jobs(live)
        self.assertEqual({k: projected[JOB][k] for k in fields}, {k: record['before'][k] for k in fields})
        self.assertEqual(wave.before_jobs(projected), projected)
        # Through the whole jobs chain to the worker layer's own reviewed base.
        chained = prerequisite.before_jobs(live)
        expected = prerequisite.witness()['maps']['workflow_jobs'][JOB]['before']
        self.assertEqual({k: chained[JOB][k] for k in fields}, {k: expected[k] for k in fields})
        bad = copy.deepcopy(live)
        bad[JOB]['source_sha256'] = '0' * 64
        self.assertEqual(wave.before_jobs(bad)[JOB], bad[JOB])  # unknown hashes are never projected
        tampered = copy.deepcopy(policy)
        tampered['workflow_jobs'][JOB]['source_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'W11 batch2 workflow job changed'):
            wave.before_policy(tampered)
        unenrolled = copy.deepcopy(wave.before_policy(policy))
        unenrolled['workflow_jobs'][JOB] = copy.deepcopy(record['after'])
        with self.assertRaisesRegex(ValueError, 'workflow job hashes without enrollment'):
            wave.before_policy(unenrolled)

    def test_enrolled_inputs_exist_and_keep_their_existing_ownership(self):
        policy = self.policy()
        for path in wave.witness()['runtime_inputs']:
            relative = path.removeprefix('{source}/')
            with self.subTest(path=path):
                self.assertTrue((ROOT / relative).is_file())
                classified = s.classify(policy, relative)
                self.assertFalse(classified['unclassified'])
                self.assertEqual(classified['effect'], 'global')
                expected = ['build.global'] if relative.startswith('scripts/') else ['selection.contract']
                self.assertEqual(classified['nodes'], expected)


if __name__ == '__main__':
    unittest.main()
