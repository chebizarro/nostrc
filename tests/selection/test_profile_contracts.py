"""Profile fidelity and bounded configured membership; not live execution proof."""
import copy
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from selector_support import ROOT, s, change
from realization_support import Project, r
from profile_contract_support import source_job_blocks, source_jobs, step_script, validate_job_accounting

INVENTORY = ROOT / "tests/selection/inventory.json"
EXPORTS = ROOT / "tests/selection/fixtures/profile_exports.json"
ROOT_WORKFLOW = ROOT / ".github/workflows/nostrc-ci.yml"
SANITIZER_HELPER = ROOT / "scripts/sanitizer-gate-ci.py"
ROOT_PROFILES = {"Linux": "core.linux.asan", "macOS": "core.macos.asan"}


def audited_nodes(check_id):
    audits = json.loads((ROOT / "tests/selection/fixtures/hosted_audit_edges.json").read_text())["audits"]
    nodes = {node for audit in audits for edge in audit["edges"] + audit.get("compatibility_edges", [])
             if edge["check"] == check_id for node in edge["required_nodes"]}
    nonmembers = json.loads((ROOT / "tests/selection/fixtures/nonmember_audit_edges.json").read_text())
    nodes.update(node for edge in nonmembers["check_edges"] if edge["check"] == check_id
                 for node in edge["required_nodes"])
    return nodes


def registrations(policy, profile):
    return [name for check in policy["checks"].values()
            for name in check.get("ctest_tests", {}).get(profile, [])]


class RootProfileContracts(unittest.TestCase):
    def setUp(self):
        from runtime_integration_support import before_runtime_policy
        self.policy = s.load_inventory(INVENTORY)
        self.workflow = ROOT_WORKFLOW.read_text()

    def capture(self, script, runner, environment=None):
        # Execute the actual workflow shell with recording tools. This proves
        # argv/environment parity, never configure/build/test success.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tool = root / "record"
            tool.write_text("#!" + sys.executable + "\nimport json,os,sys\n"
                            "with open(os.environ['CAPTURE'], 'a') as f: "
                            "f.write(json.dumps([os.path.basename(sys.argv[0]), *sys.argv[1:]])+'\\n')\n")
            tool.chmod(0o755)
            for name in ("cmake", "ctest", "sudo", "brew"):
                (root / name).symlink_to(tool)
            envfile = root / "environment"
            envfile.touch()
            (root / "calls").touch()
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"],
                       CAPTURE=str(root / "calls"), GITHUB_ENV=str(envfile),
                       PKG_CONFIG_PATH="", CPPFLAGS="", LDFLAGS="", CMAKE_BUILD_TYPE="Debug")
            env.update(environment or {})
            script = script.replace("${{ runner.os }}", runner).replace("${{ matrix.sanitizer }}", "asan_ubsan")
            subprocess.run(["bash", "-e", "-o", "pipefail", "-c", script], cwd=ROOT, env=env,
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            calls = [json.loads(line) for line in (root / "calls").read_text().splitlines()]
            exported = dict(line.split("=", 1) for line in envfile.read_text().splitlines())
            return calls, exported



    def test_build_audit_test_commands_and_only_existing_exclusions(self):
        for runner, ident in ROOT_PROFILES.items():
            contract = self.policy["profiles"][ident]["workflow_contract"]
            for name, field in [("libnostr audit tests", "audit_command"), ("Test", "test_command")]:
                calls, _ = self.capture(step_script(self.workflow, name), runner)
                self.assertEqual(calls, [[value.replace("{build}", "build") for value in contract[field]]])
            build = re.search(r"      - name: Build \(verbose\)\n        run: (.+)", self.workflow).group(1)
            calls, _ = self.capture(build, runner)
            self.assertEqual(calls, [[value.replace("{build}", "build") for value in contract["build_command"]]])
            self.assertEqual(contract["ctest_exclude"], "test_json_envelope|concurrency_subscription_shutdown_asan|MockRelayUnit")
            self.assertNotIn("main_thread_latency", contract["ctest_exclude"])
            self.assertNotIn("GoChannelStressTest", contract["ctest_exclude"])

    def test_linux_fuzz_phase_retains_all_commands_seeds_and_stricter_environment(self):
        text = step_script(self.workflow, "Short fuzz (libFuzzer)")
        actual = [line.split() for line in text.splitlines() if line.startswith("./build/tests/")]
        contract = self.policy["profiles"]["core.linux.asan"]["workflow_contract"]
        expected = [[arg.replace("{build}", "./build").replace("{source}/", "") for arg in cmd]
                    for cmd in contract["fuzz_commands"]]
        self.assertEqual(actual, expected)
        step = self.workflow.split("      - name: Short fuzz (libFuzzer)", 1)[1]
        for name, value in contract["fuzz_environment"].items():
            self.assertIn("          " + name + ": " + value, step)
        self.assertEqual(contract["fuzz_environment"]["ASAN_OPTIONS"], "detect_leaks=1:strict_string_checks=1")
        native = self.policy["profiles"]["core.macos.asan"]["workflow_contract"]
        self.assertEqual(native["fuzz_commands"], [])
        self.assertIn("if: runner.os == 'Linux' && matrix.sanitizer == 'asan_ubsan'", step)


class ConfiguredMembershipContracts(unittest.TestCase):
    def setUp(self):
        from supported_profile_support import before_supported_batch_policy
        self.policy = s.load_inventory(INVENTORY)
        self.exports = json.loads(EXPORTS.read_text())["exports"]



    def test_poll_consumer_reaches_background_through_observed_runtime_target(self):
        source = "gnome/groundhog/src/ui/gh-poll-ui.c"
        evidence = self.exports["sanitizer_linux_arm64"]["selected_membership"]["groundhog-background"]
        self.assertIn("groundhog", evidence["targets"])
        self.assertIn(source, evidence["primary_source_inputs"])
        self.assertIn("groundhog.poll_ui", evidence["nodes"])
        check = self.policy["checks"][evidence["check"]]
        self.assertIn("groundhog", check["build_targets"])
        plan = s.make_plan(self.policy, [{"status": "M", "old_path": source, "new_path": source}],
                           head="0" * 40, context="local")
        # Other actual consumers: store-sqlite/two-instance launch the app;
        # icons explicitly reads the complete src/ui runtime directory.
        self.assertTrue(set([
            "groundhog.poll_ui", "sanitizer.groundhog_background", "sanitizer.groundhog_icons",
            "sanitizer.groundhog_store_sqlite", "sanitizer.groundhog_two_instance_acceptance"]) <= set(plan["selected_checks"]))

    def test_native_stress_gap_and_all_union_gaps_stay_incomplete(self):
        members = registrations(self.policy, "libgo.macos.asan")
        self.assertEqual(len(set(members)), 32)
        self.assertNotIn("GoChannelStressTest", members)
        gaps = {gap["id"]: gap for gap in self.policy["coverage_gaps"]}
        self.assertEqual(gaps["libgo_macos_asan_stress_exclusion"]["registrations"], ["GoChannelStressTest"])
        self.assertTrue({"supported_profile_union", "shadow_cutover", "workflow_profile_union",
                         "root_asan_execution_contract", "groundhog_sanitizer_execution_contract"} <= set(gaps))
        self.assertFalse(self.policy["enforcement_ready"])


class SharedSanitizerContracts(unittest.TestCase):
    def test_exact_phase_environments_are_not_flattened_into_configure(self):
        policy = s.load_inventory(INVENTORY)
        profile = policy["profiles"]["groundhog.linux.sanitizers"]
        self.assertEqual(profile["environment"], {})
        config = profile["sanitizer_compatibility"]
        phases = profile["phase_environments"]
        self.assertEqual(set(phases), {"configure", "build", "test"})
        for phase, field in [("configure", "GH_SAN_CONFIGURE_ENV"),
                             ("build", "GH_SAN_BUILD_ENV"), ("test", "GH_SAN_ENV")]:
            self.assertEqual(phases[phase], dict(value.split("=", 1) for value in config[field]))
        self.assertEqual(phases["configure"], {})
        self.assertEqual(phases["build"], {})
        self.assertEqual(phases["test"]["ASAN_OPTIONS"], "detect_leaks=1:halt_on_error=1:verify_asan_link_order=0")
        self.assertIn("{source_root}", phases["test"]["LSAN_OPTIONS"])
        self.assertFalse(policy["enforcement_ready"])

    def test_production_profile_equals_helper_json_at_two_source_roots(self):
        for source in (str(ROOT), "/tmp/source root with spaces"):
            with self.subTest(source=source):
                command = [sys.executable, str(SANITIZER_HELPER), "config", "--json", "--source-root", source]
                legacy = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=True)
                shared = subprocess.run(command + ["--shared-profile", "groundhog.linux.sanitizers"],
                                        cwd=ROOT, text=True, capture_output=True, check=True)
                self.assertEqual(json.loads(legacy.stdout), json.loads(shared.stdout))
                config = json.loads(shared.stdout)
                self.assertEqual(config["GH_SAN_SKIP_PATTERN"], r"\*\*\*Skipped|Not Run")
                self.assertIn("detect_leaks=1", " ".join(config["GH_SAN_ENV"]))
                self.assertIn("halt_on_error=1", " ".join(config["GH_SAN_ENV"]))
                self.assertEqual(config["GH_SAN_TEST_JOBS"], "2")

    def test_membership_environment_and_configure_drift_fail_closed(self):
        original = s.load_inventory(INVENTORY)
        for mutation in ("configure", "environment", "membership", "skip_policy"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                policy = copy.deepcopy(original)
                profile = policy["profiles"]["groundhog.linux.sanitizers"]
                if mutation == "configure":
                    profile["configure_args"].append("-DGNOSTR_ENABLE_ASAN=OFF")
                elif mutation == "environment":
                    profile["environment"]["ASAN_OPTIONS"] = "detect_leaks=0"
                elif mutation == "membership":
                    check = next(c for c in policy["checks"].values() if "groundhog.linux.sanitizers" in c["profiles"])
                    check["ctest_tests"]["groundhog.linux.sanitizers"] = ["undefined-registration"]
                else:
                    profile["sanitizer_compatibility"]["GH_SAN_SKIP_PATTERN"] = "never matches"
                inventory = Path(directory) / "inventory.json"
                inventory.write_text(json.dumps(policy))
                result = subprocess.run([sys.executable, str(SANITIZER_HELPER), "config", "--json",
                                         "--shared-profile", "groundhog.linux.sanitizers", "--inventory", str(inventory)],
                                        cwd=ROOT, text=True, capture_output=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn("sanitizer-gate-ci:", result.stderr)


class AuthoredDebugConfigureContracts(Project):
    # Project only the authored configure args/build type onto the mini-project;
    # this does not establish Linux/compiler/capability or production admission.
    def use_authored_configure(self, ident, variant):
        wrapper = self.root / 'scripts/mdk-docker'
        wrapper.parent.mkdir(exist_ok=True)
        shutil.copy2(ROOT / 'scripts/mdk-docker', wrapper)
        authored = s.load_inventory(INVENTORY)["profiles"][ident]
        self.profile = {"source_dir": ".", "environment": {}, "capabilities": [],
                        "build_type": authored["build_type"],
                        "configure_args": list(authored["configure_args"])}
        self.build = Path(self.temp.name) / (ident + "-" + variant)

    def test_debug_profiles_realize_their_authored_configuration(self):
        for ident in ("core.linux", "groundhog.mdk08"):
            with self.subTest(profile=ident):
                self.use_authored_configure(ident, "debug")
                self.assertEqual(self.profile["build_type"], "Debug")
                self.assertEqual([arg for arg in self.profile["configure_args"]
                                  if arg.startswith("-DCMAKE_BUILD_TYPE=")],
                                 ["-DCMAKE_BUILD_TYPE=Debug"])
                realized = self.configure()
                self.assertIn("CMAKE_BUILD_TYPE:STRING=Debug\n",
                              (self.build / "CMakeCache.txt").read_text())
                self.assertEqual(realized["configuration"], "Debug")
                self.assertEqual(realized["phase_context"]["configuration"], "Debug")
                self.assertEqual(realized["profile"], self.profile)
                self.assertEqual(r.load_realization(self.build, self.identity), realized)

    def test_missing_or_contradictory_configure_type_still_fails_closed(self):
        for ident in ("core.linux", "groundhog.mdk08"):
            for variant in ("missing", "Release"):
                with self.subTest(profile=ident, variant=variant):
                    self.use_authored_configure(ident, variant)
                    args = self.profile["configure_args"]
                    args.remove("-DCMAKE_BUILD_TYPE=Debug")
                    if variant == "Release":
                        args.append("-DCMAKE_BUILD_TYPE=Release")
                    # Do not let an ambient CMake default conceal the missing flag.
                    with mock.patch.dict(os.environ, {"CMAKE_BUILD_TYPE": ""}), self.assertRaisesRegex(
                            r.RealizationError, "profile build type Debug differs from realized"):
                        self.configure()
                    self.assertFalse((self.build / "nostrc-test-realization.json").exists())


class OrdinaryLiveProfileContracts(unittest.TestCase):
    def test_ordinary_groundhog_has_no_mdk_live_registration_or_capability(self):
        from runtime_integration_support import before_runtime_policy
        policy = s.load_inventory(INVENTORY)
        ordinary = policy['profiles']['groundhog.linux']
        live = policy['profiles']['groundhog.mdk011']
        self.assertEqual(ordinary['capabilities'], ['display', 'private_bus', 'nak', 'gnome_keyring', 'signer_endpoint'])
        self.assertEqual(ordinary['environment']['NOSTRC_TEST_NIP5F_APPROVAL'], 'unidentified-get-public-key-once')
        self.assertNotIn('docker', ordinary['capabilities'])
        self.assertEqual(live['capabilities'], ['display', 'private_bus', 'docker', 'nak', 'gnome_keyring', 'signer_endpoint'])
        self.assertIn('-DBUILD_MDK011_INTEROP=OFF', ordinary['configure_args'])
        self.assertIn('-DBUILD_MDK011_INTEROP=ON', live['configure_args'])
        for ident, check in policy['checks'].items():
            if ident.startswith('mdk011.'):
                self.assertEqual(check['profiles'], ['groundhog.mdk011'])
                self.assertEqual(set(check['ctest_tests']), {'groundhog.mdk011'})
        self.assertTrue(registrations(policy, 'groundhog.mdk011'))
        self.assertFalse(any('mdk' in name for name in registrations(policy, 'groundhog.linux')))

    def test_non_mls_changes_do_not_select_live_checks_but_mls_still_does(self):
        policy = s.load_inventory(INVENTORY)
        for path in ('apps/grotto/src/ui/main-window.c',
                     'apps/grotto/daemon/nostr-signer-daemon.c',
                     '.github/workflows/grotto-ci.yml',
                     'gnome/groundhog/src/ui/gh-about-dialog.c',
                     'gnome/groundhog/src/ui/gh-poll-ui.c'):
            with self.subTest(path=path):
                plan = s.make_plan(policy, [change(path)], head='a' * 40, base='b' * 40, context='ci')
                self.assertEqual(plan['effective_mode'], 'affected')
                self.assertFalse(any(ident.startswith(('mdk011.', 'mdk08.', 'prepared011.'))
                                     for ident in plan['selected_checks']))
                # Ancillary registrations can share an MDK configure profile;
                # they must never acquire a Docker peer/fixture requirement.
                for ident in plan['selected_checks']:
                    self.assertNotIn('docker', policy['checks'][ident].get('requires', []))
        for path in ('gnome/groundhog/src/mls/gh-mls-service.c',
                     'gnome/groundhog/src/ui/gh-mls-copy.c'):
            with self.subTest(path=path):
                plan = s.make_plan(policy, [change(path)], head='a' * 40, base='b' * 40, context='ci')
                self.assertIn('mdk08.compatibility', plan['selected_checks'])
                self.assertTrue(any(ident.startswith('mdk011.') for ident in plan['selected_checks']))
                self.assertIn('groundhog.mdk011', plan['expected_lane_ids'])


class WorkflowMatrixAccounting(unittest.TestCase):
    def test_every_existing_job_matrix_has_a_mapping_or_explicit_gap(self):
        policy = s.load_inventory(INVENTORY)
        observed = validate_job_accounting(policy, ROOT)
        self.assertEqual(set(observed), set(policy['workflow_jobs']))
        for name in ('select', 'core', 'groundhog', 'gate', 'rust', 'vault_syncd'):
            self.assertIn('.github/workflows/ci-selective.yml:' + name, observed)
        self.assertIn('.github/workflows/dependency-tests.yml:homed_units', observed)
        self.assertIn('.github/workflows/namespace-policy-regression.yml:regressions', observed)
        self.assertIn('.github/workflows/dependency-tests.yml:policy_regressions', observed)
        unsupported = [key for key, record in policy["workflow_jobs"].items() if not record["profiles"]]
        self.assertTrue(unsupported)
        for key in unsupported:
            self.assertEqual(policy["workflow_jobs"][key]["coverage_gap"], "workflow_profile_union")

    def test_commented_quoted_job_headers_and_inline_strategies_cannot_disappear(self):
        for header in ("  new-job: # explanation", '  "new-job":', "  'new-job':"):
            with self.subTest(header=header), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                workflows = root / ".github/workflows"
                workflows.mkdir(parents=True)
                path = workflows / "new.yaml"
                path.write_text("jobs:\n" + header + "\n    runs-on: ubuntu-24.04\n")
                self.assertEqual(source_jobs(root), {".github/workflows/new.yaml:new-job": ""})
                path.write_text("jobs:\n" + header + "\n    strategy: {matrix: {os: [ubuntu-latest]}}\n")
                with self.assertRaisesRegex(ValueError, "inline strategy"):
                    source_jobs(root)
                path.write_text("jobs:\n" + header + "\n    strategy:\n      'matrix': {os: [ubuntu-latest]}\n")
                self.assertIn("matrix", source_jobs(root)[".github/workflows/new.yaml:new-job"])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".github/workflows").mkdir(parents=True)
            (root / ".github/workflows/anchor.yml").write_text("jobs:\n  new-job: *template\n")
            with self.assertRaisesRegex(ValueError, "unsupported job header"):
                source_jobs(root)

    def test_unmapped_job_matrix_drift_and_missing_gap_are_not_waived(self):
        original = s.load_inventory(INVENTORY)
        for mutation in ("job", "matrix", "gap"):
            with self.subTest(mutation=mutation):
                policy = copy.deepcopy(original)
                key = next(iter(policy["workflow_jobs"]))
                if mutation == "job":
                    del policy["workflow_jobs"][key]
                elif mutation == "matrix":
                    policy["workflow_jobs"][key]["matrix_source"] += "unexpected dimension"
                else:
                    policy["workflow_jobs"][key]["coverage_gap"] = "undefined-gap"
                with self.assertRaises(ValueError):
                    validate_job_accounting(policy, ROOT)


if __name__ == "__main__":
    unittest.main()
