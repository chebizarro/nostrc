"""Exporter contract; run with PATH pointing at each supported CMake/CTest pair.

In particular, use actual CMake 3.22 and 3.25 executables: a newer executable
with cmake_minimum_required(3.22) does not detect unavailable command syntax.
These tests intentionally do not depend on the selection/realization engine.
"""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "cmake/NostrcTestInventory.cmake"


class InventoryCompatibilityTests(unittest.TestCase):
    def setUp(self):
        self.cmake = shutil.which("cmake")
        self.ctest = shutil.which("ctest")
        self.assertIsNotNone(self.cmake, "CMake is required for the inventory contract")
        self.assertIsNotNone(self.ctest, "CTest is required for the inventory contract")
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve() / "source"
        self.build = self.root.parent / "build"
        self.root.mkdir()
        shutil.copyfile(HELPER, self.root / "inventory.cmake")

    def write(self, relative, content):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def command(self, *args, success=True):
        proc = subprocess.run(args, capture_output=True, text=True, timeout=60)
        if success:
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        else:
            self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return proc

    def configure(self, body, project="inventory_contract", success=True):
        self.write("CMakeLists.txt", f'''cmake_minimum_required(VERSION 3.22)
project({project} NONE)
enable_testing()
include(inventory.cmake)
{body}
nostrc_finalize_test_inventory()
''')
        proc = self.command(self.cmake, "-S", str(self.root), "-B", str(self.build),
                            "-DNOSTRC_TEST_SELECTION_TOKEN=contract-token", success=success)
        if not success:
            return proc
        export = json.loads((self.build / "nostrc-test-descriptors-.json").read_text())
        self.assertEqual(set(export), {"schema_version", "generation_token", "tests"})
        self.assertEqual(export["schema_version"], 1)
        self.assertEqual(export["generation_token"], "contract-token")
        tests = json.loads(self.command(self.ctest, "--test-dir", str(self.build),
                                       "--show-only=json-v1").stdout)["tests"]
        self.assertCountEqual([t["name"] for t in tests], [t["name"] for t in export["tests"]])
        return export["tests"], tests

    def test_nested_registrations_descriptors_and_authored_support_edges(self):
        self.write("libgo/CMakeLists.txt", '''
include("${CMAKE_SOURCE_DIR}/inventory.cmake")
add_test(NAME setup COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(setup PROPERTIES FIXTURES_SETUP private)
nostrc_describe_test(setup OWNER testing.framework INPUTS setup.cmake EXPECTED fixture)
add_test(NAME cleanup COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(cleanup PROPERTIES FIXTURES_CLEANUP private)
add_test(NAME ordered COMMAND "${CMAKE_COMMAND}" -E true)
add_test(NAME consumer COMMAND "${CMAKE_COMMAND}" -E true)
nostrc_describe_test(consumer OWNER libgo TARGETS runtime helper INPUTS data/vector.txt
  REQUIRES private_bus display EXPECTED pass)
# Late ordinary property changes must not be lost at descriptor/registration time.
set_tests_properties(consumer PROPERTIES DEPENDS ordered FIXTURES_REQUIRED private LABELS existing)
add_subdirectory(nested)
''')
        self.write("libgo/nested/CMakeLists.txt", '''
add_test(NAME consumer COMMAND "${CMAKE_COMMAND}" -E true)
add_test(legacy "${CMAKE_COMMAND}" -E true)
''')
        records, tests = self.configure('''
add_test(NAME root-test COMMAND "${CMAKE_COMMAND}" -E true)
add_subdirectory(libgo)
''')
        self.assertEqual(len(records), 7)
        consumer = next(d for d in records if d["name"] == "consumer" and d["source_dir"] == str(self.root / "libgo"))
        self.assertEqual(consumer, {
            "name": "consumer", "source_dir": str(self.root / "libgo"),
            "binary_dir": str(self.build / "libgo"), "owner_authority": "descriptor",
            "owner": ["libgo"], "targets": ["runtime", "helper"],
            "inputs": [str(self.root / "libgo/data/vector.txt")],
            "requires": ["private_bus", "display"], "expected": ["pass"],
            "declared_depends": ["ordered"],
        })
        nested = next(d for d in records if d["name"] == "consumer" and d != consumer)
        self.assertEqual(nested["owner_authority"], "component_directory")
        self.assertEqual(nested["owner"], ["libgo"])
        self.assertEqual(nested["declared_depends"], [])
        root = next(d for d in records if d["name"] == "root-test")
        self.assertEqual(root["owner_authority"], "unresolved")
        self.assertEqual(root["owner"], [])
        actual = next(t for t in tests if t["name"] == "consumer" and
                      any(p["name"] == "FIXTURES_REQUIRED" for p in t["properties"]))
        props = {p["name"]: p["value"] for p in actual["properties"]}
        self.assertEqual(props["FIXTURES_REQUIRED"], ["private"])
        self.assertCountEqual(props["DEPENDS"], ["ordered", "setup"])
        self.assertEqual(props["LABELS"], ["existing"])
        cleanup = next(t for t in tests if t["name"] == "cleanup")
        props = {p["name"]: p["value"] for p in cleanup["properties"]}
        self.assertEqual(props["FIXTURES_CLEANUP"], ["private"])
        self.assertIn("consumer", props["DEPENDS"])
        self.assertEqual(next(d for d in records if d["name"] == "cleanup")["declared_depends"], [])

    def test_deferred_registration_and_mutation_finish_before_export(self):
        self.write("child/CMakeLists.txt", '''
add_test(NAME early COMMAND "${CMAKE_COMMAND}" -E true)
function(late_author)
  nostrc_describe_test(early OWNER late INPUTS late.txt)
  set_tests_properties(early PROPERTIES DEPENDS deferred)
  cmake_language(DEFER CALL add_test NAME deferred COMMAND "${CMAKE_COMMAND}" -E true)
  cmake_language(DEFER CALL set_tests_properties deferred PROPERTIES FIXTURES_SETUP later)
endfunction()
cmake_language(DEFER CALL late_author)
''')
        records, _ = self.configure('''
add_subdirectory(child)
# This is after finalize was scheduled by the first registration as well.
cmake_language(DEFER CALL add_test NAME root-late COMMAND "${CMAKE_COMMAND}" -E true)
''')
        self.assertEqual(len(records), 3)
        early = next(d for d in records if d["name"] == "early")
        self.assertEqual(early["owner"], ["late"])
        self.assertEqual(early["inputs"], [str(self.root / "child/late.txt")])
        self.assertEqual(early["declared_depends"], ["deferred"])

    def test_standalone_libgo_owner_and_empty_root(self):
        records, _ = self.configure('add_test(NAME primitive COMMAND "${CMAKE_COMMAND}" -E true)', project="libgo")
        self.assertEqual(records[0]["owner_authority"], "component_directory")
        self.assertEqual(records[0]["owner"], ["libgo"])
        shutil.rmtree(self.build)
        records, tests = self.configure("")
        self.assertEqual(records, [])
        self.assertEqual(tests, [])

    def test_empty_intermediate_and_sibling_directories_do_not_copy_records(self):
        self.write("libgo/CMakeLists.txt", "add_subdirectory(nested)\nadd_subdirectory(empty)\n")
        self.write("libgo/empty/CMakeLists.txt", "# No registrations here.\n")
        self.write("libgo/nested/CMakeLists.txt", 'add_test(NAME nested COMMAND "${CMAKE_COMMAND}" -E true)\n')
        records, _ = self.configure('''
add_test(NAME root-test COMMAND "${CMAKE_COMMAND}" -E true)
add_subdirectory(libgo)
''')
        self.assertEqual(len(records), 2)
        self.assertEqual({d["name"] for d in records}, {"root-test", "nested"})
        self.assertEqual(next(d for d in records if d["name"] == "nested")["owner"], ["libgo"])

    def test_native_argument_vector_and_labels_are_unchanged(self):
        # Compare native registration with interception under the same executable,
        # including empty/list/newline/literal-variable/bracket-delimiter arguments.
        body = r'''
add_test(NAME "quoted test" COMMAND "${CMAKE_COMMAND}" -E echo "" "a;b"
  [==[${not_a_variable}]==] [==[]] ]=] back\slash]==] "line\nfeed" "\nleading" "carriage\r\nreturn" "$<CONFIG>")
add_test(legacy "${CMAKE_COMMAND}" -E echo "" "a;b" [=[${literal}]=])
set_tests_properties("quoted test" PROPERTIES LABELS existing)
nostrc_describe_test("quoted test" OWNER alpha beta)
'''
        records, tests = self.configure(body)
        props = {p["name"]: p["value"] for p in tests[1 if tests[0]["name"] == "legacy" else 0]["properties"]}
        self.assertEqual(props["LABELS"], ["existing", "selection:alpha", "selection:beta"])
        native = self.root / "native"
        native.mkdir()
        native_body = body[:body.index("nostrc_describe_test")]
        (native / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.22)\nproject(native NONE)\nenable_testing()\n' + native_body)
        self.command(self.cmake, "-S", str(native), "-B", str(native / "build"))
        original = json.loads(self.command(self.ctest, "--test-dir", str(native / "build"), "--show-only=json-v1").stdout)["tests"]
        self.assertEqual({t["name"]: t["command"] for t in tests}, {t["name"]: t["command"] for t in original})
        self.assertEqual(len(records), 2)

    def test_missing_child_collection_is_fatal_not_an_empty_fallback(self):
        self.write("before/CMakeLists.txt", 'add_test(NAME missed COMMAND "${CMAKE_COMMAND}" -E true)\n')
        self.write("CMakeLists.txt", '''cmake_minimum_required(VERSION 3.22)
project(missing NONE)
enable_testing()
add_subdirectory(before)
include(inventory.cmake)
nostrc_finalize_test_inventory()
''')
        proc = self.command(self.cmake, "-S", str(self.root), "-B", str(self.build), success=False)
        self.assertIn("Test inventory collection missing or stale", proc.stderr)
        self.assertFalse(list(self.build.glob("nostrc-test-descriptors-*.json")))
