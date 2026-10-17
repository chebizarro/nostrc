"""Actions glue tests. Synthetic receipts are not hosted execution evidence."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

from selector_support import GitCase, ROOT, policy, receipt, s
from profile_contract_support import step_script

SPEC = importlib.util.spec_from_file_location("dependency_contract", ROOT / ".github/workflows/dependency_contract.py")
ci = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ci)
WORKFLOWS = ROOT / ".github/workflows"


def workflow(name):
    return (WORKFLOWS / (name + ".yml")).read_text()


def job_block(text, job):
    # Bounded wiring assertions, not a YAML parser. actionlint validates YAML/Actions semantics.
    return re.search(r"^  " + re.escape(job) + r":\n(?:(?!^  [a-zA-Z0-9_-]+:).)*", text, re.M | re.S).group()


def ready_policy():
    p = policy()
    p["profiles"]["core.linux"] = p["profiles"].pop("linux")
    for check in p["checks"].values():
        check["profiles"] = ["core.linux"]
        check["ctest_tests"]["core.linux"] = check["ctest_tests"].pop("linux")
    p.update(enforcement_ready=True, coverage_gaps=[])
    return p


class WorkerAccountingTests(unittest.TestCase):
    def setUp(self):
        self.p = ready_policy()
        self.plan = s.make_plan(self.p, [{"status": "M", "old_path": "app/source.c", "new_path": "app/source.c"}],
                                head="a" * 40, context="ci")
        self.needs = {job: {"result": "skipped"} for job in ci.LANES.values()}
        self.needs.update(select={"result": "success"}, core={"result": "success"}, broad={"result": "skipped"})
        self.receipt = receipt(self.p, self.plan)

    def aggregate(self, receipts=None):
        return ci.aggregate(ROOT, self.p, self.plan, self.needs, [self.receipt] if receipts is None else receipts)

    def test_independent_selected_lane_conclusion_required(self):
        self.assertTrue(self.aggregate()["passed"])
        for result in ("skipped", "failure", "cancelled", "missing", None):
            with self.subTest(result=result):
                self.needs["core"]["result"] = result
                self.assertFalse(self.aggregate()["passed"])
        self.needs.pop("core")
        self.assertFalse(self.aggregate()["passed"])

    def test_selected_missing_duplicate_stale_or_skipped_receipt_fails(self):
        self.assertFalse(self.aggregate([])["passed"])
        self.assertFalse(self.aggregate([self.receipt, self.receipt])["passed"])
        self.receipt["source_identity"]["sha"] = "b" * 40
        self.assertFalse(self.aggregate()["passed"])
        self.receipt = receipt(self.p, self.plan)
        self.receipt["checks"]["app.unit"]["outcome"] = "unexpected_skip"
        self.assertFalse(self.aggregate()["passed"])

    def test_unselected_family_must_not_claim_execution(self):
        self.needs["grotto"]["result"] = "success"
        self.assertFalse(self.aggregate()["passed"])

    def test_selection_failure_and_broad_family_skip_fail(self):
        self.needs["select"]["result"] = "failure"
        self.assertFalse(self.aggregate()["passed"])
        self.needs["select"]["result"] = "success"
        broad = s.make_plan(self.p, [], head="a" * 40, context="ci", mode="broad")
        errors, _ = ci.check_jobs(broad, self.needs)
        self.assertIn("broad family coverage did not complete as selected", errors)

    def test_empty_set_needs_explicit_accounting_and_ready_policy(self):
        plan = s.make_plan(self.p, [], head="a" * 40, context="ci", event={"kind": "manual"})
        self.needs["core"]["result"] = "skipped"
        self.assertTrue(ci.aggregate(ROOT, self.p, plan, self.needs, [])["passed"])
        self.p["enforcement_ready"] = False
        plan = s.make_plan(self.p, [], head="a" * 40, context="ci")
        self.assertFalse(ci.aggregate(ROOT, self.p, plan, self.needs, [])["passed"])

    def test_unsupported_new_lane_fails_independently_without_hiding_other_conclusions(self):
        p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        plan = s.make_plan(p, [], head='a' * 40, context='ci', mode='broad')
        needs = {job: {'result': 'success'} for job in ci.LANES.values()}
        needs.update(select={'result': 'success'}, broad={'result': 'success'})
        for lane in ('core.linux.asan', 'core.macos.asan', 'groundhog.linux.sanitizers'):
            with self.subTest(lane=lane):
                failed = copy.deepcopy(needs)
                failed[ci.LANES[lane]]['result'] = 'failure'
                errors, conclusions = ci.check_jobs(plan, failed)
                self.assertEqual(errors, [f'{lane}: expected success, observed failure'])
                self.assertEqual(conclusions[lane], 'failure')
                self.assertTrue(all(value == 'success' for key, value in conclusions.items() if key != lane))
                result = ci.aggregate(ROOT, p, plan, failed, [])
                self.assertFalse(result['passed'])
                self.assertTrue(any(lane in error for error in result['errors']))

    def test_new_unwired_profile_cannot_silently_disappear(self):
        plan = copy.deepcopy(self.plan)
        plan["expected_lane_ids"] = ["new.profile"]
        with self.assertRaises(s.ContractError):
            ci.check_jobs(plan, self.needs)


class EventToWorkerTests(GitCase):
    def setUp(self):
        super().setUp()
        self.p = ready_policy()
        self.p["release_components"] = {
            "app": {"publisher": "main", "manifest_name": "App", "package_jobs": ["source"],
                    "version_sources": [{"path": "app/version", "pattern": r"version=([0-9.]+)"}]},
            "library": {"publisher": "none", "manifest_name": "Library", "package_jobs": [],
                        "version_sources": [{"path": "core/version", "pattern": r"version=([0-9.]+)"}]},
        }
        self.p["legacy_global"] = {"publishers": ["main", "windows"], "package_jobs": ["source", "windows.build"]}
        self.write("VERSION_MANIFEST.md", "| App | app | 1.2.3 | Unreleased | - | app/version |\n| Library | core | 4.5.6 | Unreleased | - | core/version |\n")
        self.write("app/version", "version=1.2.3\n")
        self.write("core/version", "version=4.5.6\n")
        self.write("tests/selection/inventory.json", s.canonical(self.p).decode())
        self.write("scripts/test_selection.py", (ROOT / "scripts/test_selection.py").read_text())
        self.write(".github/workflows/dependency_contract.py", (WORKFLOWS / "dependency_contract.py").read_text())
        self.head = self.commit()
        self.evidence = tempfile.TemporaryDirectory()
        self.addCleanup(self.evidence.cleanup)

    def invoke(self, event_name, event, *, ref="refs/heads/master", command="select", args=()):
        directory = Path(self.evidence.name)
        (directory / "event.json").write_text(json.dumps(event))
        (directory / "outputs").write_text("")
        env = dict(os.environ, GITHUB_EVENT_NAME=event_name, GITHUB_EVENT_PATH=str(directory / "event.json"),
                   GITHUB_SHA=self.head, GITHUB_REF=ref, GITHUB_OUTPUT=str(directory / "outputs"), GITHUB_RUN_ID="42")
        result = subprocess.run([sys.executable, str(self.repo / ".github/workflows/dependency_contract.py"), command,
                                 "--sha", self.head, "--directory", str(directory), *args], env=env, capture_output=True, text=True)
        outputs = dict(line.split("=", 1) for line in (directory / "outputs").read_text().splitlines())
        return result, outputs, directory

    def test_fork_pr_merge_identity_reaches_exact_selected_workers(self):
        event = {"pull_request": {"head": {"sha": self.head, "repo": {"fork": True}}, "base": {"sha": self.base}}}
        result, outputs, directory = self.invoke("pull_request", event, ref="refs/pull/3/merge")
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads((directory / "plan.json").read_text())
        self.assertEqual(plan["event"]["pr_head_sha"], self.head)
        self.assertEqual(outputs["sha"], self.head)
        self.assertEqual(plan["tested_sha"], outputs["sha"])
        self.assertEqual(json.loads(outputs["lanes"]), ["core.linux"])
        self.assertEqual(outputs["run"], "true")

    def test_merge_group_and_mismatched_group_identity(self):
        event = {"merge_group": {"base_sha": self.base, "head_sha": self.head}}
        result, outputs, directory = self.invoke("merge_group", event)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((directory / "plan.json").read_text())["event"]["kind"], "merge_group")
        event["merge_group"]["head_sha"] = self.base
        self.assertNotEqual(self.invoke("merge_group", event)[0].returncode, 0)

    def test_postmerge_and_manual_absent_base_are_broad(self):
        for name, event in (("push", {"before": self.base}), ("workflow_dispatch", {"inputs": {}})):
            with self.subTest(event=name):
                result, outputs, _ = self.invoke(name, event)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(outputs["broad"], "true")

    def test_current_inventory_broad_selection_dispatches_every_distinct_profile(self):
        # Reproduce the hosted selector failure with the actual inventory, not ready_policy.
        self.p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.write('tests/selection/inventory.json', s.canonical(self.p).decode())
        self.write('scripts/test_selection_realization.py', (ROOT / 'scripts/test_selection_realization.py').read_text())
        self.head = self.commit()
        result, outputs, directory = self.invoke('workflow_dispatch', {'inputs': {}}, args=('--mode', 'broad'))
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads((directory / 'plan.json').read_text())
        eligible = {ident for ident, profile in self.p['profiles'].items()
                    if 'ci' in profile.get('contexts', ['local', 'ci', 'release'])}
        self.assertEqual(set(plan['expected_lane_ids']), eligible)
        omitted = {item['id']: item for item in plan['omitted_profiles']}
        self.assertEqual(set(omitted), set(self.p['profiles']) - eligible)
        for ident, item in omitted.items():
            self.assertEqual(item['status'], 'not_executed')
            self.assertEqual(item['context'], 'ci')
            self.assertEqual(item['allowed_contexts'], self.p['profiles'][ident]['contexts'])
            self.assertEqual(item['profile_hash'], s.digest(self.p['profiles'][ident]))
            self.assertTrue(item['reason'])
        self.assertEqual(set(json.loads(outputs['lanes'])), set(ci.LANES))
        self.assertEqual(outputs['sha'], self.head)
        self.assertEqual(outputs['run'], 'true')
        self.assertEqual(outputs['broad'], 'true')
        # Dispatch success is not a complete-gate or profile-union attestation.
        self.assertFalse(plan['enforcement_ready'])
        self.assertTrue(plan['coverage_gaps'])

    def test_annotated_validation_only_tag_reaches_broad_workers_without_publisher(self):
        self.git("tag", "-am", "annotated", "library-v4.5.6")
        result, outputs, directory = self.invoke("push", {"before": "0" * 40}, ref="refs/tags/library-v4.5.6")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(outputs["route"], "validation_only")
        self.assertEqual(outputs["broad"], "true")
        release = json.loads((directory / "release.json").read_text())
        self.assertFalse(release["publish"])
        self.assertEqual(release["tested_sha"], self.head)
        self.assertEqual(json.loads((directory / "plan.json").read_text())["event"]["tag"], "library-v4.5.6")

    def test_existing_and_legacy_tags_route_without_duplicate_validation(self):
        for tag, route in (("app-v1.2.3", "main"), ("v987.0.0", "main,windows")):
            self.git("tag", tag)
            result, outputs, _ = self.invoke("push", {"before": "0" * 40}, ref="refs/tags/" + tag)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(outputs["route"], route)
            self.assertEqual(outputs["run"], "false")
            result, outputs, _ = self.invoke("push", {}, ref="refs/tags/" + tag, args=("--tag", tag, "--mode", "broad"))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(outputs["run"], "true")

    def test_unknown_component_tag_and_wrong_publisher_fail(self):
        self.git("tag", "unknown-v1.2.3")
        self.assertNotEqual(self.invoke("push", {}, ref="refs/tags/unknown-v1.2.3")[0].returncode, 0)
        self.git("tag", "library-v4.5.6")
        result, _, _ = self.invoke("push", {}, ref="refs/tags/library-v4.5.6", command="release-identity", args=("--publisher", "main"))
        self.assertNotEqual(result.returncode, 0)

    def test_branch_and_tag_dry_run_identity_never_publishes(self):
        self.git("tag", "app-v1.2.3")
        for ref in ("refs/heads/master", "refs/tags/app-v1.2.3"):
            result, _, directory = self.invoke("workflow_dispatch", {}, ref=ref, command="release-identity", args=("--component", "app", "--dry-run"))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(json.loads((directory / "release.json").read_text())["publish"])

    def test_staged_publication_cli_rechecks_identity_and_records_no_validation_pass(self):
        result, _, directory = self.invoke('workflow_dispatch', {}, command='release-identity', args=('--component', 'app', '--dry-run'))
        self.assertEqual(result.returncode, 0, result.stderr)
        needs = {job: {'result': 'skipped'} for job in ci.PUBLISHER_JOBS['main']}
        needs.update({'build-source': {'result': 'success'}, 'validation': {'result': 'skipped'}})
        (directory / 'needs.json').write_text(json.dumps(needs))
        (directory / 'artifacts.json').write_text(json.dumps([{'artifacts': [{'name': 'source', 'id': 1, 'size_in_bytes': 1}]}]))
        args = ('--publisher', 'main', '--release-mode', 'disabled', '--needs', str(directory / 'needs.json'), '--artifacts', str(directory / 'artifacts.json'))
        result, outputs, _ = self.invoke('workflow_dispatch', {}, command='publish-gate', args=args)
        self.assertEqual(result.returncode, 0, result.stderr)
        decision = json.loads((directory / 'publication.json').read_text())
        self.assertEqual(outputs['artifact_ids'], '1')
        self.assertEqual(decision['validation_status'], 'staged_disabled')
        self.assertIsNone(decision['validation_passed'])
        self.assertFalse(decision['publish'])
        release = json.loads((directory / 'release.json').read_text())
        release['source_identity']['sha'] = self.base
        (directory / 'release.json').write_text(json.dumps(release))
        result, outputs, _ = self.invoke('workflow_dispatch', {}, command='publish-gate', args=args)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('artifact_ids', outputs)

    def test_wrong_tag_sha_cannot_route_to_worker(self):
        self.git("tag", "app-v1.2.3", self.base)
        self.assertNotEqual(self.invoke("push", {}, ref="refs/tags/app-v1.2.3")[0].returncode, 0)


class PublicationTests(unittest.TestCase):
    def release(self, component):
        p = s.load_inventory(ROOT / "tests/selection/inventory.json")
        return s.release_plan(ROOT, p, head="HEAD", component=component, dry_run=True, run_id="synthetic")

    def needs(self, release, publisher):
        required = release["required_package_jobs"]
        result = {"validation": {"result": "success"}}
        for job in ci.PUBLISHER_JOBS[publisher]:
            key = "source" if job == "build-source" else "windows.build" if publisher == "windows" else job
            selected = any(x == key or x.startswith(key + ".") for x in required)
            result[job] = {"result": "success" if selected else "skipped"}
        return result

    def validation(self, release):
        return dict(passed=True, complete=True, enforcement_ready=True, coverage_gaps=[],
                    source_identity=release["source_identity"], tested_sha=release["tested_sha"],
                    inventory_hash=release["inventory_hash"], mode="broad", tag=release["tag"], partial_profiles=[])

    def test_all_existing_routes_and_intentionally_inapplicable_jobs(self):
        for component, publisher in (("libgo", "main"), ("gnostr", "main"), ("grotto", "main"),
                                     ("groundhog", "groundhog"), ("global", "main"), ("global", "windows")):
            with self.subTest(component=component, publisher=publisher):
                release = self.release(component)
                result = ci.publish_gate(release, publisher, self.validation(release), self.needs(release, publisher))
                self.assertTrue(result["passed"])
                self.assertFalse(result["publish"])

    def test_explicit_staging_never_forges_validation_or_weakens_package_gate(self):
        release = self.release('gnostr')
        needs = self.needs(release, 'main')
        needs['validation']['result'] = 'failure'
        for mode in ('disabled', 'shadow'):
            result = ci.publish_gate(release, 'main', None, needs, mode)
            self.assertTrue(result['passed'])
            self.assertFalse(result['validation_required'])
            self.assertIsNone(result['validation_passed'])
            self.assertEqual(result['validation_status'], 'staged_' + mode)
        with self.assertRaises(s.ContractError):
            ci.publish_gate(release, 'main', None, needs, 'enforce')
        with self.assertRaises(s.ContractError):
            ci.publish_gate(release, 'main', None, needs, 'typo')
        needs['build-linux']['result'] = 'cancelled'
        with self.assertRaises(s.ContractError):
            ci.publish_gate(release, 'main', None, needs, 'disabled')

    def test_failure_cancelled_missing_or_selected_skip_blocks_publication(self):
        release = self.release("grotto")
        needs = self.needs(release, "main")
        for result in ("failure", "cancelled", "skipped", None):
            needs["build-linux"]["result"] = result
            with self.subTest(result=result), self.assertRaises(s.ContractError):
                ci.publish_gate(release, "main", self.validation(release), needs)
        del needs["build-linux"]
        with self.assertRaises(s.ContractError):
            ci.publish_gate(release, "main", self.validation(release), needs)

    def test_exact_sha_broad_tag_and_complete_coverage_required(self):
        release = self.release("gnostr")
        for key, value in (("tested_sha", "b" * 40), ("inventory_hash", "stale"), ("tag", "other"),
                           ("mode", "affected"), ("complete", False), ("enforcement_ready", False),
                           ("coverage_gaps", ["gap"]), ("partial_profiles", ["core.linux"])):
            validation = self.validation(release)
            validation[key] = value
            with self.subTest(key=key), self.assertRaises(s.ContractError):
                ci.publish_gate(release, "main", validation, self.needs(release, "main"))

    def test_inapplicable_success_and_unowned_publisher_fail_closed(self):
        release = self.release("libgo")
        needs = self.needs(release, "main")
        needs["build-linux"]["result"] = "success"
        with self.assertRaises(s.ContractError):
            ci.publish_gate(release, "main", self.validation(release), needs)
        with self.assertRaises(s.ContractError):
            ci.package_conclusions(self.release("nip19"), "main", needs)

    def test_artifacts_exact_names_no_validation_log_or_extra_platform_sweep(self):
        release = self.release("groundhog")
        names = ["source", *[f"deb-{d}-{a}" for d in ("ubuntu24.04", "ubuntu26.04", "debian13") for a in ("x86_64", "aarch64")],
                 *[f"{f}-{a}" for f in ("rpm", "arch", "flatpak") for a in ("x86_64", "aarch64")]]
        artifacts = [{"id": i, "name": name, "size_in_bytes": 1, "expired": False} for i, name in enumerate(names, 1)]
        artifacts.append({"id": 99, "name": "dependency-validation-logs", "size_in_bytes": 10})
        self.assertNotIn("99", ci.package_artifacts(release, "groundhog", artifacts))
        for mutation in (artifacts[:-2], artifacts + [artifacts[0]], [dict(a, expired=True) for a in artifacts]):
            with self.assertRaises(s.ContractError):
                ci.package_artifacts(release, "groundhog", mutation)

    def test_windows_only_historical_exe_asset_not_portable_or_other_publisher(self):
        release = self.release("global")
        release["version"] = "1.2.3"
        artifacts = [{"id": 1, "name": "GNostrSigner-1.2.3-x64-setup", "size_in_bytes": 1},
                     {"id": 2, "name": "GNostrSigner-1.2.3-x64-portable", "size_in_bytes": 1},
                     {"id": 3, "name": "source", "size_in_bytes": 1}]
        self.assertEqual(ci.package_artifacts(release, "windows", artifacts), ["1"])


class NativeProfileTests(unittest.TestCase):
    def test_shared_metrics_header_has_input_owner_without_library_cycle(self):
        p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        classified = s.classify(p, 'libnostr/include/nostr/metrics.h')
        self.assertEqual(classified['nodes'], ['libnostr.metrics_header'])
        self.assertEqual(p['nodes']['libnostr.metrics_header']['depends_on'], [])
        self.assertIn('libnostr.metrics_header', p['nodes']['libgo']['depends_on'])
        self.assertNotIn('libnostr', p['nodes']['libgo']['depends_on'])
        affected = s.impact_closure(p, {'libnostr.metrics_header'})
        self.assertIn('libgo', affected)
        self.assertIn('libnostr', affected)

    def test_existing_macos_libgo_profile_is_faithful_and_stress_gap_is_not_passed(self):
        p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        profile = p['profiles']['libgo.macos.asan']
        self.assertEqual((profile['platform'], profile['architecture'], profile['source_dir']), ('macos', 'arm64', 'libgo'))
        self.assertEqual(profile['environment']['ASAN_OPTIONS'], 'detect_leaks=0')
        self.assertIn('-DGO_ENABLE_ASAN=ON', profile['configure_args'])
        self.assertIn('-DGO_ENABLE_UBSAN=ON', profile['configure_args'])
        self.assertIn('-DGOF_PORTABLE_CONTEXT=OFF', profile['configure_args'])
        members = [name for check in p['checks'].values() for name in check.get('ctest_tests', {}).get('libgo.macos.asan', [])]
        self.assertEqual(len(members), 32)
        self.assertEqual(len(set(members)), 32)
        self.assertIn('GoChannelTest', members)
        self.assertIn('GoFiberChanTestW4', members)
        self.assertNotIn('GoChannelStressTest', members)
        gaps = [g for g in p['coverage_gaps'] if g.get('profile') == 'libgo.macos.asan']
        self.assertEqual(gaps[0]['registrations'], ['GoChannelStressTest'])
        self.assertFalse(p['enforcement_ready'])
        self.assertIn('-E "GoChannelStressTest"', workflow('libgo-ci'))


class WorkflowWiringTests(unittest.TestCase):
    def test_unfiltered_entrypoint_merge_queue_read_only_and_stable_direct_gate(self):
        text = workflow("dependency-tests")
        self.assertNotIn("paths:", text)
        self.assertIn("merge_group:", text)
        self.assertIn("tags: ['v*', '**-v*']", text)
        self.assertIn("contents: read", text)
        self.assertNotIn("pull_request_target", text)
        self.assertNotIn("workflow_run", text)
        self.assertNotIn("secrets:", text)
        gate = job_block(text, "gate")
        self.assertIn("name: Dependency gate", gate)
        self.assertIn("if: always()", gate)
        for job in (*ci.LANES.values(), "select", "broad"):
            self.assertIn(job, gate.split("needs:", 1)[1].splitlines()[0])
        for lane, job in ci.LANES.items():
            block = job_block(text, job)
            self.assertIn("profile: " + lane, block)
            self.assertIn("contains(fromJSON(needs.select.outputs.lanes), '" + lane + "')", block)
            self.assertIn("sha: ${{ needs.select.outputs.sha }}", block)

    def test_current_inventory_profiles_have_exact_independent_worker_calls_and_gate_needs(self):
        expected = {
            'core.linux': 'core', 'core.linux.asan': 'core_asan', 'core.macos.asan': 'core_native',
            'grotto.linux': 'grotto', 'grotto.linux.asan_ubsan': 'grotto_asan', 'groundhog.linux': 'groundhog',
            'groundhog.linux.sanitizers': 'groundhog_sanitizers', 'groundhog.mdk08': 'mdk08',
            'groundhog.mdk011': 'mdk011',
            'libgo.linux.asan': 'libgo', 'libgo.macos.asan': 'libgo_native', 'static.policy': 'policy',
        }
        p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        plan = s.make_plan(p, [], head='a' * 40, context='ci', mode='broad')
        self.assertEqual(set(plan['expected_lane_ids']), set(expected))
        self.assertEqual(ci.LANES, expected)
        self.assertEqual(len(set(ci.LANES.values())), len(ci.LANES))
        text = workflow('dependency-tests')
        gate_needs = re.search(r'^    needs: \[(.*)\]$', job_block(text, 'gate'), re.M).group(1)
        self.assertEqual({x.strip() for x in gate_needs.split(',')}, {'select', 'broad', *expected.values()})
        for lane, job in expected.items():
            with self.subTest(lane=lane):
                block = job_block(text, job)
                self.assertIn('    needs: select\n', block)
                self.assertIn('uses: ./.github/workflows/dependency-worker.yml', block)
                self.assertIn('profile: ' + lane + '\n', block)
                self.assertIn('plan_id: ${{ needs.select.outputs.plan_id }}', block)
                self.assertIn('sha: ${{ needs.select.outputs.sha }}', block)
                self.assertNotIn('continue-on-error', block)

    def test_worker_installs_the_required_libpeas_abi_and_conditional_tools(self):
        text = workflow('dependency-worker')
        setup = text.split('- name: Install Linux profile prerequisites', 1)[1].split('- name:', 1)[0]
        packages = re.search(r'apt-get install(.*?)sudo localedef', setup, re.S).group(1).split()
        self.assertIn('libpeas-2-dev', packages)
        self.assertNotIn('libpeas-dev', packages)
        for package in ('libxml2-dev', 'at-spi2-core', 'dbus-bin', 'nodejs'):
            self.assertIn(package, packages)
        self.assertIn('pkg-config --modversion libpeas-2', setup)
        # This ABI requirement comes from the actual default application configure.
        self.assertIn('pkg_check_modules(LIBPEAS REQUIRED IMPORTED_TARGET libpeas-2)',
                      (ROOT / 'apps/gnostr/CMakeLists.txt').read_text())

    def test_native_root_setup_and_phase_metadata_are_not_standalone_libgo_aliases(self):
        text = workflow('dependency-worker')
        self.assertIn("(inputs.profile == 'libgo.macos.asan' || inputs.profile == 'core.macos.asan') && 'macos-latest' || 'ubuntu-24.04'", text)
        root_setup = text.split('- name: Install native root ASan prerequisites', 1)[1].split('- name:', 1)[0]
        self.assertIn("if: runner.os == 'macOS' && inputs.profile == 'core.macos.asan'", root_setup)
        for package in ('jansson', 'secp256k1', 'libwebsockets', 'libsodium', 'openssl@3', 'libsoup@3', 'nsync', 'flatcc', 'json-glib', 'libgit2', 'dbus'):
            self.assertIn(package, root_setup)
        self.assertNotIn('|| true', root_setup)
        self.assertIn("profile['platform'] == actual_platform and profile['architecture'] == platform.machine()", text)
        self.assertIn('--profile "$PROFILE"', text)
        self.assertIn('--configure --phase all', text)
        self.assertNotIn('--configure-arg', text)
        self.assertNotIn('ASAN_OPTIONS:', text)  # No worker-level environment flattening/override.
        p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        root = p['profiles']['core.macos.asan']
        self.assertEqual((root['platform'], root['architecture'], root['source_dir']), ('macos', 'arm64', '.'))
        self.assertIn('-DGO_ENABLE_ASAN=ON', root['configure_args'])
        self.assertIn('-DGO_ENABLE_UBSAN=ON', root['configure_args'])
        self.assertNotEqual(root['environment']['ASAN_OPTIONS'], p['profiles']['libgo.macos.asan']['environment']['ASAN_OPTIONS'])
        sanitizer = p['profiles']['groundhog.linux.sanitizers']
        self.assertEqual(sanitizer['phase_environments']['configure'], {})
        self.assertEqual(sanitizer['phase_environments']['build'], {})
        self.assertIn('LSAN_OPTIONS', sanitizer['phase_environments']['test'])
        self.assertIn('sanitizer_compatibility', sanitizer)

    def test_worker_pipeline_preserves_named_failure_without_synthesizing_receipt(self):
        # Execute the real worker shell with a fake CLI; this is routing/pipefail evidence only.
        text = workflow('dependency-worker')
        script = step_script(text, 'Resolve, audit, build and execute exact selected checks')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = root / 'bin'
            binary.mkdir()
            fake = binary / 'python3'
            fake.write_text('#!' + sys.executable + '\n' + '''import json, os, pathlib, sys
args = sys.argv[1:]
pathlib.Path(os.environ['CAPTURE']).write_text(json.dumps(args))
if os.environ['FAIL_RUNTIME'] == '1':
    print(os.environ['PROFILE'] + ': unsupported runtime/capabilities', file=sys.stderr)
    sys.exit(23)
output = pathlib.Path(args[args.index('--output') + 1])
output.write_text(json.dumps({'profile': os.environ['PROFILE'], 'synthetic_test_only': True}))
''')
            fake.chmod(0o755)
            for name, skip in (('xvfb-run', 1), ('dbus-run-session', 1)):
                wrapper = binary / name
                if name == 'xvfb-run':
                    wrapper.write_text('#!/bin/sh\nshift\nif [ "$1" = -s ]; then shift 2; fi\nexec "$@"\n')
                else:
                    wrapper.write_text('#!/bin/sh\nshift ' + str(skip) + '\nexec "$@"\n')
                wrapper.chmod(0o755)
            for profile in ('core.linux.asan', 'core.macos.asan', 'groundhog.linux.sanitizers',
                            'grotto.linux', 'grotto.linux.asan_ubsan', 'groundhog.mdk011'):
                for fail in ('0', '1'):
                    with self.subTest(profile=profile, failure=fail):
                        temp = root / (profile + '-' + fail)
                        temp.mkdir()
                        capture = temp / 'argv.json'
                        env = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ['PATH'],
                                   PROFILE=profile, RUNNER_TEMP=str(temp), CAPTURE=str(capture), FAIL_RUNTIME=fail,
                                   RUNNER_OS='macOS' if profile == 'core.macos.asan' else 'Linux')
                        result = subprocess.run(['bash', '-c', script], env=env, capture_output=True, text=True)
                        self.assertEqual(result.returncode, 23 if fail == '1' else 0, result.stdout + result.stderr)
                        args = json.loads(capture.read_text())
                        self.assertEqual(args[:2], ['scripts/test-plan.py', 'run'])
                        self.assertEqual(args[args.index('--profile') + 1], profile)
                        self.assertEqual(args[args.index('--phase') + 1], 'all')
                        self.assertEqual(args[args.index('--parallel') + 1],
                                         '1' if profile.startswith('grotto.linux') else '2')
                        receipt_path = temp / 'dependency/receipts' / profile / 'receipt.json'
                        self.assertEqual(receipt_path.exists(), fail == '0')
                        if fail == '1':
                            log = (receipt_path.parent / 'worker.log').read_text()
                            self.assertIn(profile + ': unsupported runtime/capabilities', log)
        self.assertIn("name: dependency-receipt-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}", text)
        self.assertIn('if: always()', text.split('- name: Retain incomplete evidence', 1)[1])

    def test_all_broad_families_reusable_and_same_sha_without_publisher_recursion(self):
        text = workflow("dependency-broad")
        for family in ci.FAMILIES:
            self.assertIn("uses: ./.github/workflows/" + family + ".yml", text)
            family_text = workflow(family)
            self.assertIn("workflow_call:", family_text)
            self.assertIn("ref: ${{ inputs.sha || github.sha }}", family_text)
            self.assertIn("persist-credentials: false", family_text)
            self.assertIn("Verify requested checkout", family_text)
            self.assertNotIn("secrets: inherit", family_text)
        self.assertIn('uses: ./.github/workflows/windows-validation.yml', job_block(text, 'windows'))
        self.assertNotIn('contents: write', workflow('windows-validation'))
        self.assertNotIn('softprops/action-gh-release', workflow('windows-validation'))
        self.assertNotIn("continue-on-error", text)
        self.assertIn("if: always()", job_block(text, "broad_gate"))

    def test_legacy_schedules_and_status_triggers_retained_during_shadow(self):
        for family in ("grotto-fuzz", "marmot-mdk-interop", "marmot-mdk011-interop"):
            self.assertIn("schedule:", workflow(family))
        for family in ("nostrc-ci", "grotto-ci", "groundhog-ci", "windows-build"):
            self.assertIn("pull_request:", workflow(family))
            self.assertIn("push:", workflow(family))
        self.assertIn("name: ${{ inputs.artifact_prefix }}bp-matrix-nightly-artifacts", workflow('bp-matrix-nightly'))

    def test_every_publisher_uses_validation_gate_and_exact_asset_ids(self):
        for name, job in (("release", "create-release"), ("groundhog-release", "release"), ("windows-build", "release")):
            text = workflow(name)
            self.assertIn("uses: ./.github/workflows/release-validation.yml", text)
            self.assertIn("publish-gate", text)
            self.assertIn("vars.NOSTRC_DEPENDENCY_RELEASE_MODE", text)
            self.assertIn('--release-mode "$RELEASE_MODE"', text)
            publish = job_block(text, job)
            self.assertIn("needs: ", publish)
            self.assertIn("release-aggregate", publish)
            self.assertIn("artifact-ids: ${{ needs.release-aggregate.outputs.artifact_ids }}", publish)
            self.assertNotIn("name: ${{ needs.validation.outputs.validation_artifact }}", publish)
        self.assertIn("github.event_name == 'push'", job_block(workflow("groundhog-release"), "release"))
        self.assertIn("!inputs.dry_run", job_block(workflow("release"), "create-release"))

    def test_current_groundhog_guard_and_advisory_grotto_contracts_retained(self):
        self.assertIn('set(GROUNDHOG_PRERELEASE', workflow("groundhog-release"))
        self.assertIn("tag version $v does not match the app's version", workflow("groundhog-release"))
        self.assertIn("groundhog --smoke", workflow("groundhog-release"))
        self.assertIn("grotto-ci-quarantine.tsv", workflow("grotto-ci"))
        self.assertIn("continue-on-error", workflow("grotto-ci"))
        self.assertIn("fail-fast: false", workflow("release"))

    def test_fuzz_selection_constructs_matrix_and_pipeline_is_blocking(self):
        text = workflow("grotto-fuzz")
        self.assertNotIn("Filter targets for manual run", text)
        self.assertIn("fromJSON(needs.select-fuzz.outputs.targets)", text)
        self.assertIn("set -euo pipefail", text)
        self.assertEqual(ci.fuzz_selection("json-event", "3600"), (["json-event"], 3600))
        self.assertEqual(ci.fuzz_selection()[1], 60)
        self.assertEqual(ci.fuzz_selection(extended=True)[1], 300)
        for value in ("0", "-1", "1.1", "3601", "$(touch x)", "1\n2", "١"):
            with self.subTest(value=value), self.assertRaises(s.ContractError):
                ci.fuzz_selection(duration=value)
        with self.assertRaises(s.ContractError):
            ci.fuzz_selection("not-a-target")
