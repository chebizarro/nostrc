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

    def test_porthome_move_retains_real_consumers_and_omits_prerequisite_suites(self):
        for path in ("libporthome/src/crypto.c", "libporthome/src/index.c",
                     "libporthome/include/porthome/compat-nh.h"):
            with self.subTest(path=path):
                self.assertEqual(s.classify(self.policy, path)["nodes"], ["libporthome"])
                plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                                   base="b" * 40, context="ci")
                selected = set(plan["selected_checks"])
                self.assertTrue({"supported.libporthome", "supported.libhanami",
                                 "homed.units", "nostr_seal.unit",
                                 "nostr_seal.streaming"} <= selected)
                self.assertNotIn("supported.libblossom", selected)
                self.assertNotIn("supported.nips.bud02", selected)

    def test_porthome_test_only_input_does_not_select_hanami_or_homed(self):
        plan = s.make_plan(self.policy, [change("libporthome/tests/test_api.c")],
                           head="a" * 40, base="b" * 40, context="ci")
        self.assertEqual(set(plan["selected_checks"]),
                         {"supported.libporthome", "root.asan.libporthome"})

    def test_blossom_runtime_selects_glib_consumers_not_upstream_suites(self):
        plan = s.make_plan(self.policy, [change("libblossom/src/client.c")],
                           head="a" * 40, base="b" * 40, context="ci")
        selected = set(plan["selected_checks"])
        self.assertEqual(plan["effective_mode"], "affected")
        self.assertTrue({"supported.nostr.gobject.blossom", "groundhog.blossom",
                         "groundhog.media"} <= selected)
        self.assertFalse({"root.asan.libgo", "root.asan.nip17"} & selected)
        self.assertIn("nostr.gobject", self.policy["nodes"]["gnome.nostr_share"]["depends_on"])
        test_plan = s.make_plan(self.policy, [change("libblossom/tests/test_blossom_guards.c")],
                                head="a" * 40, base="b" * 40, context="ci")
        self.assertNotIn("supported.nostr.gobject.blossom", test_plan["selected_checks"])

    def test_unrelated_product_edit_does_not_select_boundary_tool_regressions(self):
        plan = s.make_plan(self.policy, [change("libgo/src/channel.c")],
                           head="a" * 40, base="b" * 40, context="ci")
        self.assertFalse(set(plan["selected_checks"]) & {
            "package.boundaries.guard", "package.boundaries.regressions"
        })
