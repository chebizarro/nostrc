"""Phase custody: real miniature CMake builds, not production sanitizer proof."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

from realization_support import Project, ROOT, r
from selector_support import s


def phases():
    return {"configure": {}, "build": {}, "test": {}}


class PhaseContractTests(unittest.TestCase):
    def test_common_then_only_requested_phase_overlay(self):
        profile = {"environment": {"SCOPE": "common", "COMMON": "yes"},
                   "phase_environments": phases()}
        profile["phase_environments"]["test"] = {"SCOPE": "test", "TEST_ONLY": "", "FILE": "{source_root}/file"}
        for phase in r.PHASES:
            result = r.phase_environment(profile, "/checkout with spaces", phase, {"AMBIENT": "yes"})
            self.assertEqual(result["COMMON"], "yes")
            self.assertEqual(result["AMBIENT"], "yes")
            self.assertEqual(result["SCOPE"], "test" if phase == "test" else "common")
            self.assertEqual("TEST_ONLY" in result, phase == "test")
        self.assertEqual(result["FILE"], "/checkout with spaces/file")
        self.assertEqual(profile["phase_environments"]["test"]["FILE"], "{source_root}/file")

    def test_unknown_missing_or_nonmap_phases_are_rejected(self):
        for bad in (None, [], "test", {}, {"configure": {}, "build": {}},
                    dict(phases(), other={}), dict(phases(), test=[])):
            with self.subTest(value=bad), self.assertRaises(r.RealizationError):
                r.phase_environment({"phase_environments": bad}, "/source", "test", {})
        for phase in ("all", "configuree", "", None):
            with self.subTest(phase=phase), self.assertRaises(r.RealizationError):
                r.phase_environment({}, "/source", phase, {})

    def test_malformed_common_and_phase_environment_values_fail_closed(self):
        for bad in ({"": "x"}, {"A=B": "x"}, {"A\0": "x"}, {1: "x"}, {"A": 1},
                    {"A": None}, {"A": False}, {"A": []}, {"A": "x\0"},
                    {"A": "{source}/file"}, {"A": "{source_root}/file/{bad}"}):
            for field in ("environment", "phase_environments"):
                profile = {field: bad if field == "environment" else dict(phases(), test=bad)}
                with self.subTest(field=field, bad=bad), self.assertRaises(r.RealizationError):
                    r.validate_phase_profile(profile)

    def test_actual_sanitizer_metadata_is_admitted_only_consistently(self):
        profile = json.loads((ROOT / "tests/selection/inventory.json").read_text())["profiles"]["groundhog.linux.sanitizers"]
        r.validate_phase_profile(profile)  # no host/coverage claim
        for field, bad in (("GH_SAN_CONFIGURE", []), ("GH_SAN_CONFIGURE_ENV", ["ASAN_OPTIONS=x"]),
                           ("GH_SAN_BUILD_ENV", ["ASAN_OPTIONS=x"]), ("GH_SAN_ENV", []),
                           ("GH_SAN_TEST_JOBS", "99"), ("GH_SAN_TARGETS", ["duplicate", "duplicate"]),
                           ("GH_SAN_TESTS", []), ("GH_SAN_SKIP_PATTERN", ".*"), ("unknown", [])):
            value = copy.deepcopy(profile)
            value["sanitizer_compatibility"][field] = bad
            with self.subTest(field=field), self.assertRaises(r.RealizationError):
                r.validate_phase_profile(value)
        value = copy.deepcopy(profile)
        value["sanitizer_compatibility"]["GH_SAN_ENV"].append(value["sanitizer_compatibility"]["GH_SAN_ENV"][0])
        with self.assertRaisesRegex(r.RealizationError, "duplicate"):
            r.validate_phase_profile(value)

    def test_inventory_validation_rejects_malformed_phase_metadata(self):
        policy = json.loads((ROOT / "tests/selection/inventory.json").read_text())
        policy["profiles"]["groundhog.linux.sanitizers"]["phase_environments"]["unknown"] = {}
        with self.assertRaisesRegex(s.ContractError, "exactly configure/build/test"):
            s.validate_inventory(policy)


class PhaseExecutionTests(Project):
    def setUp(self):
        super().setUp()
        ambient = {k: v for k, v in os.environ.items() if k not in
                   ("ASAN_OPTIONS", "LSAN_OPTIONS", "PHASE_TEST_ONLY", "PHASE_BUILD_ONLY", "PHASE_SCOPE", "PHASE_COMMON")}
        self.environment = mock.patch.dict(os.environ, ambient, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.profile["configure_args"] += ["-DCMAKE_BUILD_TYPE=Debug"]
        self.profile["environment"] = {"PHASE_COMMON": "common", "PHASE_SCOPE": "common"}
        self.profile["phase_environments"] = {
            "configure": {"PHASE_SCOPE": "configure"},
            "build": {"PHASE_SCOPE": "build", "PHASE_BUILD_ONLY": "yes"},
            "test": {"PHASE_SCOPE": "test", "PHASE_TEST_ONLY": "yes", "ASAN_OPTIONS": "detect_leaks=1",
                     "LSAN_OPTIONS": "suppressions={source_root}/vector.txt:print_suppressions=1"}}
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text() + '''
if(NOT "$ENV{PHASE_SCOPE}" STREQUAL "configure" OR NOT "$ENV{PHASE_COMMON}" STREQUAL "common")
  message(FATAL_ERROR "wrong configure environment")
endif()
if(DEFINED ENV{PHASE_TEST_ONLY} OR DEFINED ENV{ASAN_OPTIONS} OR DEFINED ENV{LSAN_OPTIONS} OR DEFINED ENV{PHASE_BUILD_ONLY})
  message(FATAL_ERROR "phase overlay leaked into configure")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/configure-phase.txt" "$ENV{PHASE_SCOPE}")
add_custom_command(TARGET shared PRE_BUILD COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/phase-build.cmake)
''')
        (self.root / "phase-build.cmake").write_text('''
if(NOT "$ENV{PHASE_SCOPE}" STREQUAL "build" OR NOT "$ENV{PHASE_COMMON}" STREQUAL "common" OR NOT "$ENV{PHASE_BUILD_ONLY}" STREQUAL "yes")
  message(FATAL_ERROR "wrong build environment")
endif()
if(DEFINED ENV{PHASE_TEST_ONLY} OR DEFINED ENV{ASAN_OPTIONS} OR DEFINED ENV{LSAN_OPTIONS})
  message(FATAL_ERROR "test overlay leaked into build")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/build-phase.txt" "$ENV{PHASE_SCOPE}")
''')
        code = self.root / "main.c"
        code.write_text(code.read_text().replace('  FILE *f', '''  if (!getenv("PHASE_SCOPE") || strcmp(getenv("PHASE_SCOPE"), "test") ||
      !getenv("PHASE_TEST_ONLY") || getenv("PHASE_BUILD_ONLY") ||
      !getenv("ASAN_OPTIONS") || !getenv("LSAN_OPTIONS") ||
      !strstr(getenv("LSAN_OPTIONS"), "/vector.txt:print_suppressions=1")) return 3;
  FILE *f'''))
        self.inventory["path_rules"].append({"exact": "phase-build.cmake", "nodes": [], "effect": "global"})

    def ready(self):
        realized = self.configure()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        r.build_resolved(resolved, self.build)
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        return realized, resolved, audit

    def test_real_configure_build_and_single_ctest_use_distinct_environments(self):
        realized, resolved, audit = self.ready()
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(receipt["outcome"], "passed", receipt)
        self.assertEqual((self.build / "configure-phase.txt").read_text(), "configure")
        self.assertEqual((self.build / "build-phase.txt").read_text(), "build")
        self.assertEqual(len([x for x in receipt["commands"] if x[0] == "ctest"]), 1)
        context = realized["phase_context"]
        self.assertEqual(context["configuration"], "Debug")
        self.assertEqual(context["source_root"], str(self.root))
        self.assertIn(str(self.root / "vector.txt"), context["template_inputs"])
        self.assertIn(str(self.root / "vector.txt"), audit["input_hashes"])
        self.assertEqual(len(set(context["environment_hashes"].values())), 3)
        self.assertNotIn("LSAN_OPTIONS", context["environment_overlays"]["configure"])
        self.assertNotIn("LSAN_OPTIONS", context["environment_overlays"]["build"])
        for record in (resolved, audit, receipt):
            self.assertEqual(record["phase_context"], context)
            self.assertEqual(record["phase_context_hash"], realized["phase_context_hash"])
        self.assertNotIn("PHASE_SCOPE", os.environ)

    def test_real_cli_all_and_cached_test_phase(self):
        self.refresh_identity()
        self.inventory["profiles"] = {"synthetic": self.profile}
        self.plan["inventory_hash"] = s.digest(self.inventory)
        self.plan.update(schema_version=1, head=self.identity["sha"], tested_sha=self.identity["sha"], expected_lane_ids=["synthetic"])
        self.plan["lanes"] = [{"id": "synthetic", "profile": "synthetic", "profile_hash": s.digest(self.profile), "checks": self.plan["selected_checks"]}]
        self.plan["plan_id"] = s.digest(self.plan)
        path = self.root.parent / "plan.json"
        r._atomic(path, self.plan)
        spec = importlib.util.spec_from_file_location("phase_cli", ROOT / "scripts/test-plan.py")
        cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cli)
        args = SimpleNamespace(plan=str(path), profile="synthetic", repo=str(self.root),
                               configure=True, phase="all", build_dir=str(self.build), evidence_dir=str(self.evidence),
                               command="run", resolved=None, parallel=1, timeout=None, attempt=1, evidence_path=[])
        receipt, code = cli.realization_command(args, self.inventory, r)
        self.assertEqual(code, 0, receipt)
        self.assertEqual(receipt["phase_context_hash"], receipt["backend_receipt"]["phase_context_hash"])
        args.configure, args.phase, args.attempt = False, "test", 2
        receipt, code = cli.realization_command(args, self.inventory, r)
        self.assertEqual(code, 0, receipt)
        self.assertEqual(len([c for c in receipt["backend_receipt"]["commands"] if c[0] == "ctest"]), 1)

    def test_phase_or_configuration_mutation_is_rejected_at_resolution_and_build(self):
        realized, resolved, _ = self.ready()
        for field, bad in (("configuration", "Release"), ("phase_context_hash", "different"),
                           ("phase_context", {}), ("profile_hash", "different")):
            changed = copy.deepcopy(resolved)
            changed[field] = bad
            with self.subTest(field=field), self.assertRaisesRegex(r.RealizationError, "phase identity mismatch"):
                r.build_resolved(changed, self.build)
        realized["profile"]["phase_environments"]["test"]["ASAN_OPTIONS"] = "detect_leaks=0"
        with self.assertRaisesRegex(r.RealizationError, "environment changed"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")

    def test_cached_profile_phase_or_source_mutation_needs_reconfigure(self):
        realized = self.configure()
        for mutate in (lambda x: x["profile"]["phase_environments"]["test"].update(ASAN_OPTIONS="other"),
                       lambda x: x["profile"]["phase_environments"].update(unknown={}),
                       lambda x: x.update(configuration="Release"),
                       lambda x: x["phase_context"].update(source_root="/other"),
                       lambda x: x.pop("phase_context"),
                       lambda x: x.update(source_dir=str(self.root.parent))):
            value = copy.deepcopy(realized)
            mutate(value)
            r._atomic(self.build / "nostrc-test-realization.json", value)
            with self.assertRaises(r.RealizationError):
                r.load_realization(self.build, self.identity)
        r._atomic(self.build / "nostrc-test-realization.json", realized)
        self.assertEqual(r.load_realization(self.build, self.identity), realized)

    def test_ambient_drift_is_rejected_before_build_and_test(self):
        _, resolved, audit = self.ready()
        with mock.patch.dict(os.environ, {"PHASE_UNDECLARED_DRIFT": "changed"}):
            with self.assertRaisesRegex(r.RealizationError, "environment changed"):
                r.build_resolved(resolved, self.build)
            with self.assertRaisesRegex(r.RealizationError, "environment changed"):
                r.run_resolved(resolved, self.build, self.evidence, input_audit=audit)
        self.assertFalse(list(self.evidence.rglob("results.xml")))

    def test_overlay_and_audit_drift_cannot_reuse_cached_pass(self):
        realized, resolved, audit = self.ready()
        changed = copy.deepcopy(audit)
        changed["phase_context"]["environment_overlays"]["test"]["ASAN_OPTIONS"] = "different"
        changed["audit_hash"] = r._hash({k: v for k, v in changed.items() if k != "audit_hash"})
        with self.assertRaisesRegex(r.RealizationError, "audit identity mismatch: phase_context"):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=changed)
        receipt = r._read(next(self.evidence.rglob("receipt.json")))
        self.assertEqual(receipt["outcome"], "failed")
        self.assertFalse(receipt["diagnostic_eligible"])
        self.assertEqual(receipt["phase_context_hash"], realized["phase_context_hash"])

    def test_template_mismatch_escape_missing_and_unknown_fail_before_commands(self):
        for value in ("{source_root}/missing", "{source_root}/../escape", "{build}/file", "{source_root}/vector.txt/{other}"):
            self.profile["phase_environments"]["test"]["LSAN_OPTIONS"] = value
            with self.subTest(value=value), mock.patch.object(r, "_command") as command:
                with self.assertRaises(r.RealizationError):
                    self.configure()
                command.assert_not_called()

    def test_template_input_ownership_and_source_bytes_are_audited(self):
        realized, resolved, audit = self.ready()
        self.inventory["path_rules"] = [x for x in self.inventory["path_rules"] if x.get("exact") != "vector.txt"]
        failed = self.audit(realized)
        self.assertTrue(any(g["kind"] == "unowned_profile_input" for g in failed["gaps"]))
        (self.root / "vector.txt").write_bytes(b"changed suppression bytes\xff")
        with self.assertRaisesRegex(r.RealizationError, "source snapshot digest changed"):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit)

    def test_failed_or_cancelled_reconfigure_invalidates_cached_realization(self):
        for failure in (KeyboardInterrupt("configure cancelled"), r.RealizationError("configure failure")):
            self.configure()
            execute = r._command
            def fail(argv, **kwargs):
                if argv[0] == "cmake":
                    raise failure
                return execute(argv, **kwargs)
            with mock.patch.object(r, "_command", side_effect=fail), self.assertRaises(type(failure)):
                r.configure_profile(self.root, self.build, "synthetic", self.profile, self.identity, "inventory-fixture")
            self.assertFalse((self.build / "nostrc-test-realization.json").exists())

    def test_test_cancellation_retains_phase_identity_and_negative_eligibility(self):
        realized, resolved, audit = self.ready()
        with mock.patch.object(r, "_execute", side_effect=KeyboardInterrupt("phase cancellation")):
            with self.assertRaisesRegex(r.RealizationError, "interrupted/failed"):
                r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        receipt = r._read(next(self.evidence.rglob("receipt.json")))
        self.assertEqual(receipt["outcome"], "cancelled")
        self.assertEqual(receipt["phase_context_hash"], realized["phase_context_hash"])
        self.assertFalse(receipt["complete_gate_eligible"])
        self.assertFalse(receipt["diagnostic_eligible"])

    def test_real_build_cancellation_retains_raw_output_and_phase_hash(self):
        fifo = self.root.parent / "build-ready.fifo"
        os.mkfifo(fifo)
        self.profile["phase_environments"]["build"]["PHASE_FIFO"] = str(fifo)
        pause = self.root / "pause.py"
        pause.write_text('''import os, signal, sys
assert os.environ["PHASE_SCOPE"] == "build"
assert "ASAN_OPTIONS" not in os.environ and "PHASE_TEST_ONLY" not in os.environ
sys.stdout.buffer.write(b"build-output-\\xff\\n")
sys.stdout.buffer.flush()
with open(os.environ["PHASE_FIFO"], "w") as notify:
    notify.write("ready\\n")
    notify.flush()
signal.pause()
''')
        with (self.root / "CMakeLists.txt").open("a") as stream:
            stream.write(f'\nadd_custom_command(TARGET shared PRE_BUILD COMMAND "{sys.executable}" "{pause}")\n')
        self.inventory["path_rules"].append({"exact": "pause.py", "nodes": [], "effect": "global"})
        self.profile.update(platform={"Darwin": "macos", "Linux": "linux"}.get(r.platform.system(), r.platform.system().lower()),
                            architecture="native", compiler="clang" if "clang" in r._command(["cc", "--version"]).lower() else "gcc",
                            build_type="Debug", instrumentation="none", setup="miniature", local_availability="host")
        for name, node in self.inventory["nodes"].items():
            node.update(owner_paths=[name], rationale="miniature source dependency")
        for rule in self.inventory["path_rules"]:
            rule.setdefault("effect", "impact")
            rule["reason"] = "miniature source input"
        for check in self.inventory["checks"].values():
            check.update(cost="ordinary", requirement="blocking", expected_outcome="pass")
        self.inventory["checks"]["a_model"] = self.inventory["checks"].pop("a-model")
        self.plan["selected_checks"] = ["a_model"]
        self.inventory.update(schema_version=1, release_components={}, test_overrides=[], profiles={"synthetic": self.profile})
        s.validate_inventory(self.inventory)
        self.refresh_identity()
        self.plan.update(schema_version=1, head=self.identity["sha"], tested_sha=self.identity["sha"],
                         expected_lane_ids=["synthetic"], inventory_hash=s.digest(self.inventory))
        self.plan["lanes"] = [{"id": "synthetic", "profile": "synthetic", "profile_hash": s.digest(self.profile), "checks": self.plan["selected_checks"]}]
        self.plan["plan_id"] = s.digest(self.plan)
        planfile, policyfile = self.root.parent / "plan.json", self.root.parent / "policy.json"
        r._atomic(planfile, self.plan)
        r._atomic(policyfile, self.inventory)
        realized = r.configure_profile(self.root, self.build, "synthetic", self.profile, self.identity, s.digest(self.inventory))
        command = [sys.executable, str(ROOT / "scripts/test-plan.py"), "run", "--repo", str(self.root),
                   "--inventory", str(policyfile), "--profile", "synthetic", "--plan", str(planfile),
                   "--build-dir", str(self.build), "--evidence-dir", str(self.evidence), "--phase", "build", "--timeout", "20"]
        descriptor = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        self.addCleanup(os.close, descriptor)
        with selectors.DefaultSelector() as selector:
            selector.register(descriptor, selectors.EVENT_READ)
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            try:
                self.assertTrue(selector.select(20), "real build never reached phase-ready boundary")
                self.assertEqual(os.read(descriptor, 64), b"ready\n")
                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=20)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=20)
        self.assertNotEqual(process.returncode, 0, output)
        statusfile = next(self.evidence.rglob("*.process.json"))
        status = r._read(statusfile)
        self.assertEqual(status["outcome"], "cancelled")
        self.assertEqual(status["environment_hash"], realized["phase_context"]["environment_hashes"]["build"])
        # Ninja emits the compile progress before entering the ready link step;
        # the paused command's own stdout is still buffered inside Ninja.
        self.assertIn(b"Building C object CMakeFiles/shared.dir/main.c.o", Path(str(statusfile).removesuffix(".process.json")).read_bytes())
        self.assertFalse(list(self.evidence.rglob("results.xml")))

    def test_real_command_cancellation_defaults_to_authored_timeout_and_retains_bytes(self):
        fifo = self.root.parent / "ctest-ready.fifo"
        os.mkfifo(fifo)
        self.profile["timeout_seconds"] = 20
        self.profile["phase_environments"]["test"]["PHASE_FIFO"] = str(fifo)
        (self.root / "pause.py").write_text('''import os, signal, sys
assert os.environ["PHASE_SCOPE"] == "test" and "PHASE_BUILD_ONLY" not in os.environ
sys.stdout.buffer.write(b"ctest-output-\\xff\\n")
sys.stdout.buffer.flush()
with open(os.environ["PHASE_FIFO"], "w") as notify:
    notify.write("ready\\n")
    notify.flush()
signal.pause()
''')
        self.inventory["path_rules"].append({"exact": "pause.py", "nodes": [], "effect": "global"})
        self.inventory["checks"]["phase_pause"] = {"kind": "command", "nodes": ["application", "testing"],
            "profiles": ["synthetic"], "command": ["python3", "{source}/pause.py"],
            "working_directory": "{build}", "environment": {}, "runtime_inputs": ["{source}/pause.py"],
            "build_targets": ["shared"], "requires": [], "required_evidence": []}
        self.plan["selected_checks"] = ["a-model", "phase_pause"]
        realized, resolved, audit = self.ready()
        resolvedfile, auditfile = self.root.parent / "resolved.json", self.root.parent / "audit.json"
        r._atomic(resolvedfile, resolved)
        r._atomic(auditfile, audit)
        driver = '''import sys
sys.path.insert(0, sys.argv[1])
import test_selection_realization as r
r.run_resolved(r._read(sys.argv[2]), sys.argv[3], sys.argv[4], input_audit=r._read(sys.argv[5]), build=False)
'''
        descriptor = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        self.addCleanup(os.close, descriptor)
        with selectors.DefaultSelector() as selector:
            selector.register(descriptor, selectors.EVENT_READ)
            process = subprocess.Popen([sys.executable, "-c", driver, str(ROOT / "scripts"),
                str(resolvedfile), str(self.build), str(self.evidence), str(auditfile)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            try:
                self.assertTrue(selector.select(20), "CTest never reached phase-ready boundary")
                self.assertEqual(os.read(descriptor, 64), b"ready\n")
                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=20)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=20)
        self.assertNotEqual(process.returncode, 0, output)
        receipt = r._read(next(self.evidence.rglob("receipt.json")))
        self.assertEqual(receipt["outcome"], "cancelled")
        self.assertFalse(receipt["diagnostic_eligible"])
        self.assertFalse(receipt["complete_gate_eligible"])
        self.assertEqual(receipt["phase_context_hash"], realized["phase_context_hash"])
        command = next(c for c in receipt["commands"] if c[0] == "ctest")
        self.assertEqual(command[-2:], ["--timeout", "20"])
        statusfile = next(self.evidence.rglob("command-phase_pause.log.process.json"))
        self.assertEqual(r._read(statusfile)["outcome"], "cancelled")
        self.assertEqual(r._read(statusfile)["environment_hash"], realized["phase_context"]["environment_hashes"]["test"])
        self.assertIn(b"ctest-output-\xff", (statusfile.parent / "command-phase_pause.log").read_bytes())

    def test_malformed_cli_phase_does_not_fall_through_to_execution(self):
        args = SimpleNamespace(command="run", phase="unknown")
        self.assertEqual(r.cli(args, {}), 1)

    def test_configuration_request_mismatch_fails_closed(self):
        self.profile["configuration"] = "Release"
        with self.assertRaisesRegex(r.RealizationError, "configuration differs"):
            self.configure()

    def test_existing_compatibility_metadata_preserved_in_real_cmake_context(self):
        tests = ["a-model", "gui", "omitted"]
        (self.root / "app.c").write_text('#include "shared.h"\nint main(void) { return SHARED == 42 ? 0 : 1; }\n')
        with (self.root / "CMakeLists.txt").open("a") as stream:
            stream.write("\nadd_executable(compat-app app.c)\n")
        self.inventory["path_rules"].append({"exact": "app.c", "nodes": ["application"]})
        self.profile["parallelism"] = 1
        self.profile["phase_environments"]["test"]["GROUNDHOG_SANITIZER_TESTS"] = " ".join(tests)
        self.profile["sanitizer_compatibility"] = {
            "GH_SAN_CONFIGURE": self.profile["configure_args"][:],
            "GH_SAN_TARGETS": ["shared", "compat-app"], "GH_SAN_TESTS": tests, "GH_SAN_TEST_JOBS": "1",
            "GH_SAN_SKIP_PATTERN": r"\*\*\*Skipped|Not Run"}
        for phase, field in zip(r.PHASES, ("GH_SAN_CONFIGURE_ENV", "GH_SAN_BUILD_ENV", "GH_SAN_ENV")):
            overlay = dict(self.profile["environment"], **self.profile["phase_environments"][phase])
            self.profile["sanitizer_compatibility"][field] = [f"{k}={v}" for k, v in overlay.items()]
        realized, resolved, audit = self.ready()
        self.assertEqual(realized["profile"]["sanitizer_compatibility"], self.profile["sanitizer_compatibility"])
        self.assertEqual(resolved["compatibility_build_targets"], ["compat-app", "shared"])
        self.assertIn("compat-app", resolved["build_targets"])
        self.assertTrue((self.build / "compat-app").is_file())
        self.assertIn(str(self.root / "app.c"), audit["input_hashes"])
        for changed in (dict(resolved, compatibility_build_targets=[]), dict(resolved, build_targets=["shared"])):
            with self.assertRaisesRegex(r.RealizationError, "build targets changed"):
                r.build_resolved(changed, self.build)
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(receipt["outcome"], "passed", receipt)
        self.inventory["checks"]["gui"]["ctest_tests"]["synthetic"] = ["other"]
        with self.assertRaisesRegex(r.RealizationError, "membership disagreement"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
