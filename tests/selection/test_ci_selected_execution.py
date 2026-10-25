"""Real CLI/CMake/CTest fixture execution, not application or hosted CI proof."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

from selector_support import ROOT, change, s
from test_workflow_contracts import workflow, job_block

sys.path.insert(0, str(ROOT / '.github/workflows'))
import ci_selection as ci

spec = importlib.util.spec_from_file_location('ci_selected_execution', ROOT / 'scripts/ci-selected-execution.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class SelectedPlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def plan(self, path, mode='affected'):
        return s.make_plan(self.policy, [change(path)], head='a' * 40, context='ci', mode=mode)

    def names(self, plan, profile):
        return runner.test_names(runner.selected_checks(plan, self.policy, profile), profile)

    def test_grotto_does_not_select_its_library_prerequisite_tests(self):
        plan = self.plan('apps/grotto/src/main_app.c')
        names = self.names(plan, 'grotto.linux')
        self.assertIn('signer/approval-controller', names)
        self.assertFalse(any(name.startswith(('Go', 'test_nip44', 'test_event')) for name in names))
        self.assertNotIn('static', ci.project(plan)['families'])
        self.assertNotIn('core', ci.project(plan)['families'])
        self.assertNotIn('libgo', ci.project(plan)['families'])

    def test_isolated_non_mls_groundhog_excludes_mls_reference_perf_and_stress(self):
        plan = self.plan('gnome/groundhog/src/ui/gh-about-dialog.c')
        names = self.names(plan, 'groundhog.linux')
        self.assertIn('groundhog-about-gui', names)
        self.assertFalse(any('mls' in name or 'mdk' in name or 'perf' in name for name in names))
        self.assertFalse(ci.project(plan)['run_reference'])
        extras = runner.extra_steps(plan, 'groundhog.linux', names)
        self.assertFalse(extras['lifetime_stress'])
        self.assertFalse(extras['gtk_perf'])
        self.assertFalse(extras['session_keyring'])

    def test_consumer_flags_are_derived_from_selected_checks_not_profile_membership(self):
        for path in ['gnome/groundhog/src/ui/gh-about-dialog.c',
                     'gnome/groundhog/data/ui/gh-window.blp',
                     'gnome/groundhog/src/mls/gh-mls-service.c']:
            with self.subTest(path=path):
                checks = runner.selected_checks(self.plan(path), self.policy, 'groundhog.linux')
                self.assertFalse(runner.needs_grotto(checks, 'groundhog.linux'))
                self.assertFalse(runner.needs_native_host(checks, 'groundhog.linux'))
        checks = runner.selected_checks(self.plan('libgo/src/channel.c'), self.policy, 'groundhog.linux')
        self.assertTrue(runner.needs_grotto(checks, 'groundhog.linux'))
        self.assertTrue(runner.needs_native_host(checks, 'groundhog.linux'))
        host_only = {'host': {'kind': 'ctest', 'build_targets': ['nostr-signer-webext-host'],
                             'ctest_tests': {'groundhog.linux': ['nostr_signer_webext_host_framing']}}}
        self.assertFalse(runner.needs_grotto(host_only, 'groundhog.linux'))
        self.assertTrue(runner.needs_native_host(host_only, 'groundhog.linux'))

    def test_mls_and_shared_dependencies_include_real_consumers(self):
        plan = self.plan('gnome/groundhog/src/mls/gh-mls-service.c')
        self.assertIn('groundhog-mls-service', self.names(plan, 'groundhog.linux'))
        self.assertIn('groundhog-mls-media', self.names(plan, 'groundhog.linux'))
        self.assertTrue(ci.project(plan)['run_reference'])
        plan = self.plan('libgo/src/channel.c')
        self.assertIn('groundhog-relay', self.names(plan, 'groundhog.linux'))
        self.assertIn('signer/bunker', self.names(plan, 'grotto.linux'))
        self.assertTrue({'core', 'libgo', 'groundhog', 'grotto'} <= set(ci.project(plan)['families']))

    def test_unknown_and_incomplete_dependencies_keep_broad_fallback(self):
        for path in ['unknown/new.c', 'gnome/groundhog/src/unknown-new.c']:
            plan = self.plan(path)
            self.assertTrue(ci.project(plan)['broad'])
            self.assertTrue(ci.project(plan)['reasons'])

    def test_framework_execution_only_when_affected_or_broad(self):
        for path in ['tests/selection/test_ci_selected_execution.py', 'scripts/test_selection.py',
                     'scripts/ci-selected-execution.py']:
            self.assertIn('static', ci.project(self.plan(path))['families'])
        self.assertIn('static', ci.project(self.plan('docs/note.md', mode='broad'))['families'])
        self.assertNotIn('static', ci.project(self.plan('docs/note.md'))['families'])
        text = workflow('ci-selective')
        self.assertNotIn('unittest', job_block(text, 'cheap'))
        self.assertIn("contains(fromJSON(needs.select.outputs.families), 'static')", job_block(text, 'static'))
        self.assertIn('test-selection-operational.py', workflow('static-checks'))
        self.assertIn('"test_ci_selected_execution",', (ROOT / 'scripts/test-selection-operational.py').read_text())

    def test_ordinary_sanitizer_parser_retains_broad_recipe_and_affected_job_is_separate(self):
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/sanitizer-gate-ci.py'),
                                 'config', '--source-root', str(ROOT), '--json'],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 0, result.stderr)
        config = json.loads(result.stdout)
        self.assertIn('groundhog-identity', config['GH_SAN_TESTS'])
        self.assertIn('-DGNOSTR_ENABLE_ASAN=ON', config['GH_SAN_CONFIGURE'])
        self.assertIn('-DGNOSTR_ENABLE_UBSAN=ON', config['GH_SAN_CONFIGURE'])
        text = workflow('groundhog-ci')
        broad = job_block(text, 'groundhog-sanitizers')
        selected = job_block(text, 'groundhog-selected-sanitizers')
        self.assertIn('if: ${{ !inputs.affected }}', broad)
        self.assertNotIn('ci-selected-execution.py run', broad)
        self.assertIn('if: inputs.affected', selected)
        self.assertIn('ci-selected-execution.py run --profile groundhog.linux.sanitizers', selected)
        self.assertIn("affected: ${{ needs.select.outputs.broad == 'false' }}",
                      job_block(workflow('ci-selective'), 'groundhog'))

    def test_saved_plan_handoff_and_standalone_broad_wiring(self):
        text = workflow('ci-selective')
        for family in ['grotto', 'groundhog']:
            self.assertIn('selection_artifact: ${{ needs.select.outputs.selection_artifact }}',
                          job_block(text, family))
            child = workflow(family + '-ci')
            self.assertIn("if: inputs.selection_artifact != ''", child)
            self.assertIn('ci-selected-execution.py prepare', child)
            self.assertIn('ci-selected-execution.py run', child)
            self.assertIn("echo 'affected=false'", child)
            route = 'gate' if family == 'grotto' else 'selection'
            self.assertIn(f"steps.{route}.outputs.affected != 'true'", child)
        self.assertIn('--plan "$RUNNER_TEMP/ci-selection/plan.json"', text)


@unittest.skipUnless(shutil.which('cmake') and shutil.which('ctest'), 'CMake/CTest unavailable')
class PhysicalRunnerFixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ci-selected-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.build = self.directory / 'build'
        self.marker = self.directory / 'executed.txt'
        self.script = self.directory / 'record.py'
        self.script.write_text('import pathlib, sys\n'
                               'with pathlib.Path(sys.argv[1]).open("a") as out: out.write(sys.argv[2]+"\\n")\n'
                               'sys.exit(int(sys.argv[3]) if len(sys.argv)>3 else 0)\n')

    def configure(self, names, *, failure=None, skipped=None, disabled=None, targets=()):
        lines = ['cmake_minimum_required(VERSION 3.20)', 'project(selected_fixture NONE)', 'enable_testing()',
                 f'add_custom_target(shared_prerequisite ALL COMMAND "{sys.executable}" "{self.script}" "{self.marker}" built-prerequisite)']
        for target in sorted(set(targets) - {'shared_prerequisite'}):
            lines.append(f'add_custom_target("{target}")')
            lines.append(f'add_dependencies("{target}" shared_prerequisite)')
        for name in [*names, 'unrelated.libnostr', 'unrelated.libgo', 'fixture.setup', 'fixture.cleanup']:
            code = 9 if name == failure else 77 if name == skipped else 0
            lines.append(f'add_test(NAME "{name}" COMMAND "{sys.executable}" "{self.script}" "{self.marker}" "{name}" {code})')
        if names:
            lines.append(f'set_tests_properties("{names[0]}" PROPERTIES FIXTURES_REQUIRED selected_home)')
        lines += ['set_tests_properties(fixture.setup PROPERTIES FIXTURES_SETUP selected_home)',
                  'set_tests_properties(fixture.cleanup PROPERTIES FIXTURES_CLEANUP selected_home)']
        if skipped:
            lines.append(f'set_tests_properties("{skipped}" PROPERTIES SKIP_RETURN_CODE 77)')
        if disabled:
            lines.append(f'set_tests_properties("{disabled}" PROPERTIES DISABLED TRUE)')
        (self.directory / 'CMakeLists.txt').write_text('\n'.join(lines) + '\n')
        subprocess.run(['cmake', '-S', str(self.directory), '-B', str(self.build)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_production_cli_uses_shared_groundhog_selection_and_builds_without_prerequisite_tests(self):
        sha = s.commit_sha(ROOT, 'HEAD')
        plan = s.make_plan(self.policy, [change('gnome/groundhog/src/ui/gh-about-dialog.c')],
                           head=sha, context='ci')
        checks = runner.selected_checks(plan, self.policy, 'groundhog.linux')
        names = runner.test_names(checks, 'groundhog.linux')
        targets = {target for check in checks.values() for target in check.get('build_targets', [])}
        self.configure(names, targets=targets)
        saved = self.directory / 'plan.json'
        saved.write_text(json.dumps({'shared_plan': plan, 'dispatch': ci.project(plan)}))
        command = [sys.executable, str(ROOT / 'scripts/ci-selected-execution.py')]
        env = dict(os.environ, GITHUB_OUTPUT=str(self.directory / 'outputs'))
        subprocess.run(command + ['prepare', '--plan', str(saved), '--profile', 'groundhog.linux'],
                       check=True, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertIn('affected=true', (self.directory / 'outputs').read_text())
        result = subprocess.run(command + ['run', '--plan', str(saved), '--profile', 'groundhog.linux',
                                          '--build', str(self.build)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)
        observed = set(self.marker.read_text().splitlines())
        self.assertTrue(set(names) | {'fixture.setup', 'fixture.cleanup', 'built-prerequisite'} <= observed)
        self.assertNotIn('unrelated.libnostr', observed)
        self.assertNotIn('unrelated.libgo', observed)
        self.assertTrue((self.build / 'ci-selected.xml').is_file())
        # Same production loader refuses edited plan identity and dispatch.
        saved_data = json.loads(saved.read_text())
        saved_data['dispatch']['sha'] = 'b' * 40
        saved.write_text(json.dumps(saved_data))
        with self.assertRaisesRegex(s.ContractError, 'dispatch differs'):
            runner.load_handoff(saved)

    def test_production_grotto_cli_runs_commands_and_exact_tests_not_library_suites(self):
        sha = s.commit_sha(ROOT, 'HEAD')
        plan = s.make_plan(self.policy, [change('apps/grotto/src/main_app.c')], head=sha, context='ci')
        checks = runner.selected_checks(plan, self.policy, 'grotto.linux')
        names = runner.test_names(checks, 'grotto.linux')
        targets = {target for check in checks.values() for target in check.get('build_targets', [])}
        self.configure(names, targets=targets)
        saved = self.directory / 'plan.json'
        saved.write_text(json.dumps({'shared_plan': plan, 'dispatch': ci.project(plan)}))
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/ci-selected-execution.py'),
                                 'run', '--plan', str(saved), '--profile', 'grotto.linux',
                                 '--build', str(self.build)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('grotto.package_contract.native:', result.stdout)
        self.assertIn('grotto.quarantine_contract.native:', result.stdout)
        observed = set(self.marker.read_text().splitlines())
        self.assertTrue(set(names) <= observed)
        self.assertNotIn('unrelated.libnostr', observed)
        self.assertNotIn('unrelated.libgo', observed)

    def test_actual_configure_shell_enables_only_selected_consumers(self):
        # Run the production YAML's configure shell with production prepare output
        # against a real CMake fixture. No apt packages or product builds are faked
        # as accepted here: the fixture only proves the configure flag seam.
        step = workflow('groundhog-ci').split('- name: Configure Groundhog and in-tree adapters\n', 1)[1]
        script = textwrap.dedent(step.split('run: |\n', 1)[1].split('\n      - ', 1)[0])
        prerequisites = workflow('groundhog-ci').split('- name: Install prerequisites for selected Grotto consumers\n', 1)[1].split('\n      - ', 1)[0]
        self.assertIn("steps.selection.outputs.needs_grotto == 'true'", prerequisites)
        self.assertIn('libzbar-dev', prerequisites)
        self.assertIn('blueprint-compiler-0.22.2.tar.xz', prerequisites)
        fixture = '''cmake_minimum_required(VERSION 3.20)
project(configure_seam NONE)
enable_testing()
option(BUILD_APPS "Build app subtree" ON)
option(BUILD_SIGNER_TESTS "Build signer tests" OFF)
option(BUILD_NATIVE_HOST "Build native host" OFF)
option(ENABLE_NOSTR_SIGNER_WEBEXT_HOST "Build standalone native host" OFF)
if(BUILD_APPS)
  if(NOT "$ENV{GROTTO_PREREQUISITES_READY}" STREQUAL "yes")
    message(FATAL_ERROR "Unrelated Grotto requires unavailable prerequisites")
  endif()
  if(BUILD_SIGNER_TESTS)
    add_test(NAME signer/fixture COMMAND "${CMAKE_COMMAND}" -E true)
  endif()
endif()
add_test(NAME groundhog-fixture COMMAND "${CMAKE_COMMAND}" -E true)
'''
        for path, expected in [('gnome/groundhog/src/ui/gh-about-dialog.c', False),
                               ('gnome/groundhog/data/ui/gh-window.blp', False),
                               ('nips/nip55l/src/core/signer_ops.c', True)]:
            with self.subTest(path=path):
                case = self.directory / ('shared' if expected else Path(path).name)
                case.mkdir()
                (case / 'CMakeLists.txt').write_text(fixture)
                sha = s.commit_sha(ROOT, 'HEAD')
                changes = [change(path)]
                if expected:
                    changes.append(change('gnome/groundhog/src/ui/gh-about-dialog.c'))
                plan = s.make_plan(self.policy, changes, head=sha, context='ci')
                self.assertFalse(ci.project(plan)['broad'])
                saved = case / 'plan.json'
                saved.write_text(json.dumps({'shared_plan': plan, 'dispatch': ci.project(plan)}))
                output = case / 'outputs'
                subprocess.run([sys.executable, str(ROOT / 'scripts/ci-selected-execution.py'),
                                'prepare', '--profile', 'groundhog.linux', '--plan', str(saved)],
                               env=dict(os.environ, GITHUB_OUTPUT=str(output)), check=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                outputs = dict(line.split('=', 1) for line in output.read_text().splitlines())
                self.assertEqual(outputs['needs_grotto'], str(expected).lower())
                resolved = script
                for key, value in outputs.items():
                    resolved = resolved.replace('${{ steps.selection.outputs.' + key + ' }}', value)
                environment = dict(os.environ)
                environment.pop('GROTTO_PREREQUISITES_READY', None)
                if expected:
                    environment['GROTTO_PREREQUISITES_READY'] = 'yes'
                result = subprocess.run(['bash', '-euo', 'pipefail', '-c', resolved], cwd=case,
                                        env=environment, text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT)
                self.assertEqual(result.returncode, 0, result.stdout)
                cache = (case / 'build/CMakeCache.txt').read_text()
                self.assertIn('BUILD_APPS:BOOL=' + ('ON' if expected else 'OFF'), cache)
                self.assertIn('BUILD_SIGNER_TESTS:BOOL=' + ('ON' if expected else 'OFF'), cache)
                self.assertIn('BUILD_NATIVE_HOST:BOOL=' + ('ON' if expected else 'OFF'), cache)
                registrations = json.loads(subprocess.check_output(
                    ['ctest', '--test-dir', str(case / 'build'), '--show-only=json-v1'], text=True))
                self.assertEqual('signer/fixture' in {t['name'] for t in registrations['tests']}, expected)

    def test_exact_names_escape_regex_and_ctest_supplies_fixture_closure(self):
        names = ['selected.with+[regex](characters)']
        self.configure(names)
        runner.execute_tests(self.build, names)
        self.assertEqual(set(self.marker.read_text().splitlines()),
                         set(names) | {'fixture.setup', 'fixture.cleanup'})

    def test_missing_selected_registration_refuses_before_execution(self):
        self.configure(['present'])
        with self.assertRaisesRegex(s.ContractError, 'Missing selected CTest'):
            runner.execute_tests(self.build, ['absent'])
        self.assertFalse(self.marker.exists())

    def test_nonzero_ctest_and_fixture_failure_preserve_failure_logs(self):
        for failure in ['selected', 'fixture.setup']:
            with self.subTest(failure=failure):
                self.configure(['selected'], failure=failure)
                with self.assertRaisesRegex(s.ContractError, 'CTest failed'):
                    runner.execute_tests(self.build, ['selected'])
                self.assertTrue((self.build / 'ci-selected.log').is_file())
                self.assertTrue((self.build / 'ci-selected.xml').is_file())

    def test_skipped_and_disabled_checks_cannot_report_success(self):
        for mode in ['skipped', 'disabled']:
            with self.subTest(mode=mode):
                self.configure(['selected'], **{mode: 'selected'})
                with self.assertRaises(s.ContractError):
                    runner.execute_tests(self.build, ['selected'])

    def test_selected_command_runs_and_nonzero_exit_is_not_swallowed(self):
        checks = {'selected.command': {'kind': 'command', 'build_targets': [],
                   'command': [sys.executable, str(self.script), str(self.marker), 'command', '0']}}
        runner.execute_commands(checks, ROOT, self.build)
        self.assertEqual(self.marker.read_text(), 'command\n')
        checks['selected.command']['command'][-1] = '7'
        with self.assertRaises(subprocess.CalledProcessError) as error:
            runner.execute_commands(checks, ROOT, self.build)
        self.assertEqual(error.exception.returncode, 7)


if __name__ == '__main__':
    unittest.main()
