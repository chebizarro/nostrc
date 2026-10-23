"""Provisioning/acceptance separation; real kernel cases remain unchanged."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def read(relative):
    return (ROOT / relative).read_text()


def job(text, name):
    return re.search(r'^  ' + name + r':\n(.*?)(?=^  [A-Za-z_]+:|\Z)', text, re.M | re.S).group(1)


CONTROLLER = 'tests/selection/namespace_policy_controller.sh'
# The one shared testhost sequence: (step name, command); the stable static job
# names its execute step after the legacy suite step it replaces.
SEQUENCE = (
    ('Bind exact source and retain native launcher diagnosis (not acceptance)', 'bash ' + CONTROLLER + ' bind'),
    ('Record explicitly provisioned disposable Docker regression policy', 'bash ' + CONTROLLER + ' record'),
    ('Provision per-job namespace-custody AppArmor profile',
     'bash tests/selection/namespace_apparmor_setup.sh "$RUNNER_TEMP/namespace-policy/control/apparmor"'),
    ('Build dedicated nonroot regression image', 'bash ' + CONTROLLER + ' build'),
    (None, 'bash ' + CONTROLLER + ' execute'),
    ("Remove only this job's recorded immutable container", 'bash ' + CONTROLLER + ' remove'),
    ("Remove only this job's AppArmor profile after its consumers exited",
     'bash tests/selection/namespace_apparmor_teardown.sh "$RUNNER_TEMP/namespace-policy/control/apparmor"'),
)
ALWAYS = {"Remove only this job's recorded immutable container",
          "Remove only this job's AppArmor profile after its consumers exited"}


def steps(block):
    """Ordered (name, if, run, uses) for each step of one job block."""
    result = []
    for chunk in re.split(r'^      - ', block, flags=re.M)[1:]:
        field = lambda key: (re.search(r'^(?:      |        )' + key + r': (.*)$', '        ' + chunk, re.M) or [None, None])[1]
        run = field('run')
        if run == '|':
            run = re.search(r'run: \|\n((?:          .*\n?)+)', chunk).group(1)
        result.append(dict(name=field('name'), condition=field('if'), run=run, uses=field('uses')))
    return result


def controller_steps(block, execute_name):
    found = [step for step in steps(block)
             if step['run'] and ('namespace_policy_controller.sh' in step['run'] or 'namespace_apparmor_' in step['run'])]
    expected = [(name or execute_name, command) for name, command in SEQUENCE]
    observed = [(step['name'], step['run'].strip().splitlines()[-1].strip()) for step in found]
    return found, expected, observed


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
        entry = read('tests/selection/namespace_policy_entry.sh')
        # The legacy suite step executes the testhost entry, whose policy-suite is
        # the literal full default discovery (no subset, no host-side copy).
        self.assertIn('run policy-suite python3 -m unittest discover -s tests/selection -v', entry)
        self.assertNotIn('unittest discover', legacy)
        commands = [command for command in re.findall(r'^        run: (.+)$', legacy, re.M)
                    if not command.startswith('test ')  # checkout verification is outside the testhost
                    and command != '|' and command not in {cmd for _, cmd in SEQUENCE}]
        self.assertEqual(len(commands), 9)
        for command in commands:
            self.assertIn(command, entry)

    def test_only_scoped_disposable_container_policy_is_relaxed(self):
        workflow = read('.github/workflows/namespace-policy-regression.yml')
        legacy = read('.github/workflows/static-checks.yml')
        controller = read(CONTROLLER)
        for token in ('--user 1001:1001', '--cap-drop=ALL', '--security-opt=no-new-privileges',
                      '--security-opt="apparmor=$profile"', '-e "NOSTRC_NAMESPACE_APPARMOR_PROFILE=$profile"',
                      '--security-opt=seccomp=unconfined',
                      '--network none --read-only', 'target=/source,readonly',
                      '--tmpfs /tmp:', '--tmpfs /tmp/.X11-unix:',
                      'target=/evidence', '--cidfile', 'docker inspect', 'docker wait',
                      'namespace_testhost_lifecycle.py cleanup'):
            self.assertIn(token, controller)
        for text in (workflow, legacy):
            for token in ('bash tests/selection/namespace_apparmor_setup.sh',
                          'bash tests/selection/namespace_apparmor_teardown.sh', 'if: always()'):
                self.assertIn(token, text)
        for text in (workflow, legacy, controller):
            for token in ('--privileged', '--cap-add', '--pid=host', '/var/run/docker.sock',
                          'sudo ', 'sysctl ', 'apparmor_parser', 'continue-on-error', 'apparmor=unconfined'):
                self.assertNotIn(token, text)
        self.assertIn('namespace_testhost_lifecycle.py prepare', controller)
        self.assertIn('--name "$(cat "$control/container.name")"', controller)
        self.assertIn('native-host-probe.exit', controller)
        self.assertIn('tests/selection/namespace_policy_entry.sh\n', controller)
        for text in (workflow, legacy):
            # Teardown runs unconditionally, only after the container was removed.
            removal = text.index("name: Remove only this job's recorded immutable container")
            teardown = text.index("name: Remove only this job's AppArmor profile after its consumers exited")
            self.assertLess(removal, teardown)
            self.assertIn('if: always()', text[teardown:teardown + 200])

    def test_stable_static_job_reuses_the_one_controller_with_bounded_walltime(self):
        regression = job(read('.github/workflows/namespace-policy-regression.yml'), 'regressions')
        text = read('.github/workflows/static-checks.yml')
        self.assertEqual(re.findall(r'^  ([A-Za-z_-]+):$', text.split('\njobs:\n', 1)[1], re.M), ['static-checks'])
        legacy = job(text, 'static-checks')
        # Identity, triggers and every original finite check are unchanged; no
        # reusable-workflow call renames the check and no second controller exists.
        self.assertIn('name: Static checks\n', text)
        for trigger in ('  workflow_call:', '  push:', '  pull_request:', '  workflow_dispatch:'):
            self.assertIn('\n' + trigger + '\n', text)
        self.assertNotIn('schedule:', text)
        self.assertNotIn('uses: ./.github/workflows/', legacy)
        self.assertNotIn('docker ', legacy)
        self.assertIn('    timeout-minutes: 20\n', legacy)
        self.assertIn('    timeout-minutes: 45\n', regression)
        self.assertIn('REQUESTED_SHA: ${{ inputs.sha || github.sha }}', legacy)
        self.assertIn('ref: ${{ inputs.sha || github.sha }}', legacy)
        # Same full history as the controller's own checkout: the suite reads
        # pinned ancestor commits (test_vector_family_integration witnesses).
        checkout = legacy.split('- name: Checkout\n', 1)[1].split('- name: Verify requested checkout', 1)[0]
        self.assertIn('          fetch-depth: 0\n', checkout)
        self.assertIn('          fetch-depth: 0\n', regression)
        for found, expected, observed in (controller_steps(regression, 'Execute unchanged kernel matrix and full policy suite'),
                                          controller_steps(legacy, 'Selection and workflow contract tests')):
            self.assertEqual(observed, expected)
            for step in found:
                self.assertEqual(step['condition'], 'always()' if step['name'] in ALWAYS else None, step['name'])
            self.assertIn('test "$(git rev-parse HEAD)" = "$REQUESTED_SHA"', found[0]['run'])
        names = [step['name'] for step in steps(legacy)]
        original = ['Checkout', 'Verify requested checkout', 'Selection and workflow contract tests',
                    'No call modifies a variable another argument uses',
                    'Every GUI test installs narrow GDK frame tolerance',
                    'Local Linux image mirrors the CI package lists', 'Pre-push hook behaviour',
                    'Opt-in full gate behaviour', 'Linux gate smoke run keeps and counts reruns',
                    'Sanitizer gate reads the CI job and filters pushes', 'Old signer names stay gone (W31)',
                    'Keychain guard self-test (nostrc-2hmd)']
        self.assertEqual([name for name in names if name in original], original)
        upload = next(step for step in steps(legacy) if step['uses'] == 'actions/upload-artifact@v4')
        self.assertEqual(upload['condition'], 'always()')
        self.assertIn('name: ${{ inputs.artifact_prefix }}static-checks-testhost-${{ inputs.sha || github.sha }}-'
                      '${{ github.run_attempt }}', legacy)
        self.assertNotIn('continue-on-error', legacy)
        # Unknown operations refuse rather than fall through.
        self.assertIn("  *)\n    echo 'usage: namespace_policy_controller.sh", read(CONTROLLER))

    def test_only_privileged_actions_are_owned_profile_add_scan_and_exact_removal(self):
        setup = read('tests/selection/namespace_apparmor_setup.sh')
        teardown = read('tests/selection/namespace_apparmor_teardown.sh')
        def privileged(script):
            return [line.strip() for line in script.splitlines() if 'sudo' in line and not line.lstrip().startswith('#')]
        self.assertEqual(privileged(setup), [
            'policy() { sudo cat /sys/kernel/security/apparmor/profiles | LC_ALL=C sort; }',
            'sudo "$parser" --add --warn=all "$out/profile" 2>&1 | tee "$out/apparmor-load.log"'])
        self.assertEqual(privileged(teardown), [
            'policy() { sudo cat /sys/kernel/security/apparmor/profiles | LC_ALL=C sort; }',
            'if ! users="$(sudo python3 -I - "$name" <<\'PY\'',
            'sudo "$parser" --remove "$out/profile" 2>&1 | tee "$out/apparmor-remove.log"'])
        self.assertIn('RUNNER_ENVIRONMENT:-}" != github-hosted', setup)
        self.assertIn('secrets.token_hex(16)', setup)
        self.assertIn('cmp "$out/apparmor-settings-before.txt" "$out/apparmor-settings-after.txt"', setup)
        self.assertIn('test ! -s "$out/profiles-removed.txt"', setup)
        self.assertIn('namespace_host_probe.py" > "$out/probe-unconfined.jsonl"', setup)
        self.assertIn('cmp -s "$out/profiles-before.txt" "$out/profiles-after.txt"', teardown)
        for line in privileged(setup) + privileged(teardown):
            self.assertNotIn('--replace', line)
            self.assertNotIn(' -r ', line)
        for script in (setup, teardown):
            for token in ('sysctl', '--replace', ' -R ', 'aa-disable', 'aa-complain', 'complain',
                          '/proc/sys/kernel/' + 'apparmor_restrict_unprivileged_userns >',
                          'echo 0', 'continue-on-error', '|| true'):
                self.assertNotIn(token, script)
        self.assertNotIn('--remove', setup)
        template = [line.strip() for line in read('tests/selection/apparmor/nostrc-namespace-custody').splitlines()
                    if line.strip() and not line.lstrip().startswith('#')]
        # Path-unattached, unconfined-mode, and the userns permission only.
        self.assertEqual(template, ['abi <abi/4.0>,', 'profile @PROFILE_NAME@ flags=(unconfined) {',
                                    'userns,', '}'])

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
