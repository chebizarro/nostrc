"""Representative production runner selection; not hosted/native execution proof."""
import copy
import json
import os
import re
import sys
import tempfile
import textwrap
from pathlib import Path
import unittest
from unittest.mock import patch

from selector_support import GitCase, ROOT, change, s
from test_workflow_contracts import workflow, job_block

sys.path.insert(0, str(ROOT / '.github/workflows'))
import ci_selection as ci


class RepresentativeSelectionTests(unittest.TestCase):
    def select(self, paths=(), mode='affected'):
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        return ci.project(s.make_plan(policy, [change(path) for path in paths],
                                     head='a' * 40, context='ci', mode=mode))

    def test_groundhog_ui_omits_reference_mdk_core_and_libgo(self):
        self.assertTrue((ROOT / 'gnome/groundhog/data/ui/gh-window.blp').is_file())
        result = self.select(['gnome/groundhog/data/ui/gh-window.blp'])
        self.assertFalse(result['broad'])
        self.assertEqual(result['families'], ['groundhog'])
        self.assertFalse(result['run_reference'])

    def test_groundhog_mls_includes_both_reference_families_not_unrelated_core(self):
        self.assertTrue((ROOT / 'gnome/groundhog/src/mls/gh-mls-service.c').is_file())
        result = self.select(['gnome/groundhog/src/mls/gh-mls-service.c'])
        self.assertFalse(result['broad'])
        self.assertEqual(result['families'], ['groundhog', 'mdk011', 'mdk08'])
        self.assertTrue(result['run_reference'])

    def test_grotto_only_does_not_select_prerequisite_core_or_libgo_tests(self):
        self.assertTrue((ROOT / 'apps/grotto/src/main_app.c').is_file())
        result = self.select(['apps/grotto/src/main_app.c'])
        self.assertFalse(result['broad'])
        self.assertEqual(result['families'], ['grotto', 'windows'])
        self.assertFalse(result['run_reference'])
        # Broad/advisory paths remain; ordinary PR execution uses test-gate.
        text = workflow('grotto-ci')
        self.assertIn('ctest --test-dir build -j1 -R "$regex" --no-tests=error', text)
        self.assertIn("regex='^(signer/|nip55l_|nostr_signer_webext_host_)'", text)
        self.assertIn('ci-selected-execution.py run', text)  # advisory TSan
        self.assertIn('scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" --profile "$profile"', text)
        self.assertNotIn('ctest --test-dir build --output-on-failure\n', text)

    def test_grotto_rejects_vacuous_plan_at_the_production_workflow_boundary(self):
        text = workflow('grotto-ci')
        block = text.split('      - name: Reject vacuous Grotto plans\n', 1)[1].split('      - name:', 1)[0]
        script = textwrap.dedent(block.split('        run: |\n', 1)[1])
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        fixture = s.read_json(ROOT / 'tests/selection/fixtures/nip55l_affected.json')
        for path in ('apps/grotto/src/main_app.c', fixture['signer_path'],
                     'nostr-gobject/src/nostr_relay.c', 'apps/grotto/README.md'):
            with self.subTest(path=path), tempfile.TemporaryDirectory() as tmp:
                plan_path = Path(tmp) / 'grotto-plan/run/0/plan.json'
                plan_path.parent.mkdir(parents=True)
                plan = s.make_plan(policy, [change(path)], head='a' * 40)
                if path == fixture['signer_path']:
                    self.assertEqual(plan['selected_checks'], fixture['expected_signer_checks'])
                plan_path.write_text(json.dumps(plan))
                with patch.dict(os.environ, RUNNER_TEMP=tmp):
                    exec(compile(script, 'grotto-ci.yml:Reject vacuous Grotto plans', 'exec'), {})
                    # Deletion/rename source paths count too; zero checks must fail.
                    for side in ('old_path', 'new_path'):
                        plan['changes'] = [{side: path}]
                        plan['selected_checks'] = []
                        plan_path.write_text(json.dumps(plan))
                        with self.assertRaisesRegex(SystemExit, 'zero checks'):
                            exec(compile(script, 'grotto-ci.yml:Reject vacuous Grotto plans', 'exec'), {})

    def test_grotto_plan_is_uploaded_before_affected_execution_and_guards_remain(self):
        text = workflow('grotto-ci')
        self.assertLess(text.index('Upload plan before execution'), text.index('Execute affected Grotto profile'))
        self.assertIn('fetch-depth: 0', text)
        self.assertIn('base=$(git merge-base "${PR_BASE:-$QUEUE_BASE}" "$head")', text)
        self.assertIn('pull_request|merge_group)', text)
        self.assertIn('push|release|workflow_dispatch|workflow_call)', text)
        self.assertIn('args+=(--broad --profile "$profile")', text)
        self.assertIn('refs/heads/main|refs/heads/master|refs/tags/*)', text)
        self.assertIn('grotto.linux.asan_ubsan', text)
        self.assertIn('ADVISORY, non-gating', text)
        self.assertIn('nostrc-u6v55.14.4', text)
        self.assertIn('scripts/grotto-ci-quarantine.tsv', text)
        self.assertIn('SANITIZER_REPORTS: block', text)
        self.assertIn('detect_leaks=1:halt_on_error=1', text)

    def test_mdk09_probe_selects_the_existing_mdk011_probe_runner_not_mdk08(self):
        path = 'tests/interop/mdk/driver-0.9-probe/src/main.rs'
        self.assertTrue((ROOT / path).is_file())
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        shared = s.make_plan(policy, [change(path)], head='a' * 40, context='ci')
        self.assertIn('mdk011.mdk09_probe', shared['selected_checks'])
        dispatch = ci.project(shared)
        self.assertFalse(dispatch['broad'])
        self.assertEqual(dispatch['families'], ['mdk011'])
        call = job_block(workflow('ci-selective'), 'mdk011')
        self.assertIn('uses: ./.github/workflows/marmot-mdk011-interop.yml', call)
        runner = workflow('marmot-mdk011-interop')
        self.assertIn('-DBUILD_MDK011_INTEROP=ON', runner)
        self.assertIn("ctest --test-dir build -R '^groundhog-mdk011-interop'", runner)
        self.assertNotIn('-DBUILD_MDK09_PROBE=OFF', runner)
        cmake = (ROOT / 'gnome/groundhog/CMakeLists.txt').read_text()
        self.assertIn('option(BUILD_MDK09_PROBE "Register the MDK 0.9 expected-incompatible probe" ON)', cmake)
        self.assertIn('list(APPEND groundhog_mdk011_cases mdk09-probe)', cmake)

    def test_fuzz_harness_sources_have_shared_ownership_and_select_only_fuzz(self):
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        cmake = (ROOT / 'apps/grotto/tests/fuzz/CMakeLists.txt').read_text()
        for target, filename in [('nip49', 'fuzz_nip49.c'), ('bip39', 'fuzz_bip39.c'),
                                 ('json-event', 'fuzz_json_event.c')]:
            path = 'apps/grotto/tests/fuzz/' + filename
            with self.subTest(path=path):
                self.assertTrue((ROOT / path).is_file())
                self.assertIn(f'add_fuzz_target(fuzz-{target} {filename}', cmake)
                self.assertEqual(s.classify(policy, path)['nodes'], ['fuzz.harness_inputs'])
                shared = s.make_plan(policy, [change(path)], head='a' * 40, context='ci')
                dispatch = ci.project(shared)
                self.assertFalse(dispatch['broad'])
                self.assertEqual(dispatch['families'], ['fuzz'])
                self.assertFalse(dispatch['run_reference'])
        call = job_block(workflow('ci-selective'), 'fuzz')
        self.assertIn('uses: ./.github/workflows/grotto-fuzz.yml', call)
        runner = workflow('grotto-fuzz')
        self.assertIn("TARGET: ${{ inputs.target || 'all' }}", runner)
        self.assertIn('cmake --build build --target fuzz-${{ matrix.target }}', runner)
        self.assertIn('FUZZ_BIN="build/apps/grotto/tests/fuzz/fuzz-${{ matrix.target }}"', runner)

    def test_libgo_includes_own_tests_and_downstream_consumers(self):
        self.assertTrue((ROOT / 'libgo/src/channel.c').is_file())
        result = self.select(['libgo/src/channel.c'])
        self.assertTrue({'libgo', 'core', 'grotto', 'groundhog', 'mdk08', 'mdk011'}
                        <= set(result['families']))

    def test_unknown_path_and_unmapped_ownership_broaden_with_explanation(self):
        result = self.select(['new-unclassified-engine/file.c'])
        self.assertTrue(result['broad'])
        self.assertEqual(set(result['families']), set(ci.BROAD_FAMILIES))
        self.assertTrue(any('unclassified' in reason for reason in result['reasons']))
        result = self.select(['gnome/nostr-dav/src/main.c'])
        self.assertTrue(result['broad'])
        self.assertTrue(result['reasons'])

    def test_explicit_full_and_empty_selection(self):
        result = self.select(mode='broad')
        self.assertTrue(result['broad'])
        self.assertEqual(set(result['families']), set(ci.BROAD_FAMILIES))
        self.assertTrue(result['run_reference'])
        result = self.select()
        self.assertFalse(result['broad'])
        self.assertEqual(result['families'], [])

    def test_full_label_and_explicit_request(self):
        event = {'pull_request': {'base': {'sha': 'b' * 40}, 'head': {'sha': 'c' * 40},
                                  'labels': [{'name': 'ci:full'}]}}
        metadata, mode, _ = ci.event_contract('pull_request', event, 'a' * 40)
        self.assertEqual(mode, 'broad')
        self.assertEqual(metadata['pr_head_sha'], 'c' * 40)
        event['pull_request']['labels'] = []
        self.assertEqual(ci.event_contract('pull_request', event, 'a' * 40)[1], 'affected')
        self.assertEqual(ci.event_contract('pull_request', event, 'a' * 40, full=True)[1], 'broad')

    def test_master_release_component_tag_and_manual_always_broad(self):
        for name, event in [('push', {'ref': 'refs/heads/master'}),
                            ('push', {'ref': 'refs/heads/main'}),
                            ('push', {'ref': 'refs/tags/v1.2.3'}),
                            ('push', {'ref': 'refs/tags/groundhog-v0.12.0-alpha7'}),
                            ('release', {'action': 'published'}),
                            ('workflow_dispatch', {'inputs': {'mode': 'affected'}}),
                            ('workflow_call', {})]:
            with self.subTest(name=name, event=event):
                metadata, mode, context = ci.event_contract(name, event, 'a' * 40)
                self.assertEqual((mode, context), ('broad', 'ci'))
                policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
                result = ci.project(s.make_plan(policy, [], head='a' * 40,
                                               context=context, mode=mode, event=metadata))
                self.assertEqual(set(result['families']), set(ci.BROAD_FAMILIES))

    def test_merge_queue_uses_immutable_base_and_tested_head(self):
        event = {'merge_group': {'base_sha': 'b' * 40, 'head_sha': 'a' * 40}}
        metadata, mode, _ = ci.event_contract('merge_group', event, 'a' * 40)
        self.assertEqual(mode, 'affected')
        self.assertEqual(metadata['base_sha'], 'b' * 40)
        with self.assertRaises(s.ContractError):
            ci.event_contract('merge_group', event, 'c' * 40)

    def test_no_deleted_ref_or_privileged_event_admission(self):
        for name, event in [('push', {'deleted': True}), ('pull_request_target', {}),
                            ('workflow_run', {})]:
            with self.subTest(name=name), self.assertRaises(s.ContractError):
                ci.event_contract(name, event, 'a' * 40)


class RealHistoryTests(GitCase):
    def setUp(self):
        super().setUp()
        self.p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.write('tests/selection/inventory.json', s.canonical(self.p).decode())
        self.write('apps/grotto/src/main_app.c', 'base\n')
        self.write('libgo/src/channel.c', 'base\n')
        self.base = self.commit()

    def test_divergent_fork_head_and_tested_merge_use_shared_history_union(self):
        target = self.git('branch', '--show-current')
        self.git('checkout', '-qb', 'fork-topic')
        self.write('apps/grotto/src/main_app.c', 'PR change\n')
        pr_head = self.commit()
        self.git('checkout', target)
        self.write('docs/notes.md', 'target advanced\n')
        base_tip = self.commit()
        self.git('merge', '--no-ff', '-qm', 'tested merge', 'fork-topic')
        tested = self.git('rev-parse', 'HEAD')
        event = {'pull_request': {'base': {'sha': base_tip}, 'head': {'sha': pr_head}}}
        metadata, mode, context = ci.event_contract('pull_request', event, tested)
        shared = s.git_plan(self.repo, self.p, head=tested, event=metadata, context=context, mode=mode)
        self.assertEqual(shared['tested_sha'], tested)
        self.assertEqual(ci.project(shared)['families'], ['grotto', 'windows'])

    def test_unavailable_base_broadens_instead_of_empty_green(self):
        self.write('apps/grotto/src/main_app.c', 'PR\n')
        head = self.commit()
        metadata = {'kind': 'pull_request', 'base_sha': 'f' * 40,
                    'pr_head_sha': head, 'tested_sha': head}
        shared = s.git_plan(self.repo, self.p, head=head, event=metadata, context='ci')
        result = ci.project(shared)
        self.assertTrue(result['broad'])
        self.assertTrue(any('range unavailable' in reason for reason in result['reasons']))
        self.assertEqual(set(result['families']), set(ci.BROAD_FAMILIES))

    def test_each_fuzz_harness_history_change_reaches_only_the_fuzz_runner(self):
        for filename in ['fuzz_nip49.c', 'fuzz_bip39.c', 'fuzz_json_event.c']:
            with self.subTest(filename=filename):
                before = self.git('rev-parse', 'HEAD')
                self.write('apps/grotto/tests/fuzz/' + filename, 'harness change\n')
                self.commit()
                shared = self.select(base=before, context='ci')
                self.assertEqual(set(shared['witnesses']), {'fuzz.harness_inputs'})
                dispatch = ci.project(shared)
                self.assertFalse(dispatch['broad'])
                self.assertEqual(dispatch['families'], ['fuzz'])

    def test_rename_retains_old_library_ownership(self):
        self.git('mv', 'libgo/src/channel.c', 'apps/grotto/src/moved.c')
        self.commit()
        result = ci.project(self.select(context='ci'))
        self.assertTrue({'core', 'libgo', 'grotto'} <= set(result['families']))

    def test_cli_select_and_aggregate_bind_same_plan_and_preserve_failure_report(self):
        self.write('apps/grotto/src/main_app.c', 'PR\n')
        head = self.commit()
        event = {'pull_request': {'base': {'sha': self.base}, 'head': {'sha': head}}}
        event_path = self.repo / 'event.json'
        event_path.write_text(json.dumps(event))
        plan_path = self.repo / 'output/plan.json'
        conclusions = self.repo / 'output/conclusions.json'
        env = {'GITHUB_EVENT_PATH': str(event_path), 'GITHUB_EVENT_NAME': 'pull_request',
               'GITHUB_OUTPUT': str(self.repo / 'outputs'),
               'GITHUB_STEP_SUMMARY': str(self.repo / 'summary')}
        with patch.object(ci, 'ROOT', self.repo), patch.dict(os.environ, env):
            with patch.object(sys, 'argv', ['ci_selection.py', 'select', '--sha', head,
                                           '--plan', str(plan_path)]):
                ci.main()
            dispatch = json.loads(plan_path.read_text())['dispatch']
            needs = successful_needs(dispatch)
            needs['grotto']['result'] = 'failure'
            needs_path = self.repo / 'needs.json'
            needs_path.write_text(json.dumps(needs))
            argv = ['ci_selection.py', 'aggregate', '--sha', head, '--plan', str(plan_path),
                    '--needs', str(needs_path), '--output', str(conclusions)]
            with patch.object(sys, 'argv', argv), self.assertRaisesRegex(s.ContractError, 'grotto:'):
                ci.main()
            self.assertFalse(json.loads(conclusions.read_text())['passed'])
            self.assertIn('grotto: failure', (self.repo / 'summary').read_text())
            saved = json.loads(plan_path.read_text())
            saved['dispatch']['families'] = []
            plan_path.write_text(json.dumps(saved))
            with patch.object(sys, 'argv', argv), self.assertRaisesRegex(s.ContractError, 'bound shared plan'):
                ci.main()


def successful_needs(dispatch):
    result = {family: {'result': 'success' if family in dispatch['families'] else 'skipped'}
              for family in ci.BROAD_FAMILIES}
    result.update(select={'result': 'success'}, cheap={'result': 'success'})
    return result


class RequiredAggregateTests(unittest.TestCase):
    def setUp(self):
        self.dispatch = RepresentativeSelectionTests().select(['apps/grotto/src/main_app.c'])
        self.needs = successful_needs(self.dispatch)

    def test_selected_skipped_failed_cancelled_missing_and_planner_or_guard_failures_are_red(self):
        self.assertTrue(ci.aggregate(self.dispatch, self.needs)['passed'])
        for job in ['grotto', 'windows', 'select', 'cheap']:
            for status in ['skipped', 'failure', 'cancelled', 'missing']:
                needs = copy.deepcopy(self.needs)
                if status == 'missing':
                    del needs[job]
                else:
                    needs[job]['result'] = status
                with self.subTest(job=job, status=status):
                    self.assertFalse(ci.aggregate(self.dispatch, needs)['passed'])

    def test_omission_is_literal_and_unexpected_execution_is_not_accepted(self):
        result = ci.aggregate(self.dispatch, self.needs)
        self.assertEqual(result['families']['libgo']['coverage'], 'not_executed')
        self.needs['libgo']['result'] = 'success'
        self.assertFalse(ci.aggregate(self.dispatch, self.needs)['passed'])
        self.needs = successful_needs(self.dispatch)
        self.needs['unexpected'] = {'result': 'success'}
        self.assertFalse(ci.aggregate(self.dispatch, self.needs)['passed'])

    def test_broad_needs_every_family(self):
        dispatch = RepresentativeSelectionTests().select(mode='broad')
        needs = successful_needs(dispatch)
        self.assertTrue(ci.aggregate(dispatch, needs)['passed'])
        for family in ci.BROAD_FAMILIES:
            bad = copy.deepcopy(needs)
            bad[family]['result'] = 'skipped'
            with self.subTest(family=family):
                self.assertFalse(ci.aggregate(dispatch, bad)['passed'])


class DefaultWorkflowWiringTests(unittest.TestCase):
    def test_unfiltered_read_only_entrypoint_and_stable_always_aggregate(self):
        text = workflow('ci-selective')
        header = text.split('jobs:', 1)[0]
        for event in ['pull_request:', 'merge_group:', 'push:', 'release:', 'workflow_dispatch:']:
            self.assertIn(event, header)
        self.assertIn('branches: [main, master]', header)
        self.assertIn("tags: ['v*', '**-v*']", header)
        for forbidden in ['paths:', 'paths-ignore:', 'pull_request_target', 'workflow_run',
                          'secrets:', 'contents: write', 'actions: write']:
            self.assertNotIn(forbidden, text)
        gate = job_block(text, 'gate')
        self.assertIn('name: Dependency gate', gate)
        self.assertIn('if: always()', gate)
        needs = set(re.search(r'needs: \[(.*?)\]', gate).group(1).split(', '))
        self.assertEqual(needs, {*ci.BROAD_FAMILIES, 'cheap', 'select'})
        self.assertIn('ci_selection.py aggregate', gate)
        self.assertIn("if: always() && needs.select.result == 'success'", gate)
        self.assertNotIn('continue-on-error', gate)
        self.assertNotIn('run-id:', gate)
        self.assertNotIn('github-token:', gate)

    def test_every_family_calls_real_legacy_runner_at_tested_commit(self):
        text = workflow('ci-selective')
        for family, runner in ci.BROAD_FAMILIES.items():
            with self.subTest(family=family):
                block = job_block(text, family)
                self.assertIn(f"contains(fromJSON(needs.select.outputs.families), '{family}')", block)
                self.assertIn(f'uses: ./.github/workflows/{runner}.yml', block)
                self.assertIn('sha: ${{ needs.select.outputs.sha }}', block)
                self.assertNotIn('dependency-worker', block)
                self.assertNotIn('continue-on-error', block)
        groundhog = job_block(text, 'groundhog')
        self.assertIn("run_reference: ${{ needs.select.outputs.run_reference == 'true' }}", groundhog)
        reference = workflow('groundhog-ci').split('- name: White Noise AAC/MP4 voice interop with MDK 0.11', 1)[1]
        self.assertIn("if: inputs.sha == '' || inputs.run_reference", reference.split('- name:', 1)[0])
        self.assertIn('uses: ./.github/workflows/static-checks.yml', job_block(text, 'static'))
        self.assertNotIn('full_static:', text)
        self.assertNotIn('namespace-policy-regression.yml', text)
        self.assertNotIn('dependency_contract', (ROOT / '.github/workflows/ci_selection.py').read_text())

    def test_manual_publishers_run_broad_and_do_not_promote_family_conclusions_to_attestations(self):
        for name in ['release', 'groundhog-release', 'windows-build']:
            text = workflow(name)
            call = job_block(text, 'manual-broad')
            self.assertIn("if: github.event_name == 'workflow_dispatch'", call)
            self.assertIn('uses: ./.github/workflows/ci-selective.yml', call)
            self.assertIn('full: true', call)
            self.assertIn('uses: ./.github/workflows/release-validation.yml', text)
            self.assertIn('vars.NOSTRC_DEPENDENCY_RELEASE_MODE', text)
            if name != 'windows-build':
                gate = job_block(text, 'release-aggregate')
                self.assertIn('manual-broad', gate.split('needs:', 1)[1].splitlines()[0])
                self.assertIn('if [[ "$MANUAL" == true ]]; then test "$MANUAL_BROAD" = success; fi', gate)
        self.assertNotIn('ci_legacy_conclusions', workflow('release-validation'))

    def test_shared_planner_immutable_full_history_and_cheap_guards_remain(self):
        text = workflow('ci-selective')
        select = job_block(text, 'select')
        self.assertIn('fetch-depth: 0', select)
        self.assertIn('persist-credentials: false', select)
        self.assertIn('ref: ${{ inputs.sha || github.sha }}', select)
        self.assertIn('ci_selection.py select', select)
        cheap = job_block(text, 'cheap')
        for command in ['scripts/check-unsequenced-args.py', 'scripts/check-gui-test-gdk-frame.py',
                        'scripts/check-linux-ci-packages.py', 'gnome/groundhog/tests/check_privacy.py']:
            self.assertIn(command, cheap)
        self.assertNotIn('unittest', cheap)
        self.assertIn('test-selection-operational.py', workflow('static-checks'))
        self.assertIn('static-checks.yml', job_block(text, 'static'))
        self.assertNotIn('namespace_policy_controller', cheap)
        self.assertNotIn('docker', cheap)


if __name__ == '__main__':
    unittest.main()
