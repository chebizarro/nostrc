"""Shared sanitizer compatibility contract; fixture parity is not live ASan."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('sanitizer_adapter', ROOT/'scripts/sanitizer-gate-ci.py')
ADAPTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ADAPTER)
PROFILE = 'groundhog.linux.sanitizers'


class SanitizerSharedTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.inventory = Path(self.tmp.name)/'inventory.json'
        self.workflow = ROOT/ADAPTER.WORKFLOW
        self.config = ADAPTER.job_config(self.workflow.read_text(), '{source_root}')
        # A fixture profile supplied through the actual adapter CLI, not a
        # checked-in supported profile or a claim of completed production parity.
        self.policy = json.loads((ROOT/'tests/selection/inventory.json').read_text())
        profile = copy.deepcopy(self.policy['profiles']['groundhog.linux'])
        profile['configure_args'] = self.config['GH_SAN_CONFIGURE']
        profile['environment'] = dict(x.split('=',1) for x in self.config['GH_SAN_ENV'] if not x.startswith(ADAPTER.TESTS_VAR+'='))
        profile['sanitizer_compatibility'] = self.config
        self.policy['profiles'][PROFILE] = profile
        self.policy['checks']['fixture.sanitizer'] = dict(kind='ctest',ctest_tests={PROFILE:self.config['GH_SAN_TESTS']})

    def invoke(self, expected=0):
        self.inventory.write_text(json.dumps(self.policy))
        result = subprocess.run([sys.executable,str(ROOT/'scripts/sanitizer-gate-ci.py'),'config',
            '--workflow',str(self.workflow),'--source-root','/source with spaces',
            '--inventory',str(self.inventory),'--shared-profile',PROFILE,'--json'],capture_output=True,text=True)
        self.assertEqual(result.returncode,expected,result.stderr)
        return result

    def test_full_configuration_parity_and_root_expansion(self):
        observed = json.loads(self.invoke().stdout)
        self.assertEqual(observed,ADAPTER.job_config(self.workflow.read_text(),'/source with spaces'))
        self.assertEqual(set(observed),{'GH_SAN_TESTS','GH_SAN_TARGETS','GH_SAN_CONFIGURE',
            'GH_SAN_CONFIGURE_ENV','GH_SAN_BUILD_ENV','GH_SAN_ENV','GH_SAN_SKIP_PATTERN','GH_SAN_TEST_JOBS'})

    def test_each_phase_and_selection_drift_is_rejected(self):
        original = copy.deepcopy(self.policy)
        for key in self.config:
            with self.subTest(key=key):
                self.policy = copy.deepcopy(original)
                value = self.policy['profiles'][PROFILE]['sanitizer_compatibility'][key]
                if isinstance(value,list): value.append('unexpected')
                else: self.policy['profiles'][PROFILE]['sanitizer_compatibility'][key] += '-changed'
                self.assertIn('sanitizer',self.invoke(2).stderr)

    def test_missing_metadata_no_fallback(self):
        del self.policy['profiles'][PROFILE]['sanitizer_compatibility']
        self.assertIn('missing shared sanitizer metadata',self.invoke(2).stderr)

    def test_duplicate_membership_no_alias_pass(self):
        self.policy['checks']['fixture.duplicate'] = copy.deepcopy(self.policy['checks']['fixture.sanitizer'])
        self.assertIn('exact check membership',self.invoke(2).stderr)

    def test_unknown_path_broadens_and_docs_do_not(self):
        self.assertTrue(ADAPTER.affected(['unknown-future-owner/input.c'],ROOT)[0])
        self.assertFalse(ADAPTER.affected(['docs/prose.md'],ROOT)[0])

    def test_shared_audit_delegates_postbuild_headers_without_fallback(self):
        with mock.patch.object(ADAPTER.subprocess, 'call', return_value=1) as call:
            code = ADAPTER.main(['check-inputs','--source-root','/source','--build-dir','/build',
                                 '--shared-profile',PROFILE])
        self.assertEqual(code,1)
        command = call.call_args.args[0]
        self.assertIn('--after-build',command)
        self.assertIn(PROFILE,command)
        self.assertIn('/source/scripts/test-plan.py',command)

    def test_unreadable_shared_inventory_is_error_not_skip(self):
        result = subprocess.run([sys.executable,str(ROOT/'scripts/sanitizer-gate-ci.py'),'affected',
            '--inventory',str(self.inventory)],input='unknown/input.c\n',text=True,capture_output=True)
        self.assertEqual(result.returncode,2)
        self.assertIn('shared selection failed',result.stderr)


if __name__=='__main__': unittest.main()
