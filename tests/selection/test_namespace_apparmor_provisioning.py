"""Per-job AppArmor provisioning scripts against a fake sudo/policy, NOT real AppArmor.

Proves admission, add-never-replace, exact delta, refusal and exact-owned
removal logic without privilege. Real effect is proved only on a hosted runner.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SETUP = ROOT / 'tests/selection/namespace_apparmor_setup.sh'
TEARDOWN = ROOT / 'tests/selection/namespace_apparmor_teardown.sh'
FAKE_SUDO = r'''#!/usr/bin/env python3
import json, os, re, sys
from pathlib import Path
args = sys.argv[1:]
with open(os.environ['FAKE_TRACE'], 'a') as trace: trace.write(json.dumps(args) + '\n')
policy = Path(os.environ['FAKE_POLICY'])
def name(path): return re.search(r'^profile (\S+) flags=\(unconfined\) \{$', Path(path).read_text(), re.M)[1]
if args == ['cat', '/sys/kernel/security/apparmor/profiles']:
    sys.stdout.write(policy.read_text()); sys.exit(0)
if len(args) == 4 and args[0].endswith('apparmor_parser') and args[1:3] == ['--add', '--warn=all']:
    rows = policy.read_text().splitlines()
    if any(r.split(' ')[0] == name(args[3]) for r in rows): print('Profile already exists'); sys.exit(1)
    policy.write_text('\n'.join(rows + [name(args[3]) + ' (unconfined)']) + '\n'); sys.exit(0)
if len(args) == 3 and args[0].endswith('apparmor_parser') and args[1] == '--remove':
    rows = [r for r in policy.read_text().splitlines() if r != name(args[2]) + ' (unconfined)']
    policy.write_text('\n'.join(rows) + '\n'); sys.exit(0)
if args[:3] == ['python3', '-I', '-']:
    sys.stdin.read()
    if os.environ.get('FAKE_USERS'): print(os.environ['FAKE_USERS'])
    sys.exit(0)
sys.exit(99)
'''
HOSTED = dict(GITHUB_ACTIONS='true', RUNNER_ENVIRONMENT='github-hosted', RUNNER_OS='Linux', ImageOS='ubuntu24')


class AppArmorProvisioning(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.bin = self.base / 'bin'
        self.bin.mkdir()
        (self.bin / 'sudo').write_text(FAKE_SUDO)
        (self.bin / 'apparmor_parser').write_text('#!/bin/sh\nexit 97\n')  # only ever via sudo
        for tool in ('sudo', 'apparmor_parser'):
            (self.bin / tool).chmod(0o755)
        self.policy = self.base / 'profiles'
        self.policy.write_text('docker-default (enforce)\nunprivileged_userns (enforce)\n')
        self.trace = self.base / 'trace'
        self.out = self.base / 'out'
        clean = {k: v for k, v in os.environ.items() if k not in HOSTED}
        self.env = dict(clean, PATH=str(self.bin) + os.pathsep + os.environ['PATH'],
                        FAKE_POLICY=str(self.policy), FAKE_TRACE=str(self.trace))

    def run_script(self, script, **env):
        return subprocess.run(['bash', str(script), str(self.out)], env=dict(self.env, **env),
                              capture_output=True, text=True, timeout=60)

    def calls(self):
        return [json.loads(x) for x in self.trace.read_text().splitlines()] if self.trace.exists() else []

    def setup_loaded(self):
        result = self.run_script(SETUP, **HOSTED)
        journal = json.loads((self.out / 'setup.json').read_text())
        # Without real AppArmor the positive probe cannot succeed, so setup
        # fails AFTER the add; the add/delta logic is what this harness proves.
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(journal['state'], 'loaded', result.stderr)
        return journal

    def test_refuses_without_explicit_github_hosted_admission(self):
        for missing in (*HOSTED, None):
            env = dict(HOSTED)
            if missing:
                env.pop(missing)
            else:
                env['RUNNER_ENVIRONMENT'] = 'self-hosted'
            with self.subTest(missing=missing):
                result = self.run_script(SETUP, **env)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn('ephemeral GitHub-hosted', result.stderr)
                self.assertEqual(self.calls(), [])  # no privileged call at all

    def test_adds_fresh_owned_profile_with_exact_delta_then_removes_only_it(self):
        before = self.policy.read_text()
        journal = self.setup_loaded()
        name = journal['name']
        self.assertRegex(name, r'^nostrc-ns-[0-9a-f]{32}$')
        self.assertEqual(journal['admission']['RUNNER_ENVIRONMENT'], 'github-hosted')
        self.assertEqual((self.out / 'profiles-added.txt').read_text(), name + ' (unconfined)\n')
        self.assertEqual((self.out / 'profiles-removed.txt').read_text(), '')
        rendered = (self.out / 'profile').read_text()
        self.assertEqual(rendered.count(name), 1)
        self.assertNotIn('@', rendered)
        self.assertIn('profile ' + name + ' flags=(unconfined) {\n  userns,\n}', rendered)
        privileged = [c for c in self.calls() if c[0] != 'cat']
        self.assertEqual([c[1] for c in privileged], ['--add'])  # never --replace
        result = self.run_script(TEARDOWN)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.out / 'teardown.json').read_text())['status'], 'removed')
        self.assertEqual(self.policy.read_text(), before)
        self.assertEqual([c[1] for c in self.calls() if c[0] != 'cat'], ['--add', '-I', '--remove'])

    def test_existing_name_is_never_replaced(self):
        journal = self.setup_loaded()
        os.remove(self.out / 'setup.json')
        rendered = self.out / 'profile'
        added = subprocess.run([str(self.bin / 'sudo'), str(self.bin / 'apparmor_parser'), '--add', '--warn=all',
                                str(rendered)], env=self.env, capture_output=True, text=True)
        self.assertEqual(added.returncode, 1)
        self.assertIn(journal['name'] + ' (unconfined)', self.policy.read_text())

    def test_teardown_refuses_while_a_consumer_runs_or_bytes_changed(self):
        journal = self.setup_loaded()
        result = self.run_script(TEARDOWN, FAKE_USERS='4242')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads((self.out / 'teardown.json').read_text())['status'], 'incomplete')
        self.assertIn(journal['name'], self.policy.read_text())  # retained, not removed
        (self.out / 'profile').write_text((self.out / 'profile').read_text() + '# changed\n')
        result = self.run_script(TEARDOWN)
        self.assertEqual(result.returncode, 1)
        self.assertIn('bytes changed', json.loads((self.out / 'teardown.json').read_text())['detail'])
        self.assertIn(journal['name'], self.policy.read_text())

    def test_teardown_refuses_unexpected_policy_delta(self):
        self.setup_loaded()
        self.policy.write_text(self.policy.read_text() + 'unrelated-new (enforce)\n')
        result = self.run_script(TEARDOWN)
        self.assertEqual(result.returncode, 1)
        self.assertIn('differs from the pre-setup snapshot', json.loads((self.out / 'teardown.json').read_text())['detail'])
        self.assertIn('unrelated-new (enforce)', self.policy.read_text())  # never touched

    def test_unanticipated_teardown_failures_leave_a_typed_incomplete_record(self):
        journal = self.setup_loaded()
        (self.out / 'setup.json').write_text('{corrupt')
        result = self.run_script(TEARDOWN)
        self.assertNotEqual(result.returncode, 0)
        record = json.loads((self.out / 'teardown.json').read_text())
        self.assertEqual(record['status'], 'incomplete')
        self.assertIn('teardown failed', record['detail'])
        self.assertIn(journal['name'], self.policy.read_text())  # never removed blindly
        (self.out / 'teardown.json').unlink()
        (self.out / 'setup.json').write_text(json.dumps(journal))
        failing = self.bin / 'sudo'
        failing.rename(self.bin / 'sudo-real')
        failing.write_text('#!/bin/sh\ncase "$*" in *--remove*) exit 5;; esac\nexec "$(dirname "$0")/sudo-real" "$@"\n')
        failing.chmod(0o755)
        result = self.run_script(TEARDOWN)
        self.assertEqual(result.returncode, 5)
        record = json.loads((self.out / 'teardown.json').read_text())
        self.assertEqual(record['status'], 'incomplete')
        self.assertIn('exit 5', record['detail'])

    def test_teardown_without_provisioning_or_load_is_explicit(self):
        result = self.run_script(TEARDOWN)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads((self.out / 'teardown.json').read_text())['status'], 'not_provisioned')
        journal = self.setup_loaded()
        rows = [r for r in self.policy.read_text().splitlines() if not r.startswith(journal['name'])]
        self.policy.write_text('\n'.join(rows) + '\n')
        data = json.loads((self.out / 'setup.json').read_text())
        (self.out / 'setup.json').write_text(json.dumps(dict(data, state='rendered')))
        result = self.run_script(TEARDOWN)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.out / 'teardown.json').read_text())['status'], 'not_loaded')
        (self.out / 'setup.json').write_text(json.dumps(dict(data, state='loaded')))
        result = self.run_script(TEARDOWN)
        self.assertEqual(result.returncode, 1)
        self.assertIn('missing before teardown', json.loads((self.out / 'teardown.json').read_text())['detail'])


if __name__ == '__main__':
    unittest.main()
