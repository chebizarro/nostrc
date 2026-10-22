"""Exercise the canonical NIP49 descriptor and physical SPEC_SOURCE reader."""
import hashlib

from realization_support import Project, r
from selector_support import ROOT


class Nip49DescriptorTests(Project):
    def test_actual_descriptor_and_spec_source_reader(self):
        nip = self.root / "nips/nip49"
        nip.mkdir(parents=True)
        for relative in ("TestInventory.cmake", "SPEC_SOURCE", "tests/test_spec_source.c"):
            target = nip / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((ROOT / "nips/nip49" / relative).read_bytes())
        (self.root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.22)
project(nip49_descriptor_fixture C)
enable_testing()
include(cmake/NostrcTestInventory.cmake)
add_subdirectory(nips/nip49)
nostrc_finalize_test_inventory()
''')
        (nip / "CMakeLists.txt").write_text('''add_executable(test_nip49_spec_source tests/test_spec_source.c)
add_test(NAME test_nip49_spec_source COMMAND test_nip49_spec_source WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
foreach(name negative payload roundtrip vectors)
  add_executable(test_nip49_${name} "${CMAKE_SOURCE_DIR}/noop.c")
  add_test(NAME test_nip49_${name} COMMAND test_nip49_${name})
endforeach()
''')
        (self.root / "noop.c").write_text("int main(void) { return 0; }\n")
        self.inventory["nodes"] = {"nip49": {}, "nip49.test_inputs": {}}
        self.inventory["path_rules"] = [{"prefix": "nips/nip49", "nodes": ["nip49.test_inputs"]},
                                        {"prefix": "cmake", "effect": "global"},
                                        {"exact": "noop.c", "nodes": ["nip49.test_inputs"]},
                                        {"exact": "CMakeLists.txt", "effect": "global"}]
        self.inventory["checks"] = {"spec": {"kind": "ctest", "profiles": ["synthetic"],
                                              "ctest_tests": {"synthetic": ["test_nip49_spec_source"]},
                                              "nodes": ["nip49", "nip49.test_inputs"]}}
        self.inventory["checks"]["other"] = {"kind": "ctest", "profiles": ["synthetic"],
                                               "ctest_tests": {"synthetic": [f"test_nip49_{n}" for n in ("negative", "payload", "roundtrip", "vectors")]},
                                               "nodes": ["nip49", "nip49.test_inputs"]}
        self.plan["selected_checks"] = ["spec"]
        realized = self.configure()
        self.build_all()
        descriptors = {d["name"]: d for d in realized["descriptors"]}
        actual = descriptors["test_nip49_spec_source"]
        self.assertEqual(actual["inputs"], [str(nip / "SPEC_SOURCE")])
        self.assertEqual(actual["owner"], ["nip49", "nip49.test_inputs"])
        self.assertEqual(actual["targets"], [])  # unambiguous File API mapping is unchanged
        for name in ("negative", "payload", "roundtrip", "vectors"):
            other = descriptors[f"test_nip49_{name}"]
            self.assertEqual(other["inputs"], [])
            self.assertEqual(other["owner"], ["nip49", "nip49.test_inputs"])
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        self.assertEqual(audit["input_hashes"][str(nip / "SPEC_SOURCE")],
                         hashlib.sha256((nip / "SPEC_SOURCE").read_bytes()).hexdigest())
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        self.assertIn(str(nip / "SPEC_SOURCE"), resolved["runtime_inputs"])
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit)
        self.assertEqual(receipt["outcome"], "passed", receipt)
        self.assertEqual(receipt["observed_executions"], {"test_nip49_spec_source": "passed"})
