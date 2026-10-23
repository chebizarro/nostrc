"""Provisioning/acceptance separation; real kernel cases remain unchanged."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def read(relative):
    return (ROOT / relative).read_text()


def job(text, name):
    return re.search(r'^  ' + name + r':\n(.*?)(?=^  [A-Za-z_]+:|\Z)', text, re.M | re.S).group(1)


class NamespacePolicyContractTests(unittest.TestCase):
    def test_native_worker_still_required_after_distinct_regressions(self):
        workflow = read('.github/workflows/dependency-tests.yml')
        native = job(workflow, 'policy')
        regression = job(workflow, 'policy_regressions')
        self.assertIn('needs: [select, policy_regressions]', native)
        self.assertIn('uses: ./.github/workflows/dependency-worker.yml', native)
        self.assertIn('profile: static.policy', native)
        self.assertIn('uses: ./.github/workflows/namespace-policy-regression.yml', regression)
        self.assertNotIn('dependency-receipt-', regression)
        self.assertNotIn('continue-on-error', regression + native)
        self.assertIn('policy, broad]', job(workflow, 'gate'))

    def test_broad_setup_preserves_legacy_and_all_static_commands(self):
        broad = job(read('.github/workflows/dependency-broad.yml'), 'static')
        self.assertIn('uses: ./.github/workflows/namespace-policy-regression.yml', broad)
        legacy = read('.github/workflows/static-checks.yml')
        self.assertIn('run: python3 -m unittest discover -s tests/selection', legacy)
        entry = read('tests/selection/namespace_policy_entry.sh')
        commands = [command for command in re.findall(r'^        run: (.+)$', legacy, re.M)
                    if not command.startswith('test ')]  # checkout verification is outside the testhost
        self.assertGreaterEqual(len(commands), 10)
        for command in commands:
            self.assertIn(command, entry)

    def test_only_scoped_disposable_container_policy_is_relaxed(self):
        workflow = read('.github/workflows/namespace-policy-regression.yml')
        for token in ('--user 1001:1001', '--cap-drop=ALL', '--security-opt=no-new-privileges',
                      '--security-opt=apparmor=unconfined', '--security-opt=seccomp=unconfined',
                      '--network none --read-only', 'target=/source,readonly',
                      '--tmpfs /tmp:', '--tmpfs /tmp/.X11-unix:',
                      'target=/evidence', '--cidfile', 'docker inspect', 'docker wait',
                      'namespace_testhost_lifecycle.py cleanup', 'if: always()'):
            self.assertIn(token, workflow)
        for token in ('--privileged', '--cap-add', '--pid=host', '/var/run/docker.sock',
                      'sudo ', 'sysctl ', 'apparmor_parser', 'continue-on-error'):
            self.assertNotIn(token, workflow)
        self.assertIn('namespace_testhost_lifecycle.py prepare', workflow)
        self.assertIn('--name "$(cat "$control/container.name")"', workflow)
        self.assertIn('native-host-probe.exit', workflow)

    def test_original_matrix_and_nonacceptance_receipt_are_explicit(self):
        entry = read('tests/selection/namespace_policy_entry.sh')
        for module in ('test_namespace_custody', 'test_namespace_wrapper_adapter',
                       'test_native_services', 'test_adapter_custody'):
            self.assertIn(module, entry)
        self.assertIn('run policy-suite python3 -m unittest discover -s tests/selection -v', entry)
        self.assertIn('not_executed=sorted(required - commands.keys())', entry)
        self.assertIn('complete=False, complete_gate_eligible=False, native_profile_acceptance=False', entry)
        self.assertNotIn('complete=True', entry)
        self.assertIn("and matrix['tests'] >= 79 and matrix['skipped'] == 0", entry)
        recipe = read('tests/selection/namespace-testhost.Dockerfile')
        self.assertIn('USER 1001:1001', recipe)
        self.assertIn('install -d -o root -g root -m 1777 /tmp/.X11-unix', recipe)
        self.assertRegex(recipe, r'FROM ubuntu:24\.04@sha256:[0-9a-f]{64}')


if __name__ == '__main__':
    unittest.main()
