"""Outer-adapter prelaunch refusal tests; no live resource proof is inferred."""
import ast
import copy
import os
from types import SimpleNamespace
import unittest

from selector_support import ROOT


class WorkerHostGuardTests(unittest.TestCase):
    def setUp(self):
        script = (ROOT / 'scripts/test-gate.sh').read_text().split("<<'PY'\n", 1)[1].rsplit('\nPY', 1)[0]
        function = next(n for n in ast.parse(script).body if isinstance(n, ast.FunctionDef) and n.name == 'host_guard')
        self.ns = dict(platform=SimpleNamespace(system=lambda: 'Linux', machine=lambda: 'x86_64'), os=os,
            shutil=SimpleNamespace(which=lambda name: '/usr/bin/' + name),
            subprocess=SimpleNamespace(check_output=lambda argv: b'gcc Free Software Foundation'),
            args=SimpleNamespace(linux_custody='pidns'))
        exec(compile(ast.Module(body=[function], type_ignores=[]), 'host_guard', 'exec'), self.ns)
        self.profile = dict(platform='linux', architecture='x86_64', instrumentation='none', compiler='gcc',
            capabilities=['display', 'private_bus', 'signer_endpoint'],
            configure_args=['-DENABLE_TCP_IPC=ON', '-DNIP55L_TEST_TRUST_ENV=ON',
                            '-DNIP55L_SECRET_BACKEND=libsecret', '-DBUILD_APPS=ON'],
            environment={'NOSTRC_TEST_NIP5F_APPROVAL': 'unidentified-get-public-key-once'})
        self.check = dict(kind='ctest', profiles=['fixture'], requires=['signer_endpoint'],
                          ctest_tests={'fixture': ['test_nip5f_tcp']})

    def guard(self, check=None, profile=None):
        return self.ns['host_guard'](profile or self.profile, ['selected'],
                                    {'checks': {'selected': check or self.check}}, 'fixture')

    def test_exact_profile_is_only_prelaunch_admission_not_readiness(self):
        prefix, env, source_only = self.guard()
        self.assertEqual(prefix, [])
        self.assertFalse(source_only)
        self.assertNotIn('NOSTR_SIGNER_ENDPOINT', env)
        self.assertNotIn('NOSTR_SIGNER_TOKEN', env)

    def test_all_authored_configuration_and_consent_are_required(self):
        for flag in self.profile['configure_args']:
            with self.subTest(flag=flag):
                p = copy.deepcopy(self.profile)
                p['configure_args'].remove(flag)
                with self.assertRaisesRegex(RuntimeError, 'exact authored configuration'):
                    self.guard(profile=p)
        self.profile['environment'].clear()
        with self.assertRaisesRegex(RuntimeError, 'one-shot consent'):
            self.guard()

    def test_no_legacy_ptrace_architecture_alias_or_missing_resource_admission(self):
        for custody in ('legacy', 'ptrace'):
            self.ns['args'].linux_custody = custody
            with self.assertRaisesRegex(RuntimeError, 'pidns custody'):
                self.guard()
        self.ns['args'].linux_custody = 'pidns'
        self.profile['architecture'] = 'amd64'
        with self.assertRaisesRegex(RuntimeError, 'exact native Linux architecture'):
            self.guard()
        self.profile['architecture'] = 'x86_64'
        self.profile['capabilities'].remove('private_bus')
        with self.assertRaisesRegex(RuntimeError, 'namespace-owned'):
            self.guard()

    def test_other_signer_tests_and_commands_still_refuse(self):
        for check in (dict(self.check, ctest_tests={'fixture': ['other_signer']}),
                      dict(self.check, kind='command')):
            with self.assertRaisesRegex(RuntimeError, 'only exact test_nip5f_tcp'):
                self.guard(check=check)

    def test_profile_tool_support_does_not_require_ambient_service_or_block_build(self):
        self.profile['capabilities'].append('gnome_keyring')
        check = dict(kind='build_contract', profiles=['fixture'], requires=[])
        self.guard(check=check)
        check['kind'] = 'command'
        check['requires'] = ['gnome_keyring']
        with self.assertRaisesRegex(RuntimeError, 'throwaway Secret Service custody'):
            self.guard(check=check)


if __name__ == '__main__':
    unittest.main()
