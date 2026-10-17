"""Real CMake projection tests only: no MDK/Docker execution or custody proof."""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from selector_support import ROOT, s

PROFILE = "groundhog.linux.arm64.prepared011"


class PreparedProfileContract(unittest.TestCase):
    def test_exact_local_actual_architecture_profile_and_original_memberships(self):
        policy = s.load_inventory(ROOT / "tests/selection/inventory.json")
        profile = policy["profiles"][PROFILE]
        self.assertEqual(profile["execution_domain"], "prepared_peer_linux_v1")
        self.assertEqual(profile["prepared_peers"], ["011"])
        self.assertEqual(profile["platform"], "linux")
        self.assertEqual(profile["architecture"], "aarch64")
        self.assertEqual(profile["contexts"], ["local"])
        self.assertIn("-DBUILD_MDK09_PROBE=OFF", profile["configure_args"])
        self.assertIn("-DMDK011_INTEROP_DRIVER:STRING=/peers/011/driver", profile["configure_args"])
        members = [name for check in policy["checks"].values()
                   for name in check.get("ctest_tests", {}).get(PROFILE, [])]
        old = [name for key, check in policy["checks"].items() if key.startswith("mdk011.")
               for name in check["ctest_tests"]["groundhog.mdk011"]]
        self.assertEqual(len(members), 15)
        self.assertEqual(len(old), 16)
        self.assertEqual(set(members), set(old) - {"groundhog-mdk011-interop-mdk09-probe"})
        self.assertFalse(any("image" in name for name in members))
        self.assertFalse(policy["enforcement_ready"])

    def configure(self, *, native011=True, native09=False, probe=None, docker=False, missing=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = (ROOT / "gnome/groundhog/CMakeLists.txt").read_text()
        first = source.index('    set(groundhog_mdk011_artifacts "${CMAKE_BINARY_DIR}/mdk011-interop-artifacts")')
        last = source.index('\n  endif()\nelse()\n  message(STATUS "Groundhog MLS service', first)
        # Execute the actual fixture/case source, not a reconstructed name list.
        block = source[first:last]
        prefix = '''cmake_minimum_required(VERSION 3.22)
project(PreparedProjection NONE)
enable_testing()
function(find_program variable)
  if(variable STREQUAL "GROUNDHOG_DOCKER")
    if(NOT ALLOW_DOCKER_LOOKUP)
      message(FATAL_ERROR "unexpected Docker lookup")
    endif()
    set(${variable} "${CMAKE_COMMAND}" PARENT_SCOPE)
  else()
    _find_program(${variable} ${ARGN})
  endif()
endfunction()
add_executable(test-groundhog-mdk011-interop IMPORTED)
set_property(TARGET test-groundhog-mdk011-interop PROPERTY IMPORTED_LOCATION "${CMAKE_COMMAND}")
'''
        module = ROOT / "cmake/NostrcTestInventory.cmake"
        descriptors = ROOT / "gnome/groundhog/TestInventory.cmake"
        (root / "CMakeLists.txt").write_text(prefix + f'include("{module}")\n' + block
            + f'\ninclude("{descriptors}")\nnostrc_finalize_test_inventory()\n')
        command = ["cmake", "-S", str(root), "-B", str(root / "build")]
        if native011:
            driver = str(root / "missing-driver") if missing else shutil.which("cmake")
            command.append("-DMDK011_INTEROP_DRIVER:STRING=" + driver)
        if native09:
            command.append("-DMDK09_PROBE_DRIVER:STRING=" + shutil.which("cmake"))
        if probe is not None:
            command.append("-DBUILD_MDK09_PROBE=" + ("ON" if probe else "OFF"))
        if docker:
            command.append("-DALLOW_DOCKER_LOOKUP=ON")
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            return result, None, None
        output = subprocess.check_output(["ctest", "--test-dir", str(root / "build"), "--show-only=json-v1"], text=True)
        tests = {test["name"]: test for test in json.loads(output)["tests"]}
        exports = list((root / "build").glob("nostrc-test-descriptors-*.json"))
        descriptions = {test["name"]: test for test in json.loads(exports[0].read_text())["tests"]}
        return result, tests, descriptions

    def test_prepared011_only_omits_only_its_image_and_explicitly_disabled09(self):
        result, tests, descriptions = self.configure(probe=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(tests), 16)  # fifteen real case IDs plus artifact reset
        self.assertNotIn("groundhog-mdk011-interop-image", tests)
        self.assertNotIn("groundhog-mdk011-interop-image-mdk09", tests)
        self.assertNotIn("groundhog-mdk011-interop-mdk09-probe", tests)
        desc = descriptions["groundhog-mdk011-interop-control"]
        self.assertEqual(set(desc["requires"]), {"prepared_peer", "private_bus"})
        self.assertIn(shutil.which("cmake"), desc["inputs"])
        properties = {p["name"]: p["value"] for p in tests["groundhog-mdk011-interop-control"]["properties"]}
        self.assertEqual(properties["FIXTURES_REQUIRED"], ["groundhog-mdk011-artifacts"])

    def test_default09_is_not_suppressed_by_unrelated011_native_override(self):
        result, _, _ = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unexpected Docker lookup", result.stderr)

    def test_default_matrix_keeps_both_docker_build_fixtures_and09_case(self):
        result, tests, _ = self.configure(native011=False, docker=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(tests), 19)
        self.assertIn("groundhog-mdk011-interop-image", tests)
        self.assertIn("groundhog-mdk011-interop-image-mdk09", tests)
        self.assertIn("groundhog-mdk011-interop-mdk09-probe", tests)

    def test_native09_suppresses_only_its_own_image_not_the_probe_consumer(self):
        result, tests, descriptions = self.configure(native09=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(tests), 17)
        self.assertIn("groundhog-mdk011-interop-mdk09-probe", tests)
        self.assertNotIn("groundhog-mdk011-interop-image-mdk09", tests)
        self.assertNotIn("docker", descriptions["groundhog-mdk011-interop-mdk09-probe"]["requires"])

    def test_missing_native_file_is_not_prepared_peer_authority(self):
        result, _, descriptions = self.configure(probe=False, missing=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("docker", descriptions["groundhog-mdk011-interop-control"]["requires"])
        self.assertNotIn("prepared_peer", descriptions["groundhog-mdk011-interop-control"]["requires"])


if __name__ == "__main__":
    unittest.main()
