"""Pure contract/failure tests. Positive lifecycle evidence is tests/custody."""
import copy
import importlib.util
import subprocess
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from test_selection_custody import ContainerOwner, CustodyError, context_hash, immutable


class ContainedCustodyContract(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.owner = ContainerOwner(self.root / 'owner')

    def test_snapshot_requires_and_verifies_exact_gitlink_object_sources(self):
        spec = importlib.util.spec_from_file_location('contained_gate_contract',
            Path(__file__).resolve().parents[2] / 'scripts/contained-gate.py')
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        def repository(name):
            path = self.root / name
            path.mkdir()
            subprocess.run(['git', 'init', '-q', str(path)], check=True)
            subprocess.run(['git', '-C', str(path), 'config', 'user.name', 'fixture'], check=True)
            subprocess.run(['git', '-C', str(path), 'config', 'user.email', 'fixture@example.invalid'], check=True)
            (path / 'file').write_text(name)
            subprocess.run(['git', '-C', str(path), 'add', '.'], check=True)
            subprocess.run(['git', '-c', 'core.hooksPath=/dev/null', '-C', str(path), 'commit', '-qm', 'fixture'], check=True)
            return path, subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD']).decode().strip()
        source, _ = repository('source')
        child, child_sha = repository('child')
        subprocess.run(['git', '-C', str(source), 'update-index', '--add', '--cacheinfo',
                        '160000,' + child_sha + ',docs/child'], check=True)
        subprocess.run(['git', '-c', 'core.hooksPath=/dev/null', '-C', str(source), 'commit', '-qm', 'gitlink'], check=True)
        sha = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD']).decode().strip()
        with self.assertRaises(CustodyError):
            gate.snapshot(source, self.root / 'missing', sha)
        links = gate.snapshot(source, self.root / 'exact', sha, {'docs/child': child})
        self.assertEqual(links, {'docs/child': child_sha})
        self.assertEqual((self.root / 'exact/docs/child/file').read_text(), 'child')
        self.assertEqual(subprocess.check_output(['git', '-C', str(self.root / 'exact/docs/child'), 'rev-parse', 'HEAD']).decode().strip(), child_sha)

    def test_local_only_profile_is_accounted_unexecuted_in_ci_and_release(self):
        import test_selection as selection
        policy = selection.load_inventory(Path(__file__).resolve().parents[2] / 'tests/selection/inventory.json')
        profile = 'groundhog.linux.arm64.prepared011'
        for context in ('ci', 'release'):
            plan = selection.make_plan(policy, [], head='a' * 40, context=context, mode='broad')
            self.assertNotIn(profile, plan['expected_lane_ids'])
            self.assertEqual(len(plan['expected_lane_ids']), 10)
            self.assertEqual(plan['omitted_profiles'][0]['id'], profile)
            self.assertEqual(plan['omitted_profiles'][0]['status'], 'not_executed')
            selection.validate_plan(plan, policy)
            with self.assertRaises(selection.ContractError):
                selection.make_plan(policy, [], head='a' * 40, context=context, mode='broad', profiles=[profile])

    def test_legacy_contextless_plan_only_qualifies_for_unrestricted_policy(self):
        import test_selection as selection
        policy = {'profiles': {'ordinary': {}}, 'checks': {'ordinary': {'profiles': ['ordinary']}}}
        plan = {'schema_version': 1, 'head': 'a' * 40, 'tested_sha': 'a' * 40,
                'source_identity': {'kind': 'committed', 'sha': 'a' * 40},
                'inventory_hash': selection.digest(policy), 'selected_checks': ['ordinary'],
                'expected_lane_ids': ['ordinary'], 'lanes': [{'id': 'ordinary', 'profile': 'ordinary',
                    'profile_hash': selection.digest({}), 'checks': ['ordinary']}]}
        plan['plan_id'] = selection.digest(plan)
        selection.validate_plan(plan, policy)
        policy['profiles']['ordinary']['contexts'] = ['local']
        plan['inventory_hash'] = selection.digest(policy)
        plan['lanes'][0]['profile_hash'] = selection.digest(policy['profiles']['ordinary'])
        plan['plan_id'] = selection.digest({k: v for k, v in plan.items() if k != 'plan_id'})
        with self.assertRaisesRegex(selection.ContractError, 'invalid plan context'):
            selection.validate_plan(plan, policy)

    def test_local_context_cannot_be_relabelled_to_ci(self):
        import test_selection as selection
        policy = selection.load_inventory(Path(__file__).resolve().parents[2] / 'tests/selection/inventory.json')
        profile = 'groundhog.linux.arm64.prepared011'
        plan = selection.make_plan(policy, [], head='a' * 40, context='local', mode='broad', profiles=[profile])
        self.assertEqual(plan['expected_lane_ids'], [profile])
        plan['context'] = 'ci'
        plan['plan_id'] = selection.digest({k: v for k, v in plan.items() if k != 'plan_id'})
        with self.assertRaises(selection.ContractError):
            selection.validate_plan(plan, policy)

    def test_runtime_json_and_artifact_paths_cannot_follow_host_symlinks(self):
        spec = importlib.util.spec_from_file_location('contained_gate_paths',
            Path(__file__).resolve().parents[2] / 'scripts/contained-gate.py')
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        work = self.root / 'work'
        work.mkdir()
        outside = self.root / 'host.json'
        outside.write_text('{"secret":"must not read"}')
        (work / 'backend.json').symlink_to(outside)
        with self.assertRaises(CustodyError):
            gate.work_json(work, 'backend.json')
        with self.assertRaises(CustodyError):
            gate.work_json(work, '../host.json')

    def test_prepared_peer_capability_without_contained_domain_cannot_launch(self):
        import test_selection_realization as realization
        resolved = {'source_identity': {}, 'profile_fingerprint': 'bound', 'required_capabilities': ['prepared_peer']}
        metadata = {'profile_fingerprint': 'bound', 'profile': {}}
        with patch.object(realization, 'load_realization', return_value=metadata), \
                patch.object(realization, '_validate_resolved_phase'), \
                patch.object(realization, '_attempt_directory', side_effect=AssertionError('must not launch')), \
                self.assertRaisesRegex(realization.RealizationError, 'guarded adapter'):
            realization._run_resolved(resolved, self.root, self.root / 'evidence')

    def test_audit_rejects_a_mapping_in_place_of_live_namespace_context(self):
        import test_selection_realization as realization
        with patch.object(realization, '_validate_phase_context'), self.assertRaisesRegex(
                realization.RealizationError, 'live contained context'):
            realization.check_inputs({}, {}, custody={'peers': {'011': {}}})

    def test_peer_audit_cannot_reuse_a_different_profile(self):
        from test_selection_custody import PreparedContext
        context = object.__new__(PreparedContext)
        context.control = {'profile': {'execution_domain': 'prepared_peer_linux_v1'}}
        with self.assertRaisesRegex(CustodyError, 'audit profile differs'):
            context.audit_inputs({'profile': {}})

    def test_final_retained_bytes_membership_and_original_claims_are_not_refreshed(self):
        from test_selection_custody import retained_files, attest_retained_evidence, digest, file_hash
        (self.root / 'build').mkdir()
        (self.root / 'execution').mkdir()
        artifact = self.root / 'build/proof.txt'
        log = self.root / 'execution/command.log'
        artifact.write_bytes(b'original')
        log.write_bytes(b'raw\xff\n')
        result = {'checks': {'actual': {'log': '/work/execution/command.log', 'log_sha256': file_hash(log),
            'artifacts': {'proof': {'path': '/work/build/proof.txt', 'sha256': file_hash(artifact)}}}}}
        sealed = retained_files(self.root)
        state = {'retained_evidence': sealed, 'retained_evidence_hash': digest(sealed)}
        artifact.unlink()
        log.write_bytes(b'tampered\xff\n')
        (self.root / 'build/late').write_bytes(b'late')
        final = attest_retained_evidence(self.root, result, state)
        self.assertEqual(final['status'], 'failed')
        self.assertEqual({entry['reason'] for entry in final['errors']}, {'missing', 'changed', 'added'})
        self.assertEqual(log.read_bytes(), b'tampered\xff\n')
        # Even a recaptured seal cannot replace the receipt's original hashes.
        sealed = retained_files(self.root)
        state = {'retained_evidence': sealed, 'retained_evidence_hash': digest(sealed)}
        final = attest_retained_evidence(self.root, result, state)
        self.assertEqual(final['status'], 'failed')
        self.assertTrue(any('original receipt hash' in e['reason'] for e in final['errors']))

    def test_retained_links_cannot_hide_outside_bytes_or_special_files(self):
        from test_selection_custody import retained_files
        build = self.root / 'build'
        build.mkdir()
        outside = self.root / 'not-retained'
        outside.write_bytes(b'not sealed')
        link = build / 'link'
        link.symlink_to(outside)
        with self.assertRaises(CustodyError):
            retained_files(self.root)
        link.unlink()
        os.mkfifo(build / 'fifo')
        with self.assertRaises(CustodyError):
            retained_files(self.root)

    def test_tag_and_partial_id_never_authorize_image(self):
        for value in ('latest', 'image:tag', 'sha256:123', 'a' * 64):
            with self.subTest(value=value), self.assertRaises(CustodyError):
                immutable(value)

    def test_context_rejects_symlinks(self):
        (self.root / 'link').symlink_to('/tmp')
        with self.assertRaises(CustodyError):
            context_hash(self.root)

    def test_context_binds_contents_and_modes(self):
        source = self.root / 'source'
        source.mkdir()
        path = source / 'file'
        path.write_bytes(b'original')
        before = context_hash(source)
        path.write_bytes(b'changed')
        self.assertNotEqual(before, context_hash(source))
        before = context_hash(source)
        path.chmod(0o755)
        self.assertNotEqual(before, context_hash(source))

    def test_root_operator_is_not_silently_container_root(self):
        with patch('os.getuid', return_value=0), self.assertRaises(CustodyError):
            self.owner.create('sha256:' + 'a' * 64, ['/bin/true'])

    def test_host_network_and_pid_authority_not_exposed(self):
        for network in ('host', 'container:other', 'custom'):
            with self.subTest(network=network), self.assertRaises(CustodyError):
                self.owner.create('sha256:' + 'a' * 64, ['/bin/true'], network=network)

    def test_rpc_error_cannot_be_later_promoted_by_absence(self):
        self.owner.indeterminate = True
        result = self.owner.finalize()
        self.assertFalse(result['cleanup_complete'])
        self.assertFalse(result['complete'])
        self.assertFalse(result['complete_gate_eligible'])

    def test_binding_is_an_immutable_snapshot_not_a_circular_receipt(self):
        binding = {'profile': {'id': 'example'}}
        with patch.object(self.owner, 'create', side_effect=CustodyError('prelaunch failure')):
            self.owner.run('sha256:' + 'a' * 64, ['/bin/true'], binding=binding)
        binding['receipt'] = self.owner.receipt
        binding['profile']['id'] = 'changed'
        self.assertEqual(self.owner.receipt['binding'], {'profile': {'id': 'example'}})
        json.dumps(binding)

    def test_unknown_creation_identity_is_not_cleaned_by_name(self):
        self.owner.receipt['status'] = 'creating'
        with patch.object(self.owner, 'rpc', side_effect=AssertionError('must not signal guessed identity')):
            result = self.owner.finalize()
        self.assertFalse(result['cleanup_complete'])
        self.assertIn('identity unconfirmed', result['cleanup_errors'][0])

    def test_actual_isolation_mutations_refuse(self):
        owner = self.owner
        owner.receipt.update(image_id='sha256:' + 'a' * 64, network='none', mounts=[], argv=['python3', '-c', 'pass'])
        valid = {'Image': owner.receipt['image_id'], 'Mounts': [],
                 'Config': {'User': f'{os.getuid()}:{os.getgid()}', 'Entrypoint': ['python3'], 'Cmd': ['-c', 'pass']},
                 'NetworkSettings': {'Networks': {'none': {}}},
                 'HostConfig': {'NetworkMode': 'none', 'PidMode': '', 'IpcMode': 'private', 'Privileged': False,
                   'ReadonlyRootfs': True, 'CapDrop': ['ALL'], 'CapAdd': None, 'SecurityOpt': ['no-new-privileges=true'],
                   'Init': True, 'Devices': [], 'PortBindings': {}}}
        mutations = {'NetworkMode': 'host', 'PidMode': 'host', 'IpcMode': 'host', 'Privileged': True,
                     'ReadonlyRootfs': False, 'CapDrop': [], 'CapAdd': ['SYS_ADMIN'], 'SecurityOpt': [],
                     'Init': False, 'Devices': ['/dev/kvm'], 'PortBindings': {'80': []}}
        for key, value in mutations.items():
            changed = copy.deepcopy(valid)
            changed['HostConfig'][key] = value
            with self.subTest(key=key), self.assertRaises(CustodyError):
                owner.validate_isolation(changed)
        for field, value in [('Image', 'wrong'), ('Mounts', [{'Source': '/var/run/docker.sock', 'Destination': '/socket', 'RW': True}])]:
            changed = copy.deepcopy(valid)
            changed[field] = value
            with self.subTest(field=field), self.assertRaises(CustodyError):
                owner.validate_isolation(changed)
        for key, value in [('User', '0:0'), ('Entrypoint', ['/bin/sh']), ('Cmd', ['wrong'])]:
            changed = copy.deepcopy(valid)
            changed['Config'][key] = value
            with self.subTest(key=key), self.assertRaises(CustodyError):
                owner.validate_isolation(changed)


if __name__ == '__main__':
    unittest.main()
