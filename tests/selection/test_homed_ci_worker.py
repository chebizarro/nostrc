"""Authored CI worker route for the standalone five-unit homed profile.

Fresh CI authorship, not a relabel of local proof and not legacy parity: the CI
profile realizes exactly the local five-unit configuration through the existing
dependency worker's pidns custody, per-job AppArmor provisioning and normal
qualification. The legacy nostr-homed-ci root/systemd/FUSE matrix cells, the
coverage gap and its receipt limits stay as they were.
"""
import re
import sys
import unittest

from selector_support import ROOT, s, change

sys.path.insert(0, str(ROOT / '.github/workflows'))
import dependency_contract as ci  # noqa: E402
import test_selection_execution as normal  # noqa: E402

LOCAL, CI = 'homed.linux.units', 'homed.linux.ci'
NAMES = ['homed_cache', 'homed_cache_config', 'homed_groups', 'homed_manifest', 'homed_uid_map']
ROUTE_FIELDS = {'setup', 'local_availability', 'contexts'}
LEGACY = '.github/workflows/nostr-homed-ci.yml:build-and-test'
INPUTS = ['gnome/nostr-homed/src/common/nostr_cache.c', 'gnome/nostr-homed/include/nostr_manifest.h',
          'gnome/nostr-homed/tests/unit/test_manifest.c']


def workflow(name):
    return (ROOT / '.github/workflows' / (name + '.yml')).read_text()


def job_block(text, name):
    return re.search(r'^  ' + name + r':\n(.*?)(?=^  [A-Za-z_]+:|\Z)', text, re.M | re.S).group(1)


def step_block(text, name):
    return text.split('      - name: ' + name + '\n', 1)[1].split('\n      - ', 1)[0]


def homed_lanes(plan):
    return [lane['profile'] for lane in plan['lanes'] if lane['profile'].startswith('homed.')]


class HomedCIProfile(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.local, self.ci = self.policy['profiles'][LOCAL], self.policy['profiles'][CI]

    def test_ci_profile_is_the_local_five_unit_realization_with_only_route_identity_changed(self):
        self.assertEqual(set(self.ci), set(self.local))
        for key, value in self.local.items():
            if key not in ROUTE_FIELDS:
                self.assertEqual(self.ci[key], value, key)
        self.assertEqual(self.ci['contexts'], ['ci'])
        self.assertEqual(self.local['contexts'], ['local'])
        self.assertEqual(self.ci['setup'], '.github/workflows/dependency-worker.yml:execute')
        self.assertEqual(self.local['setup'], 'standalone/nostr-homed-units-v1')
        self.assertEqual(self.ci['source_dir'], 'gnome/nostr-homed/tests/unit')
        self.assertEqual(self.ci['host_system'], {'ID': 'ubuntu', 'VERSION_ID': '24.04'})
        self.assertEqual((self.ci['platform'], self.ci['architecture'], self.ci['compiler']), ('linux', 'x86_64', 'gcc'))
        self.assertEqual(self.ci['capabilities'], [])
        # Only the existing full normal contract; no other execution marker is authored.
        self.assertEqual(self.ci[normal.FIELD], normal.MARKER)
        self.assertEqual([key for key in self.ci if 'execution' in key], [normal.FIELD])
        normal.validate_metadata(self.policy)

    def test_same_five_owned_units_and_default_build_without_skip_authority(self):
        units, build = self.policy['checks']['homed.units'], self.policy['checks']['homed.default_build']
        self.assertEqual(units['profiles'], [LOCAL, CI])
        self.assertEqual(units['ctest_tests'], {LOCAL: NAMES, CI: NAMES})
        self.assertEqual(units['build_targets'], ['test_cache', 'test_cache_config', 'test_groups',
                                                  'test_manifest', 'test_uid_map'])
        self.assertEqual(build['profiles'], [LOCAL, CI])
        self.assertEqual((build['build_scope'], build['build_targets']), ('default', []))
        self.assertEqual(build['command'], ['cmake', '--build', '{build}', '--config', 'RelWithDebInfo'])
        for check in (units, build):
            self.assertEqual((check['requirement'], check['expected_outcome']), ('blocking', 'pass'))
        claims = [name for check in self.policy['checks'].values() for name in check.get('ctest_tests', {}).get(CI, [])]
        self.assertEqual(claims, NAMES)
        self.assertEqual(sorted(ident for ident, check in self.policy['checks'].items() if CI in check['profiles']),
                         ['homed.default_build', 'homed.units'])
        overrides = repr(self.policy.get('test_overrides', []))
        for name in NAMES:
            self.assertNotIn(name, overrides)

    def test_each_context_selects_only_its_own_homed_realization(self):
        for path in INPUTS:
            with self.subTest(path=path):
                local = s.make_plan(self.policy, [change(path)], head='a' * 40, base='b' * 40, context='local')
                self.assertEqual(homed_lanes(local), [LOCAL])
                plan = s.make_plan(self.policy, [change(path)], head='a' * 40, base='b' * 40, context='ci')
                self.assertEqual(homed_lanes(plan), [CI])
                lane = next(lane for lane in plan['lanes'] if lane['profile'] == CI)
                self.assertEqual(set(lane['checks']), {'homed.units', 'homed.default_build'})
                omitted = {item['id']: item for item in plan['omitted_profiles']}
                self.assertEqual(omitted[LOCAL]['status'], 'not_executed')
                self.assertNotIn(CI, omitted)
                release = s.make_plan(self.policy, [change(path)], head='a' * 40, base='b' * 40, context='release')
                self.assertEqual(homed_lanes(release), [])
                self.assertIn(CI, {item['id'] for item in release['omitted_profiles']})
        broad = s.make_plan(self.policy, [], head='a' * 40, context='ci', mode='broad')
        self.assertIn(CI, broad['expected_lane_ids'])
        self.assertNotIn(LOCAL, broad['expected_lane_ids'])
        with self.assertRaisesRegex(s.ContractError, 'unsupported in requested context'):
            s.make_plan(self.policy, [], head='a' * 40, context='ci', mode='broad', profiles=[LOCAL])

    def test_legacy_matrix_cells_gap_and_receipt_limits_unchanged(self):
        record = self.policy['workflow_jobs'][LEGACY]
        self.assertFalse({LOCAL, CI} & set(record['profiles']))
        self.assertEqual(self.local['legacy_workflow'], LEGACY)
        gap = next(g for g in self.policy['coverage_gaps'] if g['id'] == 'homed_unit_profile_contract')
        self.assertEqual(gap['status'], 'not_executed_by_selective_gate')
        self.assertEqual(sorted(gap['not_selected_registrations']), NAMES)
        self.assertIn('four legacy distro root', gap['reason'])
        self.assertFalse(self.policy['enforcement_ready'])
        legacy = workflow('nostr-homed-ci')
        for token in (CI, 'dependency-worker', 'dependency_contract'):
            self.assertNotIn(token, legacy)


class HomedCIWorkerRoute(unittest.TestCase):
    def test_dispatcher_lane_job_and_gate_need(self):
        self.assertEqual(ci.LANES[CI], 'homed_units')
        self.assertNotIn(LOCAL, ci.LANES)
        self.assertEqual(len(set(ci.LANES.values())), len(ci.LANES))
        self.assertNotIn('homed_units', ci.BROAD_FAMILIES)
        text = workflow('dependency-tests')
        block = job_block(text, 'homed_units')
        for token in ('    needs: select\n',
                      "contains(fromJSON(needs.select.outputs.lanes), 'homed.linux.ci')",
                      'uses: ./.github/workflows/dependency-worker.yml',
                      'sha: ${{ needs.select.outputs.sha }}',
                      'plan_id: ${{ needs.select.outputs.plan_id }}', 'profile: homed.linux.ci\n'):
            self.assertIn(token, block)
        self.assertNotIn('continue-on-error', block)
        needs = re.search(r'^    needs: \[(.*)\]$', job_block(text, 'gate'), re.M).group(1)
        self.assertIn('homed_units', {item.strip() for item in needs.split(',')})

    def test_gate_accounts_for_the_homed_worker_conclusion(self):
        plan = {'expected_lane_ids': [CI], 'effective_mode': 'affected'}
        needs = {job: {'result': 'skipped'} for job in ci.LANES.values()}
        needs['broad'] = {'result': 'skipped'}
        needs['homed_units'] = {'result': 'success'}
        self.assertEqual(ci.check_jobs(plan, needs), ([], {CI: 'success'}))
        for result in ('failure', 'skipped', 'cancelled'):
            needs['homed_units'] = {'result': result}
            errors, _ = ci.check_jobs(plan, needs)
            self.assertEqual(errors, [f'{CI}: expected success, observed {result}'])
        needs['homed_units'] = {'result': 'success'}
        errors, _ = ci.check_jobs({'expected_lane_ids': [], 'effective_mode': 'affected'}, needs)
        self.assertEqual(errors, [f'{CI}: expected skipped, observed success'])

    def test_worker_routes_through_pidns_apparmor_and_normal_qualification(self):
        text = workflow('dependency-worker')
        runner = re.search(r'^    runs-on: (.*)$', text, re.M).group(1)
        self.assertNotIn('homed', runner)  # the ubuntu-24.04 default host
        provision = step_block(text, 'Provision per-job namespace-custody AppArmor profile')
        self.assertIn('static.policy:*|core.linux:*|grotto.linux:*|groundhog.linux:*|homed.linux.ci:*|*:pidns)', provision)
        execute = step_block(text, 'Resolve, audit, build and execute exact selected checks')
        pidns = execute.split('            pidns|homed.linux.ci|core.linux|grotto.linux|groundhog.linux)\n', 1)[1]
        pidns = pidns.split('\n              ;;\n', 1)[0]
        self.assertIn('--native-custody pidns', pidns)
        self.assertIn('--profile "$PROFILE"', pidns)
        qualify = step_block(text, 'Qualify normal lane after pidns exit and AppArmor teardown')
        self.assertIn("if: success() && runner.os == 'Linux'", qualify)
        self.assertIn('worker-qualify', qualify)
        self.assertLess(text.index("- name: Remove only this job's AppArmor profile after its consumers exited"),
                        text.index('- name: Qualify normal lane after pidns exit and AppArmor teardown'))
        setup = step_block(text, 'Install Linux profile prerequisites')
        packages = re.search(r'apt-get install(.*?)sudo localedef', setup, re.S).group(1).split()
        for package in ('build-essential', 'cmake', 'ninja-build', 'pkg-config',
                        'libjansson-dev', 'libsqlite3-dev', 'libssl-dev'):
            self.assertIn(package, packages)
        # Existing support only: no homed-specific step, container backend or helper.
        self.assertEqual(text.count('homed'), 2)


if __name__ == '__main__':
    unittest.main()
