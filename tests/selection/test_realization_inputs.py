import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from realization_support import Project, ROOT, r


class InputTests(Project):
    def test_complete_native_source_runtime_header_audit(self):
        realized = self.configure()
        self.build_all()
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        self.assertGreater(audit["header_entries"], 0)
        self.assertIn(str(self.root / "shared.h"), audit["input_hashes"])

    def test_discovered_header_missing_edge_blocks_even_omitted_test(self):
        realized = self.configure()
        self.build_all()
        self.inventory["nodes"]["application"]["depends_on"].remove("core")
        audit = self.audit(realized)
        self.assertFalse(audit["passed"])
        self.assertTrue(audit["requires_broad"])
        self.assertTrue(any(g["kind"] == "missing_impact_edge" and g["test"] == "omitted"
                            and g["input"] == "shared.h" for g in audit["gaps"]))

    def test_missing_runtime_file_is_not_ignored(self):
        realized = self.configure()
        (self.root / "vector.txt").unlink()
        audit = r.check_inputs(self.inventory, realized)
        self.assertTrue(any(g["kind"] == "missing_input" for g in audit["gaps"]))

    def test_unowned_registration_is_explicit_gap(self):
        realized = self.configure()
        realized["descriptors"] = [d for d in realized["descriptors"] if d["name"] != "omitted"]
        audit = r.check_inputs(self.inventory, realized)
        self.assertIn({"kind": "unowned_registration", "test": "omitted"}, audit["gaps"])

    def test_no_header_index_is_not_a_passing_audit(self):
        realized = self.configure()
        audit = self.audit(realized)
        self.assertTrue(any(g["kind"] == "missing_discovered_header_index" for g in audit["gaps"]))

    def test_dirty_input_identity_must_match(self):
        realized = self.configure()
        with self.assertRaisesRegex(r.RealizationError, "identity mismatch"):
            r.check_inputs(self.inventory, realized, dict(self.identity, snapshot_digest="changed"))

    def test_deleted_file_in_runtime_directory_remains_required(self):
        directory = self.root / "runtime-dir"
        directory.mkdir()
        (directory / "required-schema.xml").write_text("schema")
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("INPUTS vector.txt", "INPUTS runtime-dir"))
        self.inventory["path_rules"].append({"prefix": "runtime-dir/", "nodes": ["vectors"]})
        realized = self.configure()
        (directory / "required-schema.xml").unlink()
        audit = r.check_inputs(self.inventory, realized)
        self.assertTrue(any(g["kind"] == "missing_input" and g["input"].endswith("required-schema.xml") for g in audit["gaps"]))
        self.assertTrue(any(g["kind"] == "empty_runtime_directory" for g in audit["gaps"]))

    def test_runtime_symlink_cannot_escape_checkout(self):
        realized = self.configure()
        path = self.root / "vector.txt"
        path.unlink()
        external = self.root.parent / "external"
        external.write_text("not source snapshot content")
        path.symlink_to(external)
        audit = r.check_inputs(self.inventory, realized)
        self.assertTrue(any(g["kind"] == "external_symlink_input" for g in audit["gaps"]))


class WrapperTargetMappingTests(unittest.TestCase):
    """Export production registrations with minimal targets, not application runs."""

    def configure(self, options=(), sanitize="address,undefined", deps=True):
        self.assertIsNotNone(shutil.which("cmake"))
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        source = Path(temp.name).resolve() / "source"
        build = source.parent / "build"
        source.mkdir()
        query = build / ".cmake/api/v1/query"
        query.mkdir(parents=True)
        (query / "codemodel-v2").touch()
        shutil.copy(ROOT / "cmake/NostrcTestInventory.cmake", source / "inventory.cmake")
        text = (ROOT / "tests/CMakeLists.txt").read_text()
        registration = text[text.index("# tests/ precedes apps/"):text.index("# Concurrency Test Suite")]
        relay = (ROOT / "apps/relayd/tests/CMakeLists.txt").read_text()
        relay = relay[relay.index("if(UNIX AND NOT APPLE AND NOT CYGWIN)"):relay.index("# Session-relay peercred")]
        for directory, body in (("tests", registration), ("apps/relayd/tests", relay)):
            path = source / directory
            path.mkdir(parents=True)
            (path / "CMakeLists.txt").write_text(body)
        (source / "apps/relayd/tests/run_bind_iface_integ.sh").write_text(
            (ROOT / "apps/relayd/tests/run_bind_iface_integ.sh").read_text())
        schema = source / "apps/gnostr/data/schemas"
        schema.mkdir(parents=True)
        shutil.copy(ROOT / "apps/gnostr/data/schemas/org.gnostr.gnostr.gschema.xml", schema)
        (source / "main.c").write_text("int main(void) { return 0; }\n")
        # Deterministic dependency/platform discovery for this isolated fixture;
        # do not depend on a developer's GUI packages or Linux-only ss command.
        (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.22)
project(wrapper_mapping_fixture C)
enable_testing()
include(inventory.cmake)
macro(pkg_check_modules prefix)
  if(NOT "${ARGN}" STREQUAL "QUIET;gtk4;glib-2.0;gobject-2.0;gio-2.0")
    message(FATAL_ERROR "unexpected app availability probe: ${ARGN}")
  endif()
  set(${prefix}_FOUND ${TEST_APP_DEPS})
endmacro()
set(UNIX TRUE)
set(APPLE FALSE)
set(CYGWIN FALSE)
set(SS_EXECUTABLE "${CMAKE_COMMAND}")
set(BASH_EXECUTABLE /bin/bash)
set(VALGRIND_EXECUTABLE /fixture/valgrind)
add_subdirectory(tests)
# Model the later app target: registration cannot rely on an early TARGET check.
if((NOT DEFINED BUILD_APPS OR BUILD_APPS)
    AND (NOT DEFINED ENABLE_APPS OR ENABLE_APPS)
    AND (NOT DEFINED BUILD_GNOSTR_APP OR BUILD_GNOSTR_APP)
    AND TEST_APP_DEPS)
  add_executable(gnostr main.c)
  set_target_properties(gnostr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/gnostr")
  add_custom_target(gnostr-schemas)
  add_dependencies(gnostr gnostr-schemas)
endif()
add_executable(nostrc-relayd main.c)
add_subdirectory(apps/relayd/tests)
nostrc_finalize_test_inventory()
''')
        command = ["cmake", "-S", str(source), "-B", str(build),
                   "-DNOSTRC_TEST_SELECTION_TOKEN=wrapper-contract",
                   f"-DSANITIZE={sanitize}", f"-DTEST_APP_DEPS={'ON' if deps else 'OFF'}", *options]
        proc = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        proc = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                              capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        tests = {t["name"]: t for t in json.loads(proc.stdout)["tests"]}
        descriptors = {d["name"]: d for d in json.loads(
            (build / "nostrc-test-descriptors-.json").read_text())["tests"]}
        reply = build / ".cmake/api/v1/reply"
        codemodel = json.loads(next(reply.glob("codemodel-v2-*.json")).read_text())
        targets = {}
        for entry in codemodel["configurations"][0]["targets"]:
            target = json.loads((reply / entry["jsonFile"]).read_text())
            targets[target["id"]] = {
                "id": target["id"], "name": target["name"], "type": target["type"],
                "artifacts": [str(build / a["path"]) for a in target.get("artifacts", [])],
                "dependencies": [d["id"] for d in target.get("dependencies", [])]}
        return source, build, tests, descriptors, {"targets": targets}

    def test_disabled_apps_never_register_missing_executable(self):
        for options, deps in [(["-DBUILD_APPS=OFF"], True),
                              (["-DENABLE_APPS=OFF"], True),
                              (["-DBUILD_GNOSTR_APP=OFF"], True), ([], False)]:
            for sanitize in ("address,undefined", "thread", ""):
                with self.subTest(options=options, deps=deps, sanitize=sanitize):
                    _, _, tests, descriptors, realization = self.configure(options, sanitize, deps)
                    self.assertEqual(set(tests), {"relayd_bind_iface_integ"})
                    self.assertNotIn("gnostr_asan_test", descriptors)
                    self.assertNotIn("gnostr", {t["name"] for t in realization["targets"].values()})

    def test_enabled_and_default_apps_keep_sanitizer_wrapper_contract(self):
        for options in ([], ["-DBUILD_APPS=ON", "-DENABLE_APPS=ON", "-DBUILD_GNOSTR_APP=ON"]):
            for sanitize in ("address", "undefined", "address,undefined"):
                with self.subTest(options=options, sanitize=sanitize):
                    source, build, tests, descs, realization = self.configure(options, sanitize)
                    test, desc = tests["gnostr_asan_test"], descs["gnostr_asan_test"]
                    self.assertEqual(test["command"][1:], ["-E", "env",
                        "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1",
                        "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1",
                        str(build / "apps/gnostr/gnostr")])
                    props = r._properties(test)
                    self.assertEqual(props["TIMEOUT"], 60)
                    self.assertTrue({"sanitizer", "gtk"}.issubset(props["LABELS"]))
                    self.assertEqual(props["WORKING_DIRECTORY"], str(build / "apps/gnostr"))
                    self.assertEqual(desc["owner"], ["shared.tests"])
                    self.assertEqual(desc["targets"], ["gnostr"])
                    self.assertEqual(desc["requires"], ["display"])
                    self.assertCountEqual(desc["inputs"], [str(source / "apps/gnostr/data/schemas/org.gnostr.gnostr.gschema.xml"),
                                                           str(build / "apps/gnostr/gschemas.compiled")])
                    ids = r._target_closure(r._mapped_targets(test, desc, realization), realization)
                    self.assertEqual({realization["targets"][i]["name"] for i in ids}, {"gnostr", "gnostr-schemas"})

    def test_other_enabled_app_modes_are_preserved(self):
        for sanitize, expected, timeout in [("thread", "gnostr_tsan_test", 90),
                                             ("", "gnostr_valgrind_crash_test", 180)]:
            with self.subTest(sanitize=sanitize):
                _, _, tests, _, _ = self.configure(sanitize=sanitize)
                self.assertIn(expected, tests)
                self.assertEqual(r._properties(tests[expected])["TIMEOUT"], timeout)
                if sanitize == "thread":
                    self.assertEqual(tests[expected]["command"][1:4], ["-E", "env",
                        "TSAN_OPTIONS=halt_on_error=1:history_size=7:second_deadlock_stack=1"])
                self.assertNotIn("gnostr_asan_test", tests)

    def test_relay_wrapper_maps_real_executable_and_script(self):
        source, _, tests, descs, realization = self.configure(["-DBUILD_GNOSTR_APP=OFF"])
        test, desc = tests["relayd_bind_iface_integ"], descs["relayd_bind_iface_integ"]
        self.assertEqual(test["command"][:2], ["/bin/bash", str(source / "apps/relayd/tests/run_bind_iface_integ.sh")])
        self.assertEqual(desc["owner"], ["relayd"])
        self.assertEqual(desc["inputs"], [test["command"][1]])
        self.assertEqual(desc["targets"], ["nostrc-relayd"])
        self.assertEqual(desc["requires"], [])
        props = r._properties(test)
        self.assertEqual(props["TIMEOUT"], 30)
        self.assertEqual(props["SKIP_RETURN_CODE"], 77)
        ids = r._mapped_targets(test, desc, realization)
        self.assertEqual([realization["targets"][i]["name"] for i in ids], ["nostrc-relayd"])
        self.assertEqual(test["command"][2], realization["targets"][ids[0]]["artifacts"][0])
        # The guard still rejects the original missing metadata and made-up targets.
        with self.assertRaisesRegex(r.RealizationError, "script/wrapper lacks explicit"):
            r._mapped_targets(test, {"owner": ["relayd"]}, realization)
        with self.assertRaisesRegex(r.RealizationError, "unknown descriptor target"):
            r._mapped_targets(test, {"targets": ["nonexistent"]}, realization)
