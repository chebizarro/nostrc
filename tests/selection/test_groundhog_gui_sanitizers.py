"""Focused Groundhog GUI sanitizer recipe/adapter regression coverage."""
import runpy
import unittest

from selector_support import ROOT


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
        self.assertEqual(env["LD_LIBRARY_PATH"], "/opt/groundhog-ci-gtk/lib")
        self.assertEqual(env["LSAN_OPTIONS"],
                         "suppressions=/src/gnome/groundhog/tests/lsan.supp:print_suppressions=1")

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

    def test_no_per_operation_dependency_or_application_suppression(self):
        text = (ROOT / "gnome/groundhog/tests/lsan.supp").read_text()
        entries = {line for line in text.splitlines() if line.startswith("leak:")}
        self.assertEqual(entries, {"leak:secret_collection_signal", "leak:match_in_thread",
                                   "leak:pango_fc_font_map_load_fontset", "leak:FcParsePattern",
                                   "leak:pango_fc_fontset_load_next_font"})


if __name__ == "__main__":
    unittest.main()
