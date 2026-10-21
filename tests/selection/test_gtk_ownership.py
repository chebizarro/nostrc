"""CMake-grounded portable/legacy/resource ownership, not production acceptance."""
import copy
import re
import unittest

from selector_support import ROOT, change, s


class GtkOwnership(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / "tests/selection/inventory.json")
        self.cmake = (ROOT / "nostr-gtk/CMakeLists.txt").read_text()

    def source_list(self, name):
        # These authored sets contain literal paths and references to other sets.
        match = re.search(r"set\(" + name + r"\s+(.*?)\)", self.cmake, re.S)
        self.assertIsNotNone(match, name)
        paths = set()
        for word in match[1].split():
            if word.startswith("${"):
                paths.update(self.source_list(word[2:-1]))
            else:
                self.assertTrue(word.startswith(("src/", "include/")), word)
                paths.add("nostr-gtk/" + word)
        return paths

    def selected(self, policy, path):
        plan = s.make_plan(policy, [change(path)], head="a" * 40, base="b" * 40,
                           context="ci")
        self.assertEqual(plan["effective_mode"], "affected")
        self.assertFalse(plan["broadening_reasons"])
        return set(plan["selected_checks"])

    def test_actual_target_source_and_header_sets_have_distinct_owners(self):
        portable = self.source_list("NOSTR_GTK_PORTABLE_SOURCES")
        portable |= self.source_list("NOSTR_GTK_ISSUE_MODEL_SOURCES")
        portable |= self.source_list("NOSTR_GTK_PORTABLE_HEADERS")
        portable |= {"nostr-gtk/src/content_renderer.c", "nostr-gtk/src/nostr-gtk-error.c",
                     "nostr-gtk/src/gn-media-decode-private.h",
                     "nostr-gtk/src/gn-portable-i18n-private.h"}
        legacy = self.source_list("NOSTR_GTK_CORE_SOURCES")
        legacy |= self.source_list("NOSTR_GTK_GNOSTR_COUPLED_SOURCES")
        legacy |= self.source_list("NOSTR_GTK_HEADERS") - portable
        legacy |= {"nostr-gtk/src/note-card-binding-ctx.h", "nostr-gtk/src/note-card-data.h",
                   "nostr-gtk/include/nostr-gtk-1.0/gnostr-timeline-view-private.h"}
        self.assertTrue(portable.isdisjoint(legacy))
        for paths, owner in ((portable, "nostr.gtk.portable"), (legacy, "nostr.gtk")):
            for path in sorted(paths):
                with self.subTest(path=path):
                    self.assertTrue((ROOT / path).is_file())
                    self.assertEqual(s.classify(self.policy, path)["nodes"], [owner])
        self.assertIn("$<TARGET_OBJECTS:gn_content_parser> $<TARGET_OBJECTS:gn_issue_model>", self.cmake)
        self.assertIn("target_link_libraries(nostr_gtk PUBLIC nostr_gtk_portable)", self.cmake)
        self.assertRegex(self.cmake, r"set\(NOSTR_GTK_SOURCES\s+\$\{NOSTR_GTK_CORE_SOURCES\}\s+"
                         r"\$\{NOSTR_GTK_GNOSTR_COUPLED_SOURCES\}\s+\$\{GRESOURCE_C\}\s*\)")
        self.assertEqual(self.policy["nodes"]["nostr.gtk"]["depends_on"],
                         ["nostr.gtk.portable", "nostr.gtk.resources"])
        self.assertNotIn("nostr.gtk", self.policy["nodes"]["groundhog.conversations"]["depends_on"])
        self.assertIn("nostr.gtk.portable", self.policy["nodes"]["groundhog.conversations"]["depends_on"])

    def test_every_tracked_resource_and_legacy_input_avoids_mdk_but_reaches_ui(self):
        paths = [str(path.relative_to(ROOT)) for path in (ROOT / "nostr-gtk/data").rglob("*")
                 if path.is_file()]
        self.assertGreater(len(paths), 10)
        for path in paths:
            self.assertEqual(s.classify(self.policy, path)["nodes"], ["nostr.gtk.resources"])
        paths += sorted(self.source_list("NOSTR_GTK_CORE_SOURCES") |
                        self.source_list("NOSTR_GTK_GNOSTR_COUPLED_SOURCES"))
        for path in paths:
            with self.subTest(path=path):
                selected = self.selected(self.policy, path)
                self.assertIn("gnostr.resource_build", selected)
                if "/data/" in path:
                    self.assertIn("nostr.gtk.resource_build", selected)
                self.assertFalse(any(name.startswith(("mdk08.", "mdk011.")) for name in selected))
                self.assertNotIn("groundhog.media", selected)
                self.assertNotIn("groundhog.mls_service", selected)

    def test_resource_and_portable_edges_are_independently_required(self):
        resource = "nostr-gtk/data/ui/gnostr-composer.blp"
        good = self.selected(self.policy, resource)
        broken = copy.deepcopy(self.policy)
        broken["nodes"]["nostr.gtk"]["depends_on"].remove("nostr.gtk.resources")
        bad = self.selected(broken, resource)
        self.assertIn("gnostr.resource_build", good)
        self.assertNotIn("gnostr.resource_build", bad)
        # The direct resource contract remains selected even when UI impact is broken.
        self.assertIn("nostr.gtk.resource_build", bad)
        broken = copy.deepcopy(self.policy)
        broken["nodes"]["gnostr"]["depends_on"].remove("nostr.gtk")
        self.assertNotIn("gnostr.resource_build", self.selected(broken, resource))
        live = {name for name in self.policy["checks"] if name.startswith(("mdk08.", "mdk011."))}
        for path in ("nostr-gtk/src/gn-nostr-reference.c",
                     "nostr-gtk/include/nostr-gtk-1.0/gn-nostr-reference.h",
                     "nostr-gtk/src/gn-media-decode-private.h"):
            with self.subTest(path=path):
                self.assertTrue(live <= self.selected(self.policy, path))
        for node in ("groundhog.unclassified", "execution.groundhog"):
            self.assertEqual(self.policy["nodes"][node]["depends_on"], [])


if __name__ == "__main__":
    unittest.main()
