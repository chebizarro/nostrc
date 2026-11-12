"""Selection/descriptor contracts only, not call runtime or hosted CI proof."""
import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from groundhog_call_support import call_skip_policy
from selector_support import ROOT, change, s


CALLS = {
    "groundhog-calls", "groundhog-call-capabilities", "groundhog-call-loopback",
    "groundhog-call-media", "groundhog-call-media-two-process",
    "groundhog-call-settings", "groundhog-call-settings-store-failure",
    "groundhog-call-ui", "groundhog-call-preferences",
}


class GroundhogCallSelection(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / "tests/selection/inventory.json")

    def test_authored_registrations_have_one_ordinary_owner_not_sanitizer_claims(self):
        source = "\n".join((ROOT / path).read_text() for path in (
            "gnome/groundhog/CMakeLists.txt",
            "gnome/groundhog/cmake/GroundhogCalls.cmake",
            "gnome/groundhog/cmake/GroundhogCallMedia.cmake"))
        authored = set(re.findall(r"add_test\(NAME (groundhog-calls?[-\w]*)\s", source))
        self.assertEqual(authored, CALLS)
        for name in sorted(CALLS):
            owners = [check for check in self.policy["checks"].values()
                      if name in check.get("ctest_tests", {}).get("groundhog.linux", [])]
            self.assertEqual(len(owners), 1, name)
            check = owners[0]
            self.assertEqual(check["profiles"], ["groundhog.linux"])
            self.assertEqual(check["expected_outcome"], "pass")
            target = "test-groundhog-call-media" if name.endswith("-two-process") else "test-" + name
            self.assertEqual(check["build_targets"], [target])
        sanitizer = self.policy["profiles"]["groundhog.linux.sanitizers"]["sanitizer_compatibility"]
        # nostrc-44vhc: adding hosted call coverage requires a deliberate workflow change.
        self.assertFalse(CALLS & set(sanitizer["GH_SAN_TESTS"]))
        for suffix in ("ui", "preferences"):
            self.assertIn("display", self.policy["checks"]["groundhog.call_" + suffix]["requires"])

    def test_call_inputs_select_consumers_and_docs_remain_non_execution(self):
        cases = {
            "gnome/groundhog/src/app/gh-call-service.c": "groundhog.calls",
            "gnome/groundhog/tests/app/test_call_transport.c": "groundhog.calls",
            "gnome/groundhog/tests/fixtures/nipac-nospeak.json": "groundhog.calls",
            "gnome/groundhog/src/media/gh-call-media.c": "groundhog.call_media",
            "gnome/groundhog/tests/media/calls/two_process.py": "groundhog.call_media_two_process",
            "gnome/groundhog/tests/store/test_call_settings.c": "groundhog.call_settings",
            "gnome/groundhog/src/ui/gh-call-view.c": "groundhog.call_ui",
            "gnome/groundhog/tests/ui/test_call_preferences.c": "groundhog.call_preferences",
        }
        for path, check in cases.items():
            self.assertTrue((ROOT / path).is_file())
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40, profiles=["groundhog.linux"])
            # Preserve the descriptor's existing conservative component owner.
            owner = s.classify(self.policy, path)["nodes"]
            expected_mode = "broad" if "groundhog.unclassified" in owner else "affected"
            self.assertEqual(plan["effective_mode"], expected_mode, path)
            self.assertIn(check, plan["selected_checks"], path)
        for path in ("gnome/groundhog/cmake/GroundhogCalls.cmake",
                     "gnome/groundhog/cmake/GroundhogCallMedia.cmake"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40)
            self.assertIn("selection.contract", plan["selected_checks"])
            self.assertTrue({"groundhog." + n.removeprefix("groundhog-").replace("-", "_")
                             for n in CALLS} <= set(plan["selected_checks"]))
        for path in ("docs/groundhog-call-signalling.md", "docs/groundhog-call-interop-runbook.md"):
            self.assertEqual(s.make_plan(self.policy, [change(path)], head="a" * 40)["selected_checks"], [])
        probe = s.classify(self.policy, "docs/investigations/gst-libav-avpacket-leak-2026-11-11-repro.c")
        self.assertEqual(probe["effect"], "global")
        self.assertFalse(probe["unclassified"])
        signer = s.make_plan(self.policy, [change("nips/nip55l/src/glib/signer_service_g.c")],
                             head="a" * 40)
        self.assertIn("groundhog.calls", signer["selected_checks"])
        self.assertNotIn("groundhog.call_ui", signer["selected_checks"])
        self.assertNotIn("groundhog.call_media", signer["selected_checks"])

    def test_inherited_required_runtime_is_bound_without_flattening_sanitizers(self):
        dockerfile = (ROOT / "scripts/linux-ci.Dockerfile").read_text()
        self.assertIn("ENV GROUNDHOG_CALL_TEST_REQUIRE_RUNTIME=1", dockerfile)
        self.assertIn("libgstreamer-plugins-bad1.0-dev libnice-dev gstreamer1.0-nice", dockerfile)
        for name in ("groundhog.linux", "groundhog.mdk011", "groundhog.linux.sanitizers"):
            phases = self.policy["profiles"][name]["phase_environments"]
            for phase in ("configure", "build", "test"):
                self.assertEqual(phases[phase]["GROUNDHOG_CALL_TEST_REQUIRE_RUNTIME"], "1")
            for phase in ("configure", "build"):
                self.assertNotIn("ASAN_OPTIONS", phases[phase])
                self.assertNotIn("LSAN_OPTIONS", phases[phase])

    def test_descriptor_projection_executes_authored_skip_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.22)\nproject(call_policy NONE)\nenable_testing()\n"
                + call_skip_policy()
                + "\n".join(f"add_test(NAME {name} COMMAND ${{CMAKE_COMMAND}} -E true)"
                            for name in sorted(CALLS))
                + "\ngroundhog_call_test_skip_policy()\n")
            result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(["ctest", "--test-dir", str(root / "build"), "--show-only=json-v1"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            tests = json.loads(result.stdout)["tests"]
            skipped = {test["name"] for test in tests for prop in test["properties"]
                       if prop["name"] == "SKIP_RETURN_CODE" and prop["value"] == 77}
            self.assertEqual(skipped, {"groundhog-call-capabilities", "groundhog-call-loopback",
                                       "groundhog-call-media", "groundhog-call-media-two-process"})


if __name__ == "__main__":
    unittest.main()
