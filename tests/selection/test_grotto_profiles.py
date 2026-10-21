"""Exact signer-selection projections, not whole-job or native x86_64 acceptance."""
import json
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

from selector_support import ROOT, s
from profile_contract_support import step_script

INVENTORY = ROOT / 'tests/selection/inventory.json'
WORKFLOW = ROOT / '.github/workflows/grotto-ci.yml'
EXPORT = ROOT / 'tests/selection/fixtures/grotto_profile_exports.json'
PROFILES = {'native': 'grotto.linux', 'asan-ubsan': 'grotto.linux.asan_ubsan'}


class GrottoProfiles(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(INVENTORY)
        self.workflow = WORKFLOW.read_text()

    def capture(self, script, mode):
        # Run literal legacy selection/configure shell with recording tools.
        # This is argv evidence only; real Linux exports are checked separately.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            recorder = root / 'cmake'
            recorder.write_text('#!' + sys.executable + '\nimport json,os,sys\n'
                                'with open(os.environ["CAPTURE"], "a") as f: '
                                'f.write(json.dumps(sys.argv[1:])+"\\n")\n')
            recorder.chmod(0o755)
            capture = root / 'commands.jsonl'
            env = dict(os.environ, MODE=mode, PATH=str(root) + os.pathsep + os.environ['PATH'],
                       CAPTURE=str(capture))
            result = subprocess.run(['bash', '-c', script], cwd=root, env=env,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return [json.loads(line) for line in capture.read_text().splitlines()]

    def test_configure_and_default_build_are_literal_workflow_contracts(self):
        for mode, ident in PROFILES.items():
            with self.subTest(mode=mode):
                profile = self.policy['profiles'][ident]
                args, = self.capture(step_script(self.workflow, 'Configure'), mode)
                self.assertEqual(args[:4], ['-S', '.', '-B', 'build'])
                self.assertEqual(args[4:], profile['configure_args'])
                build, = self.capture(step_script(self.workflow, 'Build'), mode)
                self.assertEqual(build, ['--build', 'build', '--parallel', '2'])
                self.assertEqual(profile['workflow_contract']['build_command'],
                                 ['cmake', '--build', '{build}', '--parallel', '2'])
                self.assertEqual(profile['build_parallelism'], 2)
                self.assertEqual(profile['test_parallelism'], 1)
                self.assertIn('Default-all', profile['workflow_contract']['execution_gap'])
                self.assertNotIn('build2/test1 API', profile['workflow_contract']['execution_gap'])
                self.assertEqual(profile['setup'], '.github/workflows/grotto-ci.yml:signer')
                self.assertEqual(profile['workflow_contract']['matrix']['runner'], 'ubuntu-24.04')
                self.assertEqual(profile['capabilities'], ['display', 'private_bus'])
                self.assertFalse(any('MDK' in flag for flag in profile['configure_args']))
                for key, value in profile['environment'].items():
                    self.assertIn('      ' + key + ': ' + value + '\n', self.workflow)

    def test_native_uses_the_entire_fresh_regex_surface_not_only_required_minimum(self):
        exports = json.loads(EXPORT.read_text())
        self.assertEqual(exports['platform'], 'linux')
        self.assertEqual(exports['architecture'], 'aarch64')
        self.assertFalse(exports['authored_x86_64_execution_acceptance'])
        for mode, ident in PROFILES.items():
            profile = self.policy['profiles'][ident]
            exported = exports['variants'][mode]
            primary = exported['primary_names']
            actual = [name for check in self.policy['checks'].values()
                      for name in check.get('ctest_tests', {}).get(ident, [])]
            self.assertEqual(sorted(actual), primary)
            self.assertEqual(len(actual), len(set(actual)))
            self.assertEqual(exported['configure_args'], profile['configure_args'])
            self.assertTrue(set(profile['workflow_contract']['required_minimum']) <= set(actual))
            regex = profile['workflow_contract']['test_command'][5]
            self.assertEqual(sorted(name for name in exported['all_registration_names']
                                    if re.search(regex, name)), primary)
            self.assertEqual(sorted(exported['closure_names']),
                             sorted(primary + exported['fixture_names']))
            for name in primary:
                self.assertIn(name, exported['descriptors'])
                desc = exported['descriptors'][name]
                self.assertTrue(desc['owner'])
                if name not in ('signer/blueprint-drift',):
                    self.assertTrue(exported['target_mapping'][name]['targets'], name)
        native = exports['variants']['native']['primary_names']
        self.assertEqual(len(native), 53)
        self.assertGreater(len(native), 17)
        for alias in ('signer/bunker-startup', 'signer/bunker', 'signer/bunker-headless',
                      'signer/bunker-e2e', 'signer/ui-layout', 'signer/ui-accessibility',
                      'signer/room-flow', 'signer/qr-scanner'):
            self.assertIn(alias, native)
        # These Node tests are registered but the legacy host_ regex does not
        # execute extension_ names. Do not silently claim broader coverage.
        self.assertIn('nostr_signer_webext_extension_policy',
                      exports['variants']['native']['all_registration_names'])
        self.assertNotIn('nostr_signer_webext_extension_policy', native)
        self.assertEqual(len(exports['variants']['asan-ubsan']['primary_names']), 13)

    def test_each_added_check_node_has_a_configured_source_or_descriptor_witness(self):
        exports = json.loads(EXPORT.read_text())
        for mode, suffix in (('native', 'native'), ('asan-ubsan', 'asan_ubsan')):
            exported = exports['variants'][mode]
            witnesses = exported['ownership_witnesses']
            self.assertEqual(set(self.policy['checks']['grotto.signer.' + suffix]['nodes']),
                             set(witnesses) | {'execution.grotto'})
            for node, witness in witnesses.items():
                if witness['kind'] == 'FileAPI_or_runtime':
                    self.assertIn(node, s.classify(self.policy, witness['input'])['nodes'])
                else:
                    self.assertIn(node, exported['descriptors'][witness['test']]['owner'])

    def test_unimplemented_tsan_and_latest_variants_are_not_silently_equated(self):
        record = self.policy['workflow_jobs']['.github/workflows/grotto-ci.yml:signer']
        latest, tsan = record['unimplemented_variants']
        self.assertEqual((latest['runner'], latest['mode'], latest['status']),
                         ('ubuntu-latest', 'native', 'unsupported'))
        self.assertEqual((tsan['runner'], tsan['mode'], tsan['status'], tsan['advisory']),
                         ('ubuntu-24.04', 'tsan', 'unsupported', True))
        args, = self.capture(step_script(self.workflow, 'Configure'), 'tsan')
        self.assertEqual(args[4:], tsan['configure_args'])
        self.assertEqual(tsan['required_names'], ['signer/bunker', 'signer/bunker-e2e',
                         'nip55l_approval_service', 'nip55l_cache_concurrency'])
        self.assertIn("continue-on-error: ${{ matrix.mode == 'tsan' }}", self.workflow)
        remainder = record['unimplemented_native_steps']
        self.assertEqual(remainder['baseline']['status'], 'not_executed')
        self.assertEqual(remainder['shell_extension']['requires'], ['private_bus'])
        self.assertEqual(set(remainder), {'baseline', 'meson', 'debian_package', 'shell_extension'})

    def test_export_source_bindings_require_fresh_registration_evidence(self):
        exports = json.loads(EXPORT.read_text())
        renewal = json.loads((ROOT / 'tests/selection/fixtures/ordinary_subset_source_renewal_20261022.json').read_text())
        for relative, expected in exports['source_sha256'].items():
            self.assertIn('{source}/' + relative,
                          self.policy['checks']['selection.contract']['runtime_inputs'])
            content = (ROOT / relative).read_bytes()
            if relative == 'apps/grotto/CMakeLists.txt':
                # Preserve the historical exports, including ASan; only these two
                # token-guarded metadata blocks differ, not registration logic.
                actual = hashlib.sha256(content).hexdigest()
                change = renewal['profiles']['grotto.linux']['source_input_changes'][relative]
                self.assertEqual(change, {'before': expected, 'after': actual})
                self.assertEqual(self.policy['diagnostic_nonmember_subsets']['grotto.linux']
                                 ['review']['source_inputs'][relative], actual)
                for name in ('org.nostr.Signer', 'org.nostr.Grotto'):
                    call = (f'configure_file(data/{name}.service.in\n'
                            f'               ${{CMAKE_CURRENT_BINARY_DIR}}/{name}.service @ONLY)\n').encode()
                    metadata = ('''if(NOSTRC_TEST_SELECTION_TOKEN)
  include("${CMAKE_SOURCE_DIR}/cmake/NostrcConfigureProvenance.cmake")
  nostrc_record_configure_output("${CMAKE_CURRENT_LIST_FILE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/NAME.service.in"
    "${CMAKE_CURRENT_BINARY_DIR}/NAME.service")
endif()
'''.replace('NAME', name)).encode()
                    self.assertEqual(content.count(call + metadata), 1)
                    content = content.replace(call + metadata, call)
            self.assertEqual(hashlib.sha256(content).hexdigest(), expected, relative)

    def test_build_aggregate_is_not_native_host_packaging_or_ctest_coverage(self):
        check = self.policy['checks']['grotto.build_contract']
        self.assertEqual(check['kind'], 'build_contract')
        self.assertEqual(check['build_targets'], ['signer-tests'])
        self.assertEqual(check['command'], ['cmake', '--build', '{build}', '--target', 'signer-tests'])
        self.assertNotIn('ctest_tests', check)
        self.assertNotIn('grotto.native_host', check['nodes'])
        self.assertNotIn('packaging.grotto', check['nodes'])

    def test_symbols_counterfactuals_and_unsupported_remainder_stay_explicit(self):
        worker = (ROOT / '.github/workflows/dependency-worker.yml').read_text()
        name = 'Matching third-party symbols for narrow GUI LSan exceptions'
        self.assertEqual(step_script(worker, name), step_script(self.workflow, name))
        self.assertIn("inputs.profile == 'grotto.linux.asan_ubsan'", worker)
        runtime = ['dbus-run-session', '--', 'xvfb-run', '-a', '-s', '-screen 0 1280x800x24']
        for ident in PROFILES.values():
            self.assertEqual(self.policy['profiles'][ident]['workflow_contract']['runtime_wrapper'], runtime)
        self.assertIn("wrapper=(dbus-run-session -- xvfb-run -a -s '-screen 0 1280x800x24')", worker)
        self.assertIn("dbus-run-session -- xvfb-run -a -s '-screen 0 1280x800x24'", self.workflow)
        counterfactual = self.policy['checks']['grotto.lsan_counterfactual']
        self.assertEqual(counterfactual['command'], ['python3',
                         '{source}/scripts/test-grotto-lsan-suppressions.py', '{build}/lsan-counterfactuals'])
        self.assertEqual(counterfactual['profiles'], ['grotto.linux.asan_ubsan'])
        self.assertFalse(counterfactual.get('requires'))
        record = self.policy['workflow_jobs']['.github/workflows/grotto-ci.yml:signer']
        self.assertEqual(record['profiles'], list(PROFILES.values()))
        for gap in ('ubuntu-latest', 'TSan', 'advisory', 'Meson', 'deb', 'shell', 'baseline', 'Default-all'):
            self.assertIn(gap, record['unsupported_reason'])
        self.assertNotIn('grotto.linux.tsan', self.policy['profiles'])
        self.assertFalse(self.policy['enforcement_ready'])
        self.assertEqual(set(self.policy['diagnostic_nonmembers']), {'groundhog.linux.sanitizers'})


if __name__ == '__main__':
    unittest.main()
