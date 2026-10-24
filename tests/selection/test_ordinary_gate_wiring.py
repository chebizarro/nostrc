"""Ordinary static coverage is independent of explicit qualification diagnostics."""
import importlib.util
import runpy
import unittest

from selector_support import ROOT


class OrdinaryGateWiringTests(unittest.TestCase):
    def test_operational_family_map_does_not_load_experimental_contract(self):
        path = ROOT / ".github/workflows/ci_families.py"
        spec = importlib.util.spec_from_file_location("ci_families", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual(module.BROAD_FAMILIES["static"], "static-checks")
        self.assertNotIn("dependency_contract", path.read_text())

    def test_ordinary_static_runs_operational_tests_and_original_host_checks(self):
        text = (ROOT / ".github/workflows/static-checks.yml").read_text()
        self.assertIn("python3 scripts/test-selection-operational.py", text)
        for forbidden in ("namespace_policy_controller", "namespace_apparmor", "static_frontend_host", "docker "):
            self.assertNotIn(forbidden, text)
        for command in (
            "python3 scripts/check-unsequenced-args.py --self-test",
            "python3 scripts/check-gui-test-gdk-frame.py --self-test",
            "python3 scripts/check-linux-ci-packages.py --self-test",
            "bash scripts/test-pre-push.sh", "bash scripts/test-full-gate.sh",
            "bash scripts/test-linux-gate-smoke.sh", "bash scripts/test-sanitizer-gate.sh",
            "bash scripts/check-grotto-names.sh", "bash scripts/test-check-macos-keychain.sh",
        ):
            self.assertIn("run: " + command, text)

    def test_namespace_qualification_is_manual_and_keeps_cleanup_and_evidence(self):
        text = (ROOT / ".github/workflows/namespace-policy-regression.yml").read_text()
        self.assertIn("  workflow_dispatch:", text)
        self.assertIn("  workflow_call:", text)  # retained for explicit diagnostic orchestration
        self.assertNotIn("  push:", text)
        self.assertNotIn("  pull_request:", text)
        self.assertNotIn("  schedule:", text)
        self.assertIn("namespace_policy_controller.sh execute", text)
        self.assertIn("namespace_policy_controller.sh remove", text)
        self.assertIn("namespace_apparmor_teardown.sh", text)
        self.assertIn("actions/upload-artifact@v4", text)
        self.assertGreaterEqual(text.count("if: always()"), 3)

    def test_operational_suite_keeps_selection_runner_and_workflow_tests(self):
        config = runpy.run_path(str(ROOT / "scripts/test-selection-operational.py"))
        modules = set(config["MODULES"])
        self.assertTrue({"test_selector", "test_git_ranges", "test_local_selective_delivery",
                         "test_workflow_defaults", "test_cmake_inventory_compat"} <= modules)
        self.assertFalse(any("custody" in name or "enrollment" in name or "w11" in name for name in modules))


if __name__ == "__main__":
    unittest.main()
