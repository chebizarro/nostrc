"""Package boundary tools have executable, bounded affected-check ownership."""
import unittest

from selector_support import ROOT, change, s


class PackageBoundarySelection(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.policy = s.load_inventory(ROOT / "tests/selection/inventory.json")

    def test_script_and_allowlist_edits_select_only_boundary_commands(self):
        paths = (
            "scripts/check-package-boundaries.sh",
            "scripts/check-package-boundaries.py",
            "scripts/test-package-boundaries.py",
            "scripts/package-boundaries.allowlist",
        )
        expected = {"package.boundaries.guard", "package.boundaries.regressions"}
        for path in paths:
            for context in ("ci", "local"):
                with self.subTest(path=path, context=context):
                    self.assertEqual(s.classify(self.policy, path)["nodes"], ["package.boundaries"])
                    plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                                       base="b" * 40, context=context)
                    self.assertEqual(set(plan["selected_checks"]), expected)
                    for name in expected:
                        check = self.policy["checks"][name]
                        self.assertEqual(check["kind"], "command")
                        self.assertEqual(check["build_targets"], [])
                        self.assertIn("selection.operational.linux", check["profiles"])
                        self.assertIn("selection.operational.macos", check["profiles"])

    def test_unrelated_product_edit_does_not_select_boundary_tool_regressions(self):
        plan = s.make_plan(self.policy, [change("libgo/src/channel.c")],
                           head="a" * 40, base="b" * 40, context="ci")
        self.assertFalse(set(plan["selected_checks"]) & {
            "package.boundaries.guard", "package.boundaries.regressions"
        })
