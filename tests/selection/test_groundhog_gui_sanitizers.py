"""Focused Groundhog GUI sanitizer recipe/adapter regression coverage."""
import copy
import runpy
import unittest

from selector_support import ROOT, s
from realization_support import r
from groundhog_gui_sanitizer_support import historical_nonmember_policy


class GroundhogGuiSanitizers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.adapter = runpy.run_path(str(ROOT / "scripts/sanitizer-gate-ci.py"))
        cls.workflow = (ROOT / ".github/workflows/groundhog-ci.yml").read_text()

    def config(self, text=None):
        return self.adapter["job_config"](self.workflow if text is None else text, "/src")

    def test_gui_suites_and_dependency_control_are_selected(self):
        config = self.config()
        self.assertTrue({"groundhog-shell", "groundhog-preferences", "groundhog-conversation-view",
                         "groundhog-account-store-gui", "groundhog-gtk-lsan"}
                        <= set(config["GH_SAN_TESTS"]))
        self.assertEqual(config["GH_SAN_DISPLAY_WRAP"], "1")
        env = dict(value.split("=", 1) for value in config["GH_SAN_ENV"])
        self.assertIn("detect_leaks=1", env["ASAN_OPTIONS"])
        self.assertIn("malloc_context_size=64", env["ASAN_OPTIONS"])
        self.assertNotIn("LD_LIBRARY_PATH", env)
        self.assertNotIn("PKG_CONFIG_PATH", env)
        call_runtime = ["GROUNDHOG_CALL_TEST_REQUIRE_RUNTIME=1"]
        self.assertEqual(config["GH_SAN_CONFIGURE_ENV"], call_runtime)
        self.assertEqual(config["GH_SAN_BUILD_ENV"], call_runtime)
        self.assertEqual(env["GROUNDHOG_CALL_TEST_REQUIRE_RUNTIME"], "1")
        self.assertEqual(env["LSAN_OPTIONS"],
                         "suppressions=/src/gnome/groundhog/tests/lsan.supp:print_suppressions=1")

    def test_withdrawn_nonmember_exemption_cannot_omit_gui_suites(self):
        policy = s.load_inventory(ROOT / "tests/selection/inventory.json")
        self.assertEqual(policy["diagnostic_nonmembers"], {})
        self.assertEqual(s.diagnostic_nonmember_contracts(policy), {})
        self.assertEqual(r._diagnostic_nonmembers(policy, {}, {}, {}), [])

        historical = historical_nonmember_policy()
        profile = "groundhog.linux.sanitizers"
        contract = s.diagnostic_nonmember_contracts(historical)[profile]
        current = set(self.config()["GH_SAN_TESTS"])
        previous = set(historical["profiles"][profile]["sanitizer_compatibility"]["GH_SAN_TESTS"])
        self.assertTrue(previous < current)
        self.assertTrue({"groundhog-shell", "groundhog-preferences", "groundhog-conversation-view"}
                        <= current & set(contract["registrations"]))
        # Historical provenance is not a renewal for the expanded GUI profile.
        stale = copy.deepcopy(policy)
        stale["diagnostic_nonmembers"] = historical["diagnostic_nonmembers"]
        with self.assertRaisesRegex(ValueError, "malformed/stale"):
            s.diagnostic_nonmember_contracts(stale)

    def test_stock_libraries_and_context_independent_image(self):
        dockerfile = (ROOT / "scripts/linux-ci.Dockerfile").read_text()
        self.assertNotIn("COPY ", dockerfile)
        self.assertNotIn("ENV LD_LIBRARY_PATH", dockerfile)
        self.assertNotIn("ENV PKG_CONFIG_PATH", dockerfile)
        self.assertNotIn("sassc", dockerfile)
        self.assertIn("libadwaita-1-0", dockerfile)
        local = (ROOT / "scripts/groundhog-linux-ci.sh").read_text()
        gate = (ROOT / "scripts/linux-gate.sh").read_text()
        self.assertIn('- < "$ROOT/scripts/linux-ci.Dockerfile"', local)
        self.assertIn('- < "$SCRIPTS/linux-ci.Dockerfile"', gate)
        self.assertNotIn("LD_LIBRARY_PATH", self.workflow)
        self.assertNotIn("PKG_CONFIG_PATH", self.workflow)

    def test_gui_variants_build_their_real_shared_executable(self):
        targets = self.config()["GH_SAN_TARGETS"]
        self.assertIn("test-groundhog-account-store", targets)
        self.assertNotIn("test-groundhog-account-store-gui", targets)
        self.assertIn("groundhog", targets)  # groundhog-launch is the app's --smoke route
        self.assertNotIn("test-groundhog-launch", targets)
        self.assertEqual(len(targets), len(set(targets)))

    def test_ordinary_local_mode_resets_cached_instrumentation(self):
        script = (ROOT / "scripts/groundhog-linux-ci.sh").read_text()
        ordinary = script.split('else\n  cmake -S /src', 1)[1].split('fi\nfor i', 1)[0]
        self.assertIn('-DGNOSTR_ENABLE_ASAN=OFF', ordinary)
        self.assertIn('-DGNOSTR_ENABLE_UBSAN=OFF', ordinary)

    def test_unsupported_display_wrapper_fails_closed(self):
        changed = self.workflow.replace("-screen 0 1280x800x24", "-screen 0 800x600x24")
        with self.assertRaises(self.adapter["WorkflowError"]):
            self.config(changed)

    def test_only_reviewed_library_function_suppressions(self):
        text = (ROOT / "gnome/groundhog/tests/lsan.supp").read_text()
        entries = {line for line in text.splitlines() if line.startswith("leak:")}
        self.assertEqual(entries, {"leak:secret_collection_signal", "leak:match_in_thread",
                                   "leak:pango_fc_font_map_load_fontset", "leak:FcParsePattern",
                                   "leak:pango_fc_fontset_load_next_font",
                                   "leak:gtk_widget_list_mnemonic_labels",
                                   "leak:gtk_shortcut_label_rebuild",
                                   "leak:adw_breakpoint_bin_size_allocate",
                                   "leak:adw_about_dialog_add_legal_section"})


if __name__ == "__main__":
    unittest.main()
