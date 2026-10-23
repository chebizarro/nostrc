"""Controller/source contracts; these unit cases do not claim live Docker success."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from selector_support import ROOT, s
from profile_contract_support import validate_job_accounting
import w11_policy_support as p

spec = importlib.util.spec_from_file_location('policy_controller', ROOT / 'scripts/test_selection_policy.py')
controller = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controller)


class PolicyControllerTests(unittest.TestCase):
    def test_actual_policy_command_and_domain_are_not_native_predecessor_only(self):
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        check = policy['checks']['selection.contract']
        self.assertEqual(check['command'], ['python3', '{source}/scripts/test_selection_policy.py',
            'attach', '--control', '{build}/namespace-policy/control'])
        self.assertEqual(check['requires'], [controller.CAPABILITY])
        self.assertEqual(policy['profiles']['static.policy']['execution_domain'], 'policy_controller_linux_v1')
        self.assertEqual(policy['profiles']['static.policy']['capabilities'], [controller.CAPABILITY])
        validate_job_accounting(policy, ROOT)
        self.assertFalse(policy['enforcement_ready'])
        with self.assertRaises(s.ContractError):
            modified = copy.deepcopy(policy)
            modified['checks']['selection.contract']['command'].append('--fake-green')
            p.before_policy_enrollment(modified)

    def test_source_enrollment_is_actual_and_complete(self):
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        inputs = policy['checks']['selection.contract']['runtime_inputs']
        for name, sha in controller.source_inputs(ROOT).items():
            self.assertIn('{source}/' + name, inputs)
            self.assertEqual(controller.hashed(ROOT / name), sha)
        for name in p.witness()['input_additions']:
            self.assertEqual(inputs.count(name), 1)
            self.assertTrue((ROOT / name.removeprefix('{source}/')).is_file())

    def test_unapproved_domain_or_architecture_refuses_before_docker(self):
        for domain, platform, arch in (('native', 'linux', 'x86_64'),
                                      ('policy_controller_linux_v1', 'macos', 'arm64'),
                                      ('policy_controller_linux_v1', 'linux', 'amd64')):
            realized = dict(profile_id='static.policy', profile=dict(execution_domain=domain,
                            platform=platform, architecture=arch))
            with self.subTest(domain=domain, platform=platform, arch=arch), patch.object(controller, 'tool', side_effect=AssertionError('tool contacted')):
                with self.assertRaises(controller.PolicyError):
                    controller.contract(realized, {})

    def test_retained_contract_shape_requires_host_tools_and_physical_producer_hashes(self):
        # Schema-only fixture: not Docker execution, input audit or acceptance.
        identity = {'kind': 'committed', 'sha': 'a' * 40}
        proof = dict(kind='policy_controller_contract_v1', source_identity=identity,
            inputs={name: 'b' * 64 for name in controller.INPUTS}, image='sha256:' + 'c' * 64,
            tools={name: {'path': '/fixture/bin/' + name, 'sha256': 'd' * 64} for name in ('docker', 'git', 'python')},
            setup_inputs={name: 'e' * 64 for name in ('/fixture/policy-testhost-setup.json',
                '/fixture/build/image-build.log', '/fixture/build/image-build.log.process.json')},
            readiness=False, scope='authored_provisioned_policy_workload_not_native_profile')
        controller._validate_contract_shape(proof, identity)
        for mutate in (
            lambda p: p.pop('tools'), lambda p: p.pop('setup_inputs'),
            lambda p: p['tools'].pop('git'), lambda p: p['tools']['docker'].update(sha256='not-a-hash'),
            lambda p: p['tools']['python'].update(path='relative/python'),
            lambda p: p['setup_inputs'].pop('/fixture/build/image-build.log.process.json'),
            lambda p: p['setup_inputs'].update({'/different/image-build.log.process.json': p['setup_inputs'].pop('/fixture/build/image-build.log.process.json')}),
        ):
            modified = copy.deepcopy(proof)
            mutate(modified)
            with self.subTest(mutate=mutate), self.assertRaises(controller.PolicyError):
                controller._validate_contract_shape(modified, identity)

    def test_aggregate_refuses_predecessor_status_or_bool_only_policy_proof(self):
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        profile = policy['profiles']['static.policy']
        plan, lane = {'source_identity': {'kind': 'committed', 'sha': 'a' * 40}}, {'profile': 'static.policy'}
        controller.validate_runtime({}, plan, {'profile': 'unrelated'}, {})
        for backend in (
            {},
            {'policy_controller_runtime': {'passed': True}},
            {'required_capabilities': [controller.CAPABILITY]},
            {'required_capabilities': [controller.CAPABILITY], 'policy_controller_runtime': {'status': 'passed', 'cleanup_complete': True}},
            {'required_capabilities': [controller.CAPABILITY], 'policy_controller_runtime': {'passed': True, 'native_profile_acceptance': False}},
        ):
            with self.subTest(backend=backend), self.assertRaises(controller.PolicyError):
                controller.validate_runtime(backend, plan, lane, profile)

    def test_full_source_clone_has_independent_git_objects_and_symlink_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, target, log = root / 'original', root / 'copied', root / 'logs'
            source.mkdir(); log.mkdir()
            def git(*args):
                return subprocess.check_output(['git', '-C', str(source), *args]).decode().strip()
            git('init', '-q')
            (source / 'input').write_text('physical source\n')
            (source / 'link').symlink_to('missing-target')
            git('add', '.')
            git('-c', 'user.name=Policy fixture', '-c', 'user.email=fixture@example.invalid',
                '-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'fixture')
            manifest = controller.clone_source(source, target, controller.tool('git')['path'], log)
            self.assertEqual(manifest['link']['mode'], '120000')
            self.assertEqual((target / 'link').readlink(), Path('missing-target'))
            self.assertEqual((target / 'input').read_bytes(), (source / 'input').read_bytes())
            self.assertFalse((target / '.git/objects/info/alternates').exists())
            self.assertEqual(git('status', '--porcelain'), '')

    def test_unchanged_workload_and_scoped_custody_are_explicit(self):
        source = (ROOT / 'scripts/test_selection_policy.py').read_text()
        for token in ('--cap-drop=ALL', '--security-opt=no-new-privileges', '--network',
                      '--read-only', '--cidfile', "'--security-opt=apparmor=' + APPARMOR_PROFILE",
                      "APPARMOR_PROFILE = 'nostrc-namespace-custody'", 'workload_apparmor(evidence)',
                      'target=/source,readonly', "lifecycle['cleanup'](control)"):
            self.assertIn(token, source)
        for token in ('--privileged', '--cap-add', '--pid=host', '/var/run/docker.sock',
                      'sudo ', 'apparmor_parser', 'apparmor=unconfined', 'sysctl'):
            self.assertNotIn(token, source)
        entry = (ROOT / 'tests/selection/namespace_policy_entry.sh').read_text()
        self.assertIn('run policy-suite python3 -m unittest discover -s tests/selection -v', entry)
        self.assertIn('test_namespace_custody test_namespace_wrapper_adapter test_native_services test_adapter_custody', entry)
        self.assertIn("matrix['skipped'] == 0", entry)


if __name__ == '__main__':
    unittest.main()
