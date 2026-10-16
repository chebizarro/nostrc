"""Profile fidelity and bounded configured membership; not live execution proof."""
import copy
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

from selector_support import ROOT, s
from profile_contract_support import source_jobs, step_script, validate_job_accounting

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

    def test_two_actual_matrix_entries_have_distinct_faithful_profiles(self):
        key = ".github/workflows/nostrc-ci.yml:build-and-test"
        observed = source_jobs(ROOT)[key]
        self.assertEqual(observed, "matrix:\n        os: [ubuntu-latest, macos-latest]\n        sanitizer: [asan_ubsan]")
        self.assertEqual(set(self.policy["workflow_jobs"][key]["profiles"]), set(ROOT_PROFILES.values()))
        for runner, ident in ROOT_PROFILES.items():
            with self.subTest(runner=runner):
                profile = self.policy["profiles"][ident]
                calls, _ = self.capture(step_script(self.workflow, "Configure (sanitizers + ENABLE_NIP19/49 + fuzzers)"), runner)
                self.assertEqual(len(calls), 1)
                self.assertEqual(calls[0][:5], ["cmake", "-S", ".", "-B", "build"])
                self.assertEqual(calls[0][5:], profile["configure_args"])
                self.assertEqual(profile["compiler"], "clang")
                self.assertEqual(profile["instrumentation"], "asan_ubsan")
                self.assertEqual(profile["source_dir"], ".")
                self.assertEqual(profile["setup"], key)
                self.assertEqual(profile["workflow_contract"]["matrix"]["os"],
                                 "ubuntu-latest" if runner == "Linux" else "macos-latest")
                self.assertEqual(profile["workflow_contract"]["matrix"]["sanitizer"], "asan_ubsan")
                self.assertEqual(profile["architecture"], "x86_64" if runner == "Linux" else "arm64")

    def test_effective_job_environment_and_runner_setup_defaults(self):
        job_env = self.workflow.split("    env:\n", 1)[1].split("    steps:\n", 1)[0]
        expected = {}
        for line in job_env.splitlines():
            match = re.fullmatch(r"      ([A-Z][A-Z0-9_]*): (.+)", line)
            if match:
                expected[match[1]] = match[2].strip('"')
        for runner, ident in ROOT_PROFILES.items():
            with self.subTest(runner=runner):
                environment = dict(expected)
                if runner == "Linux":
                    names = ["Select clang toolchain (Ubuntu)", "Install dependencies (Ubuntu)"]
                else:
                    names = ["Install dependencies (macOS)", "Override ASAN options (macOS)"]
                for name in names:
                    _, exported = self.capture(step_script(self.workflow, name), runner)
                    environment.update(exported)
                self.assertEqual(self.policy["profiles"][ident]["environment"], environment)

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
        self.policy = s.load_inventory(INVENTORY)
        self.exports = json.loads(EXPORTS.read_text())["exports"]

    def test_root_membership_comes_from_actual_source_owner_exports(self):
        for key in ("root_macos_arm64", "root_linux_arm64"):
            export = self.exports[key]
            profile = export["profile"]
            members = registrations(self.policy, profile)
            expected = [name for names in export["source_owner_membership"].values() for name in names]
            self.assertEqual(len(members), len(set(members)))
            self.assertEqual(set(members), set(expected))
            captured = set(export["registrations"])
            fixtures = set(export["fixture_registrations"])
            exclusions = set(export["legacy_excluded_registrations"])
            self.assertFalse(fixtures & exclusions)
            self.assertFalse(set(members) & (fixtures | exclusions))
            self.assertEqual(set(members), captured - fixtures - exclusions)
            self.assertEqual(captured, set(members) | fixtures | exclusions)
            self.assertEqual(len(members), 267 if key == "root_macos_arm64" else 268)
            self.assertEqual(self.policy["profiles"][profile]["configure_args"], export["configure_args"])
            for owners, names in export["source_owner_membership"].items():
                for check_id, check in self.policy["checks"].items():
                    if set(check.get("ctest_tests", {}).get(profile, [])) & set(names):
                        self.assertEqual(set(check["nodes"]), set(owners.split(",")) | {"execution.core"} | audited_nodes(check_id))
            self.assertFalse(set(members) & set(export["fixture_registrations"]))
            self.assertEqual(export["legacy_excluded_registrations"],
                             ["MockRelayUnit", "concurrency_subscription_shutdown_asan", "test_json_envelope"])
            self.assertFalse(set(members) & set(export["legacy_excluded_registrations"]))
            self.assertIn("test_nostr_gobject_main_thread_latency", members)
            self.assertIn("GoChannelStressTest", members)
            for field in ("ctest_sha256", "descriptors_sha256"):
                self.assertRegex(export[field], r"^[0-9a-f]{64}$")
        self.assertTrue(self.exports["root_macos_arm64"]["matches_declared_architecture"])
        self.assertFalse(self.exports["root_linux_arm64"]["matches_declared_architecture"])
        self.assertEqual(self.exports["root_linux_arm64"]["actual_architecture"], "arm64")
        self.assertEqual(self.policy["profiles"]["core.linux.asan"]["architecture"], "x86_64")

    def test_sanitizer_members_are_observed_and_have_source_grounded_target_inputs(self):
        export = self.exports["sanitizer_linux_arm64"]
        profile = "groundhog.linux.sanitizers"
        actual = registrations(self.policy, profile)
        observed = export["selected_membership"]
        self.assertEqual(len(actual), 62)
        self.assertEqual(len(set(actual)), len(actual))
        self.assertEqual(set(actual), set(observed))
        self.assertTrue(set(actual) <= set(export["registrations"]))
        for name, evidence in observed.items():
            check = self.policy["checks"][evidence["check"]]
            self.assertEqual(check["ctest_tests"][profile], [name])
            self.assertEqual(check["build_targets"], evidence["targets"])
            self.assertEqual(set(check["nodes"]), set(evidence["nodes"]) | {"execution.groundhog"} | audited_nodes(evidence["check"]))
            for source in evidence["primary_source_inputs"]:
                self.assertTrue(set(s.classify(self.policy, source)["nodes"]) <= set(check["nodes"]))
        self.assertFalse(export["matches_declared_architecture"])

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
        self.assertEqual(plan["selected_checks"], [
            "groundhog.poll_ui", "sanitizer.groundhog_background", "sanitizer.groundhog_icons",
            "sanitizer.groundhog_store_sqlite", "sanitizer.groundhog_two_instance_acceptance"])

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


class WorkflowMatrixAccounting(unittest.TestCase):
    def test_every_existing_job_matrix_has_a_mapping_or_explicit_gap(self):
        policy = s.load_inventory(INVENTORY)
        observed = validate_job_accounting(policy, ROOT)
        self.assertEqual(len(observed), 87)
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
