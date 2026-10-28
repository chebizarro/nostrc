"""Provisioning/acceptance separation; real kernel cases remain unchanged."""
import ast
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


def read(relative):
    return (ROOT / relative).read_text()


def job(text, name):
    return re.search(r'^  ' + name + r':\n(.*?)(?=^  [A-Za-z_]+:|\Z)', text, re.M | re.S).group(1)


CONTROLLER = 'tests/selection/namespace_policy_controller.sh'
HELPER = 'tests/selection/static_frontend_host.py'
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


def host_commands():
    """The helper's literal COMMANDS table, read without importing it."""
    tree = ast.parse(read(HELPER))
    node = next(n for n in tree.body if isinstance(n, ast.Assign) and getattr(n.targets[0], 'id', None) == 'COMMANDS')
    return [(name, list(argv)) for name, argv in ast.literal_eval(node.value)]


def proc_stat(pid):
    try:
        raw = Path('/proc/%d/stat' % pid).read_text()
    except OSError:
        return None
    fields = raw[raw.rindex(')') + 2:].split()
    return fields[0], int(fields[3]), int(fields[19])


def running(pid, starttime=None):
    observed = proc_stat(pid)
    return observed is not None and observed[0] != 'Z' and (starttime is None or observed[2] == starttime)


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
        ordinary = read('.github/workflows/static-checks.yml')
        entry = read('tests/selection/namespace_policy_entry.sh')
        self.assertIn('run policy-suite python3 -m unittest discover -s tests/selection -v', entry)
        self.assertIn('python3 scripts/test-selection-operational.py', ordinary)
        self.assertNotIn(CONTROLLER, ordinary)
        commands = host_commands()
        self.assertEqual([name for name, _ in commands],
                         ['unsequenced', 'gui-frame', 'linux-packages', 'pre-push', 'full-gate',
                          'linux-smoke', 'sanitizer', 'signer-names', 'keychain-guard'])
        for _, argv in commands:
            command = ' '.join(argv)
            self.assertIn(command, entry)
            self.assertIn('run: ' + command, ordinary)
        self.assertNotIn('python3 ' + HELPER + ' replay ', ordinary)
        helper = read(HELPER)
        for token in ('timeout=', 'pgrep', 'killpg(-', 'os.kill(-'):
            self.assertNotIn(token, helper)

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
        for text in (workflow,):
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
        for text in (workflow,):
            # Teardown runs unconditionally, only after the container was removed.
            removal = text.index("name: Remove only this job's recorded immutable container")
            teardown = text.index("name: Remove only this job's AppArmor profile after its consumers exited")
            self.assertLess(removal, teardown)
            self.assertIn('if: always()', text[teardown:teardown + 200])

    def test_static_and_manual_diagnostic_routes_stay_separate(self):
        regression = job(read('.github/workflows/namespace-policy-regression.yml'), 'regressions')
        text = read('.github/workflows/static-checks.yml')
        self.assertEqual(re.findall(r'^  ([A-Za-z_-]+):$', text.split('\njobs:\n', 1)[1], re.M), ['static-checks'])
        ordinary = job(text, 'static-checks')
        self.assertIn('name: Static checks\n', text)
        for trigger in ('workflow_call', 'workflow_dispatch'):
            self.assertIn('\n  ' + trigger + ':\n', text)
        for trigger in ('push', 'pull_request', 'schedule'):
            self.assertNotIn('\n  ' + trigger + ':', text)
        self.assertIn('    timeout-minutes: 20\n', ordinary)
        self.assertIn('    timeout-minutes: 45\n', regression)
        for route in (ordinary, regression):
            self.assertIn('ref: ${{ inputs.sha || github.sha }}', route)
            self.assertIn('          fetch-depth: 0\n', route)
            self.assertNotIn('continue-on-error', route)
        self.assertNotIn(CONTROLLER, ordinary)
        self.assertNotIn('docker ', ordinary)
        found, expected, observed = controller_steps(regression, 'Execute unchanged kernel matrix and full policy suite')
        self.assertEqual(observed, expected)
        for step in found:
            self.assertEqual(step['condition'], 'always()' if step['name'] in ALWAYS else None, step['name'])
        self.assertIn('test "$(git rev-parse HEAD)" = "$REQUESTED_SHA"', found[0]['run'])
        upload = next(step for step in steps(regression) if step['uses'] == 'actions/upload-artifact@v4')
        self.assertEqual(upload['condition'], 'always()')
        self.assertIn('namespace-policy-regressions-${{ inputs.sha || github.sha }}-${{ github.run_attempt }}', regression)
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


@unittest.skipUnless(platform.system() == 'Linux',
                     'the owned host group is the Linux runner-host lifecycle (/proc, child subreaper)')
class HostFrontendGroupLifecycle(unittest.TestCase):
    """The real helper with only its COMMANDS table swapped for stand-ins."""

    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix='host-frontend-'))
        self.runner = self.temp / 'runner'
        self.runner.mkdir()
        self.root = self.runner / 'namespace-policy/host-frontend'
        head = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True)
        self.env = dict(os.environ, RUNNER_TEMP=str(self.runner), REQUESTED_SHA=head.stdout.strip())
        self.addCleanup(shutil.rmtree, self.temp, True)
        self.addCleanup(self.end_owned)

    def end_owned(self):
        if (self.root / 'owner.json').exists():  # never leave a supervisor: end it through its own record
            self.helper_run('ensure-ended')

    def helper(self, commands):
        block = 'COMMANDS = (\n' + ''.join('    (%r, %r),\n' % (n, tuple(a)) for n, a in commands) + ')\n'
        text, count = re.subn(r'(?s)(# BEGIN COMMANDS[^\n]*\n).*?(# END COMMANDS)',
                              lambda m: m.group(1) + block + m.group(2), read(HELPER))
        self.assertEqual(count, 1)
        self.path = self.temp / 'helper.py'
        self.path.write_text(text)

    def helper_run(self, *args, status=None):
        env = dict(self.env, **({'JOB_STATUS': status} if status else {}))
        return subprocess.run([sys.executable, str(self.path), *args], cwd=ROOT, env=env,
                              capture_output=True, text=True, timeout=60)

    def owner(self):
        return json.loads((self.root / 'owner.json').read_text())

    def final(self):
        return json.loads((self.root / 'final.json').read_text())

    def results(self):
        return [(c['name'], c['status'], c['exit']) for c in self.final()['commands']]

    def test_production_table_is_the_original_host_argv(self):
        self.assertEqual(host_commands(), [
            ('unsequenced', ['python3', 'scripts/check-unsequenced-args.py', '--self-test']),
            ('gui-frame', ['python3', 'scripts/check-gui-test-gdk-frame.py', '--self-test']),
            ('linux-packages', ['python3', 'scripts/check-linux-ci-packages.py', '--self-test']),
            ('pre-push', ['bash', 'scripts/test-pre-push.sh']),
            ('full-gate', ['bash', 'scripts/test-full-gate.sh']),
            ('linux-smoke', ['bash', 'scripts/test-linux-gate-smoke.sh']),
            ('sanitizer', ['bash', 'scripts/test-sanitizer-gate.sh']),
            ('signer-names', ['bash', 'scripts/check-grotto-names.sh']),
            ('keychain-guard', ['bash', 'scripts/test-check-macos-keychain.sh'])])

    def test_join_seals_order_exits_and_ends_orphans_from_its_own_child_list(self):
        self.helper([('a', ['true']), ('b', ['false']), ('c', ['bash', '-c', 'sleep 60 & exit 0']), ('d', ['true'])])
        self.assertEqual(self.helper_run('replay', 'a').returncode, 1)  # nothing sealed before a join
        self.assertEqual(self.helper_run('start').returncode, 0)
        self.assertIn('already exists', self.helper_run('start').stderr)  # one owned group per job
        owner = self.owner()
        self.assertEqual(sorted(owner), ['boot_id', 'kind', 'pid', 'scope', 'starttime', 'token', 'uid'])
        joined = self.helper_run('join', status='success')
        self.assertEqual(joined.returncode, 0, joined.stdout + joined.stderr)
        self.assertFalse(running(owner['pid'], owner['starttime']))  # released last, after cleanup
        final = self.final()
        self.assertEqual(self.results(), [('a', 'exited', 0), ('b', 'exited', 1), ('c', 'exited', 0), ('d', 'exited', 0)])
        self.assertTrue(final['completed'])
        self.assertEqual((final['survivors'], final['released_by']), ([], 'join'))
        self.assertEqual([line.split(' ', 1)[1] for line in final['orphans_ended']], ['sleep 60'])
        orphan = int(final['orphans_ended'][0].split()[0])
        self.assertFalse(running(orphan) and 'sleep' in Path('/proc/%d/cmdline' % orphan).read_text())
        self.assertFalse(final['complete'] or final['complete_gate_eligible'])
        for name, code in (('a', 0), ('b', 1), ('c', 0), ('d', 0)):
            self.assertEqual(self.helper_run('replay', name).returncode, code, name)
        self.assertEqual(self.helper_run('ensure-ended').returncode, 0)  # already ended and sealed

    def wait_for(self, path):
        deadline = time.monotonic() + 15
        while not path.exists():
            self.assertLess(time.monotonic(), deadline, path)
            time.sleep(0.02)

    def test_signal_during_join_takes_the_bounded_cancel_path_and_seals_evidence(self):
        self.helper([('a', ['sleep', '30']), ('b', ['true'])])
        self.assertEqual(self.helper_run('start').returncode, 0)
        owner = self.owner()
        join = subprocess.Popen([sys.executable, str(self.path), 'join'], cwd=ROOT,
                                env=dict(self.env, JOB_STATUS='success'), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.wait_for(self.root / 'logs/a.log')
        time.sleep(0.3)  # join is now waiting on a running command
        begun = time.monotonic()
        join.send_signal(signal.SIGTERM)
        self.assertNotEqual(join.wait(timeout=20), 0)
        join.stdout.close()
        join.stderr.close()
        self.assertLess(time.monotonic() - begun, 7)  # inside the runner's cancellation grace
        self.assertFalse(running(owner['pid'], owner['starttime']))
        final = self.final()
        self.assertEqual([(n, s) for n, s, _ in self.results()], [('a', 'cancelled'), ('b', 'not_executed')])
        self.assertTrue(final['cancelled'] and not final['completed'])
        self.assertEqual((final['released_by'], final['survivors']), ('cancel', []))
        self.assertFalse(running(final['commands'][0]['pid']))
        self.assertEqual(self.helper_run('replay', 'a').returncode, 1)
        self.assertEqual(self.helper_run('replay', 'b').returncode, 1)

    def test_cancelled_job_status_at_join_cancels(self):
        self.helper([('a', ['sleep', '30'])])
        self.assertEqual(self.helper_run('start').returncode, 0)
        self.wait_for(self.root / 'logs/a.log')
        self.assertEqual(self.helper_run('join', status='cancelled').returncode, 1)
        self.assertEqual(self.results()[0][1], 'cancelled')

    def supervise(self, block):
        """The supervisor alone, as start launches it, with this test holding the handshake."""
        self.root.mkdir(parents=True)
        (self.root / 'logs').mkdir()
        go_read, go_write = os.pipe()
        ready_read, ready_write = os.pipe()
        mask = (lambda: signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM, signal.SIGUSR1})) \
            if block else None
        process = subprocess.Popen([sys.executable, str(self.path), 'supervise', str(self.root), 'f' * 64,
                                    str(go_read), str(ready_write)], pass_fds=(go_read, ready_write),
                                   start_new_session=True, preexec_fn=mask)
        self.addCleanup(lambda: process.poll() is None and (process.kill(), process.wait()))
        os.close(go_read)
        os.close(ready_write)
        self.assertEqual(os.read(ready_read, 16), b'ready\n')
        os.close(ready_read)
        return process, go_write

    def test_interrupted_publication_runs_nothing_and_cannot_be_claimed(self):
        self.helper([('a', ['touch', str(self.temp / 'ran')])])
        process, go = self.supervise(block=False)
        os.close(go)  # start died after launch, before publication
        self.assertEqual(process.wait(timeout=15), 3)
        self.assertEqual(list((self.root / 'logs').iterdir()), [])
        self.assertFalse((self.temp / 'ran').exists())
        ended = self.helper_run('ensure-ended')
        self.assertEqual(ended.returncode, 1)
        self.assertIn('never published', ended.stderr)

    def test_signal_pending_during_publication_is_honoured_after_go(self):
        self.helper([('a', ['touch', str(self.temp / 'ran')])])
        process, go = self.supervise(block=True)
        process.send_signal(signal.SIGTERM)  # blocked: launch and publication cannot be interrupted
        time.sleep(0.3)
        self.assertIsNone(process.poll())
        os.write(go, b'go\n')
        os.close(go)
        self.assertEqual(process.wait(timeout=15), 0)
        self.assertFalse((self.temp / 'ran').exists())
        self.assertEqual(self.results(), [('a', 'not_executed', None)])
        self.assertTrue(self.final()['cancelled'])

    def test_forged_stale_or_foreign_record_never_signals_an_unrelated_process(self):
        self.helper([('a', ['true'])])
        victim = subprocess.Popen(['sleep', '30'])
        self.addCleanup(lambda: (victim.kill(), victim.wait()))
        observed = proc_stat(victim.pid)
        self.root.mkdir(parents=True)
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        base = dict(kind='static_host_frontend_group_v1', token='f' * 64, pid=victim.pid, starttime=observed[2],
                    boot_id=boot, uid=os.getuid(), scope='scheduling_only_not_acceptance')
        for name, record, message in (('forged token/session', base, 'refused'),
                                      ('stale start time', dict(base, starttime=observed[2] + 1), 'without a sealed result'),
                                      ('other boot', dict(base, boot_id='other'), 'refused'),
                                      ('malformed', dict(base, pid='1'), 'refused')):
            with self.subTest(case=name):
                (self.root / 'owner.json').write_text(json.dumps(record))
                for verb, status in (('ensure-ended', None), ('join', 'cancelled')):
                    result = self.helper_run(verb, status=status)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertIn(message, result.stderr)
                    self.assertIsNone(victim.poll())  # the unrelated process is untouched
                    self.assertTrue(running(victim.pid, observed[2]))
        # Same-UID session leaders that pass the pid/start/boot/uid/session checks; each is this
        # test's own unreaped child. Only the published 64-hex token in start's exact launch
        # argv for THIS root may be treated as the owned supervisor.
        def leader(argv, **kwargs):
            process = subprocess.Popen(argv, start_new_session=True, **kwargs)
            self.addCleanup(lambda: process.poll() is None and (process.kill(), process.wait()))
            return process, proc_stat(process.pid)[2]

        def untouched(process, start):
            status = Path('/proc/%d/status' % process.pid).read_text().splitlines()
            pending = [int(line.split()[1], 16) for line in status if line.startswith(('SigPnd:', 'ShdPnd:'))]
            return process.poll() is None and running(process.pid, start) and pending == [0, 0]

        hex_token = '0123456789abcdef' * 4
        sleeper, sleeper_start = leader(['sleep', '30'])
        python, python_start = leader([sys.executable, '-c', 'import time; time.sleep(30)', hex_token])
        foreign = self.temp / 'foreign-runner/namespace-policy/host-frontend'
        (foreign / 'logs').mkdir(parents=True)
        go_read, go_write = os.pipe()
        ready_read, ready_write = os.pipe()
        blocked = lambda: signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM, signal.SIGUSR1})
        supervisor, supervisor_start = leader([sys.executable, '-I', str(self.path), 'supervise', str(foreign),
                                               'a' * 64, str(go_read), str(ready_write)],
                                              pass_fds=(go_read, ready_write), preexec_fn=blocked)
        self.addCleanup(os.close, go_write)  # never "go": a closed handshake runs nothing
        os.close(go_read)
        os.close(ready_write)
        self.assertEqual(os.read(ready_read, 16), b'ready\n')
        os.close(ready_read)
        without = {k: v for k, v in base.items() if k != 'token'}
        for name, process, start, record, message in (
                ('missing token', sleeper, sleeper_start, without, 'ownership record malformed'),
                ('empty token', sleeper, sleeper_start, dict(base, token=''), 'ownership record malformed'),
                ('non-string token', sleeper, sleeper_start, dict(base, token=123), 'ownership record malformed'),
                ('token equal to an argument', sleeper, sleeper_start, dict(base, token='30'),
                 'ownership record malformed'),
                ('well-formed token as an unrelated argument', python, python_start, dict(base, token=hex_token),
                 'not the owned supervisor'),
                ('supervisor shape for a foreign root', supervisor, supervisor_start, dict(base, token='a' * 64),
                 'not the owned supervisor')):
            with self.subTest(case=name):
                self.assertTrue(untouched(process, start))
                (self.root / 'owner.json').write_text(json.dumps(dict(record, pid=process.pid, starttime=start)))
                for verb, status in (('ensure-ended', None), ('join', 'cancelled')):
                    result = self.helper_run(verb, status=status)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertIn('refused', result.stderr)
                    self.assertIn(message, result.stderr)
                    self.assertTrue(untouched(process, start))  # alive, no signal delivered or pending
        (self.root / 'owner.json').unlink()

    def test_exited_process_is_ended_and_never_signalled(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('static_frontend_host', ROOT / HELPER)
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        token = 'e' * 64
        child = subprocess.Popen([sys.executable, '-c', 'import sys; sys.exit(0)', token])
        observed = proc_stat(child.pid)
        record = dict(kind=helper.KIND, token=token, pid=child.pid, starttime=observed[2],
                      boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip(), uid=os.getuid())
        deadline = time.monotonic() + 15
        while proc_stat(child.pid)[0] != 'Z':  # exited, deliberately not yet reaped
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        self.assertEqual(helper.owner_status(record), 'ended')
        self.assertFalse(helper.signal_owned(record, signal.SIGTERM))
        child.wait()
        self.assertEqual(helper.owner_status(record), 'ended')  # reaped: the number is no longer ours
        self.assertFalse(helper.signal_owned(record, signal.SIGTERM))

    def test_missing_duplicate_tampered_or_unknown_results_refuse(self):
        self.helper([('a', ['true']), ('b', ['true'])])
        self.assertEqual(self.helper_run('start').returncode, 0)
        self.assertEqual(self.helper_run('join', status='success').returncode, 0)
        sealed = (self.root / 'final.json').read_text()
        final = json.loads(sealed)
        self.assertEqual(self.helper_run('replay', 'a').returncode, 0)
        for name, mutate in (('duplicate', lambda f: f['commands'].append(dict(f['commands'][0]))),
                             ('missing', lambda f: f['commands'].pop(0)),
                             ('argv drift', lambda f: f['commands'][0].update(argv=['false'])),
                             ('not executed', lambda f: f['commands'][0].update(status='not_executed', exit=None))):
            with self.subTest(case=name):
                changed = json.loads(sealed)
                mutate(changed)
                (self.root / 'final.json').write_text(json.dumps(changed))
                self.assertEqual(self.helper_run('replay', 'a').returncode, 1)
        (self.root / 'final.json').write_text(sealed)
        (self.root / 'logs/a.log').write_text('forged\n')
        self.assertEqual(self.helper_run('replay', 'a').returncode, 1)  # log differs from its sealed hash
        self.assertEqual(self.helper_run('replay', 'unknown').returncode, 1)
        self.assertEqual(final['logs_sha256'].keys(), {'a.log', 'b.log'})

    def test_nothing_started_owns_nothing(self):
        self.helper([('a', ['true'])])
        for verb in ('join', 'ensure-ended'):
            self.assertEqual(self.helper_run(verb).returncode, 0)
        self.assertFalse(self.root.exists())

class TesthostReceiptVerdict(unittest.TestCase):
    """The entry's own receipt summary over synthetic command records (tool lookup stubbed)."""

    def summary(self, records):
        entry = read('tests/selection/namespace_policy_entry.sh')
        code = entry.split("python3 - <<'PY'\n")[-1].split('\nPY\n')[0]
        temp = Path(tempfile.mkdtemp(prefix='receipt-'))
        self.addCleanup(shutil.rmtree, temp, True)
        (temp / 'logs').mkdir()
        (temp / 'logs/kernel-matrix.log').write_text('Ran 108 tests in 1.0s\n\nOK\n')
        (temp / 'commands.tsv').write_text(''.join('%s\t%s\n' % pair for pair in records))
        self.assertEqual(code.count("pathlib.Path('/evidence')"), 1)
        script = temp / 'summary.py'
        script.write_text('import shutil, sys\nshutil.which = lambda name: sys.executable\n'
                          + code.replace("pathlib.Path('/evidence')", 'pathlib.Path(%r)' % str(temp)))
        result = subprocess.run([sys.executable, '-I', str(script)], cwd=ROOT, capture_output=True, text=True, timeout=120)
        return result.returncode, json.loads((temp / 'regression.json').read_text())

    REQUIRED = ('environment', 'mapping-probe', 'kernel-prerequisite', 'kernel-matrix', 'policy-suite',
                'unsequenced', 'gui-frame', 'linux-packages', 'pre-push', 'full-gate', 'linux-smoke',
                'sanitizer', 'signer-names', 'keychain-guard')

    def test_clean_records_pass(self):
        code, receipt = self.summary([(name, 0) for name in self.REQUIRED])
        self.assertEqual((code, receipt['passed'], receipt['duplicate_commands']), (0, True, []))

    def test_duplicate_record_fails_even_when_its_last_exit_is_zero(self):
        for tail in ([('pre-push', 1), ('pre-push', 0)], [('pre-push', 0), ('pre-push', 0)]):
            with self.subTest(tail=tail):
                records = [(name, 0) for name in self.REQUIRED if name != 'pre-push'] + tail
                code, receipt = self.summary(records)
                self.assertEqual((code, receipt['passed'], receipt['duplicate_commands']), (1, False, ['pre-push']))
                self.assertFalse(receipt['complete'] or receipt['complete_gate_eligible'])

    def test_missing_or_nonzero_records_fail(self):
        code, receipt = self.summary([(name, 0) for name in self.REQUIRED if name != 'sanitizer'])
        self.assertEqual((code, receipt['passed'], receipt['not_executed']), (1, False, ['sanitizer']))
        code, receipt = self.summary([(name, int(name == 'full-gate')) for name in self.REQUIRED])
        self.assertEqual((code, receipt['passed']), (1, False))

if __name__ == '__main__':
    unittest.main()
