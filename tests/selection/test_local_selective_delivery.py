"""Representative operational selection and real CMake/CTest regressions."""
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from selector_support import ROOT, change, policy, s
import test_selection_legacy as legacy


class AffectedDeliveryTests(unittest.TestCase):
    def test_reverse_closure_not_prerequisite_tests(self):
        p = policy()
        selected = s.make_plan(p, [change("app/source.c")], head="a" * 40)
        self.assertEqual(selected["selected_checks"], ["app.live", "app.unit"])
        self.assertNotIn("core.unit", selected["selected_checks"])
        selected = s.make_plan(p, [change("core/source.c")], head="a" * 40)
        self.assertEqual(selected["witnesses"]["app"], ["core", "app"])
        self.assertIn("app.live", selected["selected_checks"])

    def test_actual_inventory_expensive_inputs_and_unrelated_omission(self):
        p = s.load_inventory(ROOT / "tests/selection/inventory.json")
        def selected(path):
            return s.make_plan(p, [change(path)], head="a" * 40)
        unrelated = selected("nips/nip02/src/nip02.c")
        self.assertEqual(unrelated["effective_mode"], "affected")
        self.assertFalse(any(ident.startswith("mdk") for ident in unrelated["selected_checks"]))
        for path in ("libmarmot/src/marmot.c", "nips/nip44/src/nip44.c", "libgo/src/channel.c"):
            self.assertTrue(any(ident.startswith("mdk") for ident in selected(path)["selected_checks"]), path)
        unknown = selected("a-new-component/not-classified.c")
        self.assertEqual(unknown["effective_mode"], "broad")
        self.assertTrue(unknown["broadening_reasons"])
        self.assertTrue(any(ident.startswith("mdk") for ident in unknown["selected_checks"]))
        renamed = s.make_plan(p, [change("docs/moved.txt", status="R100", old="libmarmot/src/marmot.c")], head="a" * 40)
        self.assertTrue(any(ident.startswith("mdk") for ident in renamed["selected_checks"]))

    def test_fixture_closure_transitive_and_missing_refusal(self):
        def test(name, **props):
            return dict(name=name, properties=[dict(name=k, value=v) for k, v in props.items()])
        export = {"tests": [test("a", FIXTURES_REQUIRED=["one"]),
                            test("setup", FIXTURES_SETUP=["one"], FIXTURES_REQUIRED=["two"]),
                            test("cleanup", FIXTURES_CLEANUP=["one"]),
                            test("setup2", FIXTURES_SETUP=["two"]), test("unrelated")]}
        self.assertEqual(legacy.fixture_closure(export, ["a"]), ["a", "cleanup", "setup", "setup2"])
        with self.assertRaisesRegex(RuntimeError, "missing"):
            legacy.fixture_closure(export, ["nonexistent"])

    def test_generated_fixture_capabilities_follow_selected_closure(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build / "nostrc-test-descriptors-Debug.json").write_text(json.dumps({"tests": [
                {"name": "setup", "requires": ["private_bus"]}, {"name": "unrelated", "requires": ["docker"]}]}))
            self.assertEqual(legacy.fixture_capabilities(build, "Debug", {"setup", "a"}), {"private_bus"})

    def test_skip_and_missing_result_are_not_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = Path(tmp) / "result.xml"
            result.write_text('<testsuite><testcase name="a"><skipped/></testcase></testsuite>')
            with self.assertRaisesRegex(RuntimeError, "skipped"):
                legacy.check_junit(result, ["a"])
            with self.assertRaisesRegex(RuntimeError, "membership"):
                legacy.check_junit(result, ["a", "b"])

    def test_duplicate_names_are_rejected_before_name_based_execution(self):
        with self.assertRaisesRegex(RuntimeError, "duplicate"):
            legacy.fixture_closure({"tests": [{"name": "a"}, {"name": "a"}]}, ["a"])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "result.xml"
            path.write_text('<testsuite><testcase name="a"><skipped/></testcase><testcase name="a"/></testsuite>')
            with self.assertRaisesRegex(RuntimeError, "duplicate"):
                legacy.check_junit(path, ["a"])

    def test_expected_incompatibility_requires_return77_and_original_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "result.xml"
            marker = "XFAIL [unsupported]: MDK 0.9.0 (0xF2F1 v1 proof) is incompatible: verified"
            def write(message, output):
                path.write_text(f'<testsuite><testcase name="probe" status="notrun"><skipped message="{message}"/><system-out>{output}</system-out></testcase></testsuite>')
            for message, output in (("Disabled", marker), ("SKIP_RETURN_CODE=77", "driver unset")):
                write(message, output)
                with self.assertRaisesRegex(RuntimeError, "skipped"):
                    legacy.check_junit(path, ["probe"], ["probe"])
            write("SKIP_RETURN_CODE=77", marker)
            self.assertEqual(legacy.check_junit(path, ["probe"], ["probe"]), {"probe": "expected_incompatibility"})

    def test_unknown_explicit_profile_and_capability_refuse(self):
        p = policy()
        plan = s.make_plan(p, [change("app/source.c")], head="a" * 40)
        with self.assertRaisesRegex(RuntimeError, "unknown explicit profiles"):
            legacy.run(p, plan, Path("."), Path("."), {"lanes": []}, command=None, materialize=None, profiles=["core.typo"])
        with self.assertRaisesRegex(RuntimeError, "specialized adapter"):
            legacy._prefix({"unsupported"})


class LegacySignerCompositionTests(unittest.TestCase):
    def test_approval_and_ambient_authority_refuse_before_service_contact(self):
        with self.assertRaisesRegex(RuntimeError, "explicit"):
            with legacy._signer_fixture({}, ROOT, ROOT, ROOT, {}):
                self.fail("unapproved fixture entered")
        profile = {"environment": {"NOSTRC_TEST_NIP5F_APPROVAL": "unidentified-get-public-key-once"}}
        with patch.dict(os.environ, NOSTR_SIGNER_TOKEN="not-a-test-token"):
            with self.assertRaisesRegex(RuntimeError, "ambient"):
                with legacy._signer_fixture(profile, ROOT, ROOT, ROOT, {}):
                    self.fail("ambient signer authority admitted")

    def test_existing_delivery_boundary_and_cleanup_on_success_and_failure(self):
        # Unit composition only: no daemon or Linux/native acceptance is claimed.
        profile = {"environment": {"NOSTRC_TEST_NIP5F_APPROVAL": "unidentified-get-public-key-once"}}
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as tmp:
                lane = Path(tmp)
                class Owned:
                    def __init__(self, *args, **kwargs):
                        self.env = {"NOSTR_SIGNER_ENDPOINT": "tcp:127.0.0.1:12345", "NOSTR_SIGNER_TOKEN": "a" * 64}
                        self.token = self.env["NOSTR_SIGNER_TOKEN"]
                        self.result = {}
                    def start(self):
                        pass
                    def snapshot(self):
                        return "(uint32 1, uint32 1, uint32 0)"
                    def finish(self):
                        self.result["finished"] = True
                config = SimpleNamespace(inputs={})
                fixture = {"prepare_fixture": lambda *args, **kwargs: config, "Signer": Owned}
                state = {}
                with patch.dict(os.environ, {"NOSTR_SIGNER_ENDPOINT": "", "NOSTR_SIGNER_TOKEN": "", "NOSTR_SIGNER_SOCK": ""}), patch.object(legacy.runpy, "run_path", return_value=fixture):
                    try:
                        with legacy._signer_fixture(profile, ROOT, ROOT, lane, state) as argv:
                            spec = Path(argv[1])
                            self.assertTrue(spec.is_file())
                            self.assertEqual(argv[0], "--resource-spec-file")
                            if fail:
                                raise RuntimeError("test interruption")
                    except RuntimeError as exc:
                        self.assertTrue(fail)
                        self.assertEqual(str(exc), "test interruption")
                self.assertFalse(spec.exists())
                self.assertTrue(state["signer"]["cleanup"]["finished"])
                self.assertEqual(state["signer"]["status"], "failed" if fail else "passed")


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"), "requires CMake and CTest")
class PhysicalLegacyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nostrc-selective-")
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name) / "source"
        self.source.mkdir()
        (self.source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.22)
project(delivery_fixture NONE)
enable_testing()
add_custom_target(default_probe ALL COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/built)
add_test(NAME app.unit COMMAND ${CMAKE_COMMAND} -E true)
add_test(NAME app.live COMMAND ${CMAKE_COMMAND} -E true)
set_tests_properties(app.unit PROPERTIES FIXTURES_REQUIRED seed)
add_test(NAME setup COMMAND ${CMAKE_COMMAND} -E true)
set_tests_properties(setup PROPERTIES FIXTURES_SETUP seed)
add_test(NAME cleanup COMMAND ${CMAKE_COMMAND} -E true)
set_tests_properties(cleanup PROPERTIES FIXTURES_CLEANUP seed)
add_test(NAME unowned COMMAND ${CMAKE_COMMAND} -E true)
add_test(NAME core.unit COMMAND ${CMAKE_COMMAND} -E true)
add_test(NAME fixture.unit COMMAND ${CMAKE_COMMAND} -E true)
add_test(NAME other.unit COMMAND ${CMAKE_COMMAND} -E true)
''')
        self.p = policy()
        profile = self.p["profiles"]["linux"]
        profile.update(platform={"Darwin": "macos", "Linux": "linux"}[platform.system()],
                       architecture="any", compiler="none", configure_args=["-DCMAKE_BUILD_TYPE=Debug"])
        self.argv = []

    def command(self, argv, *, cwd=None, env=None, log=None):
        self.argv.append(list(map(str, argv)))
        with open(log, "xb") as stream:
            result = subprocess.run(list(map(str, argv)), cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f"exit {result.returncode}: {log}")

    def run_plan(self, *, mode="affected", profiles=("linux",)):
        plan = s.make_plan(self.p, [change("app/source.c")], head="a" * 40, mode=mode)
        directory = Path(self.temp.name) / ("run-" + str(len(list(Path(self.temp.name).glob("run-*")))))
        directory.mkdir()
        record = {"lanes": []}
        legacy.run(self.p, plan, self.source, directory, record, command=self.command,
                   materialize=lambda *args: None, profiles=list(profiles) if profiles else None)
        return record, directory

    def test_real_default_build_selected_tests_fixtures_and_omission(self):
        record, directory = self.run_plan()
        self.assertEqual(record["status"], "partial", record)
        lane = record["lanes"][0]
        self.assertEqual(lane["fixture_closure"], ["app.live", "app.unit", "cleanup", "setup"])
        self.assertTrue(list(directory.glob("*/build/built")))
        self.assertFalse(lane["complete"])
        self.assertFalse(lane["complete_gate_eligible"])
        self.assertNotIn("unowned", lane["fixture_closure"])

    def test_host_local_default_projection_executes_affected_lane(self):
        native = sorted(legacy.DEFAULT_PROFILES[platform.system()])[0]
        self.p["profiles"][native] = self.p["profiles"].pop("linux")
        for check in self.p["checks"].values():
            check["profiles"] = [native]
            check["ctest_tests"][native] = check["ctest_tests"].pop("linux")
        record, _ = self.run_plan(profiles=None)
        self.assertEqual(record["status"], "partial", record)
        self.assertEqual(record["lanes"][0]["profile"], native)
        self.assertEqual(record["scope"], "host local defaults")

    def test_broad_runs_unowned_registration_too(self):
        record, _ = self.run_plan(mode="broad")
        self.assertEqual(record["status"], "partial", record)
        self.assertIn("unowned", record["lanes"][0]["fixture_closure"])

    def test_missing_selected_registration_fails(self):
        self.p["checks"]["app.unit"]["ctest_tests"]["linux"] = ["absent"]
        record, _ = self.run_plan()
        self.assertEqual(record["status"], "failed")
        self.assertIn("missing", record["lanes"][0]["error"])

    def test_failure_retained_not_retried(self):
        cmake = self.source / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("NAME app.unit COMMAND ${CMAKE_COMMAND} -E true", "NAME app.unit COMMAND ${CMAKE_COMMAND} -E false"))
        record, directory = self.run_plan()
        self.assertEqual(record["status"], "failed")
        self.assertEqual(sum("--output-on-failure" in argv for argv in self.argv), 1)
        self.assertTrue(list(directory.glob("*/legacy-result.json")))

    def test_guards_run_with_no_selected_runtime_checks(self):
        script = self.source / "scripts/check-unsequenced-args.py"
        script.parent.mkdir(exist_ok=True)
        script.write_text('from pathlib import Path\nPath("guard-ran").touch()\n')
        selected = s.make_plan(self.p, [change("docs/notes.md")], head="a" * 40)
        directory = Path(self.temp.name) / "guards"
        directory.mkdir()
        record = {"lanes": []}
        legacy.run(self.p, selected, self.source, directory, record, command=self.command,
                   materialize=lambda *args: None, profiles=["linux"])
        self.assertTrue((self.source / "guard-ran").exists())
        self.assertEqual(record["lanes"], [])
        self.assertEqual(len(record["guards"]), 1)


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"), "requires CMake and CTest")
class GateEntrypointTests(PhysicalLegacyTests):
    def setUp(self):
        super().setUp()
        for relative in ("scripts/test-gate.sh", "scripts/test_selection.py", "scripts/test_selection_subset.py",
                         "scripts/test_selection_macos_nip5f_tcp.py", "scripts/test_selection_legacy.py"):
            target = self.source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
        # This fixture does not exercise a keychain backend. Pin that the
        # adapter invokes the read-only guard around execution, without
        # requiring any particular developer login-keychain configuration.
        (self.source / "scripts/check-macos-keychain.sh").write_text("#!/bin/sh\necho fixture-read-only-guard\n")
        (self.source / "tests/selection").mkdir(parents=True)
        (self.source / "tests/selection/inventory.json").write_text(json.dumps(self.p))
        (self.source / "app").mkdir()
        (self.source / "app/source.c").write_text("base\n")
        self.git("init", "-q")
        self.git("config", "user.name", "Delivery Test")
        self.git("config", "user.email", "delivery@example.invalid")
        self.git("add", "-A")
        self.git("commit", "-qm", "fixture")
        self.base = self.git("rev-parse", "HEAD")
        (self.source / "app/source.c").write_text("changed\n")
        self.git("commit", "-qam", "app change")
        self.head = self.git("rev-parse", "HEAD")

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.source), *args], stderr=subprocess.PIPE).decode().strip()

    def gate(self, *args):
        evidence = Path(self.temp.name) / "evidence"
        result = subprocess.run(["bash", str(self.source / "scripts/test-gate.sh"), "--repo", str(self.source),
                                 "--profile", "linux", "--base", self.base, "--evidence-dir", str(evidence), *args],
                                capture_output=True, text=True, env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        path = next(evidence.glob("*/adapter-receipt.json"))
        return json.loads(path.read_text()), path.parent

    def test_default_entrypoint_uses_real_legacy_runner(self):
        record, directory = self.gate("--head", self.head)
        self.assertEqual(record["execution_api"], "legacy CMake/CTest")
        self.assertEqual(record["status"], "partial")
        self.assertTrue(list(directory.glob("*/legacy-*/build/built")))
        self.assertEqual(len(self.git("worktree", "list").splitlines()), 1)

    def test_full_alias_is_real_broad_ctest_execution(self):
        record, _ = self.gate("--head", self.head, "--full")
        lane = record["executions"][0]["lanes"][0]
        self.assertIn("unowned", lane["fixture_closure"])
        self.assertEqual(lane["test_outcomes"]["unowned"], "passed")
        self.assertFalse(lane["complete_gate_eligible"])

    def test_explain_is_plan_only_and_preserves_real_index(self):
        (self.source / "app/source.c").write_text("dirty, not executable\n")
        index = self.source / ".git/index"
        original = index.read_bytes()
        record, directory = self.gate("--explain")
        self.assertEqual(record["status"], "planned")
        self.assertEqual(index.read_bytes(), original)
        self.assertFalse(list(directory.rglob("CMakeCache.txt")))
        self.assertFalse(list(directory.rglob("ctest.xml")))
        self.assertEqual(len(self.git("worktree", "list").splitlines()), 1)


if __name__ == "__main__":
    unittest.main()
