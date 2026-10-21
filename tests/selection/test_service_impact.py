"""Exact service producers/consumers; no daemon-to-GUI or catchall edges."""
import copy
import re
import shutil

from selector_support import ROOT, change, s
from realization_support import Project, r


class ServiceImpact(Project):
    def setUp(self):
        super().setUp()
        self.policy = s.load_inventory(ROOT / "tests/selection/inventory.json")

    def selected(self, path):
        plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                           base="b" * 40, context="ci")
        self.assertEqual(plan["effective_mode"], "affected")
        self.assertFalse(plan["broadening_reasons"])
        return set(plan["selected_checks"])

    def test_exact_shared_producer_and_distinct_activation_templates(self):
        expected = {
            "apps/grotto/CMakeLists.txt": ["grotto.daemon", "grotto.gui", "version.contract"],
            "apps/grotto/data/org.nostr.Signer.service.in": ["grotto.daemon", "selection.contract"],
            "apps/grotto/data/org.nostr.Grotto.service.in": ["grotto.gui", "selection.contract"],
        }
        for path, nodes in expected.items():
            self.assertEqual(s.classify(self.policy, path)["nodes"], nodes)
        for path in ("apps/grotto/src/main.c", "apps/grotto/data/org.nostr.Grotto.desktop"):
            self.assertEqual(s.classify(self.policy, path)["nodes"], ["grotto.gui"])
        self.assertNotIn("grotto.gui", self.policy["nodes"]["grotto.daemon"]["depends_on"])
        self.assertNotIn("grotto.daemon", self.policy["nodes"]["grotto.gui"]["depends_on"])
        for path in expected:
            selected = self.selected(path)
            self.assertIn("grotto.signer.native", selected)
            self.assertIn("selection.contract", selected)
            self.assertFalse(any(n.startswith(("mdk08.", "mdk011.")) for n in selected))

    def test_real_services_audit_each_owner_and_keep_omissions_visible(self):
        # Execute the actual two configure/helper and add_test statements, with
        # the real descriptors/templates/checker, but no unrelated application.
        app = self.root / "apps/grotto"
        (app / "data").mkdir(parents=True)
        for name in ("CheckDbusServiceFile.cmake", "NostrcConfigureProvenance.cmake"):
            shutil.copy2(ROOT / "cmake" / name, self.root / "cmake")
        for name in ("org.nostr.Grotto.service.in", "org.nostr.Signer.service.in",
                     "org.nostr.Grotto.desktop"):
            shutil.copy2(ROOT / "apps/grotto/data" / name, app / "data")
        shutil.copy2(ROOT / "apps/grotto/TestInventory.cmake", app)
        source = (ROOT / "apps/grotto/CMakeLists.txt").read_text()
        commands = ["set(APP_SIGNER_FLATPAK_ID org.nostr.Grotto)",
                    "set(CMAKE_INSTALL_FULL_BINDIR /usr/bin)"]
        names = ("grotto_dbus_service_configured", "grotto_app_dbus_service_configured")
        for file, name in zip(("org.nostr.Signer", "org.nostr.Grotto"), names):
            configure = re.search(r"configure_file\(data/" + re.escape(file) +
                                  r"\.service\.in\b.*?\nendif\(\)", source, re.S)
            test = re.search(r"add_test\(NAME " + name + r"\b.*?\n  set_tests_properties\("
                             + name + r"[^\n]*\)", source, re.S)
            self.assertIsNotNone(configure)
            self.assertIsNotNone(test)
            commands.extend([configure[0], test[0]])
        commands.append("include(TestInventory.cmake)")
        (app / "CMakeLists.txt").write_text("\n".join(commands) + "\n")
        top = self.root / "CMakeLists.txt"
        top.write_text(top.read_text().replace("nostrc_finalize_test_inventory()",
                       "add_subdirectory(apps/grotto)\nnostrc_finalize_test_inventory()"))
        self.inventory["nodes"].update(copy.deepcopy(self.policy["nodes"]))
        paths = {"apps/grotto/CMakeLists.txt",
                 "apps/grotto/data/org.nostr.Signer.service.in",
                 "apps/grotto/data/org.nostr.Grotto.service.in"}
        self.inventory["path_rules"] += [copy.deepcopy(rule) for rule in self.policy["path_rules"]
                                        if rule.get("exact") in paths]
        self.inventory["path_rules"].append({"prefix": "apps/grotto/data/", "nodes": ["grotto.gui"]})
        realized = self.configure()
        self.assertEqual({t["name"] for t in realized["targets"].values()}, {"shared"})
        for name in names:
            desc = next(d for d in realized["descriptors"] if d["name"] == name)
            test = next(t for t in realized["tests"] if t["name"] == name)
            self.assertEqual(r._mapped_targets(test, desc, realized), [])
        audit = r.check_inputs(self.inventory, realized)
        omissions = [{"kind": "unclaimed_registration", "test": n} for n in names]
        self.assertCountEqual(audit["gaps"], omissions)
        self.assertFalse(audit["passed"])
        self.assertIn(str(app / "CMakeLists.txt"), audit["input_hashes"])
        baseline = copy.deepcopy(self.inventory)
        for rule in baseline["path_rules"]:
            if rule.get("exact") == "apps/grotto/CMakeLists.txt":
                rule["nodes"].remove("grotto.daemon")
            elif rule.get("exact") == "apps/grotto/data/org.nostr.Signer.service.in":
                rule["nodes"] = ["grotto.gui", "selection.contract"]
        before = r.check_inputs(baseline, realized)
        removed = [g for g in before["gaps"] if g not in audit["gaps"]]
        self.assertEqual({g["input"] for g in removed},
                         {"apps/grotto/CMakeLists.txt", "apps/grotto/data/org.nostr.Signer.service.in"})
        self.assertTrue(all(g["kind"] == "missing_impact_edge" and g["test"] == names[0]
                            for g in removed))
        self.assertEqual(before["input_hashes"], audit["input_hashes"])
        for path in paths:
            with self.subTest(missing_owner=path):
                broken = copy.deepcopy(self.inventory)
                rule = next(rule for rule in broken["path_rules"] if rule.get("exact") == path)
                rule["nodes"] = ["other"]
                gaps = r.check_inputs(broken, realized)["gaps"]
                self.assertTrue(any(g["kind"] == "missing_impact_edge" and g.get("input") == path
                                    for g in gaps), gaps)
                self.assertTrue(all(g in gaps for g in omissions))
        self.build_all()
        post = self.audit(realized)
        self.assertCountEqual(post["gaps"], omissions)
        self.assertGreater(post["header_entries"], 0)
        r._command(["ctest", "--test-dir", str(self.build), "-R",
                    "^grotto_(app_)?dbus_service_configured$", "--output-on-failure"])
        self.assertEqual(r.load_realization(self.build, self.identity), realized)

    def test_groundhog_template_already_reaches_declared_background_check(self):
        cmake = (ROOT / "gnome/groundhog/CMakeLists.txt").read_text()
        source = (ROOT / "gnome/groundhog/src/app/gh-background.c").read_text()
        self.assertIn("configure_file(${GROUNDHOG_DATA}/org.nostr.Groundhog-autostart.desktop.in", cmake)
        self.assertIn("file(READ ${GROUNDHOG_AUTOSTART_DESKTOP} GROUNDHOG_AUTOSTART_TEMPLATE)", cmake)
        self.assertIn("file(CONFIGURE OUTPUT ${GROUNDHOG_BACKGROUND_GENERATED}/gh-background-autostart.h", cmake)
        self.assertIn("add_library(groundhog-background STATIC src/app/gh-background.c)", cmake)
        self.assertIn('#include "gh-background-autostart.h"', source)
        self.assertIn("GH_BACKGROUND_AUTOSTART_TEMPLATE", source)
        template = "gnome/groundhog/data/org.nostr.Groundhog-autostart.desktop.in"
        self.assertEqual(s.classify(self.policy, template)["nodes"], ["groundhog.resources"])
        self.assertIn("sanitizer.groundhog_background", self.selected(template))
        self.assertEqual(self.policy["checks"]["sanitizer.groundhog_background"]["ctest_tests"],
                         {"groundhog.linux.sanitizers": ["groundhog-background"]})
        # Do not solve ordinary unclaimed coverage with a dependency on all UI.
        for node in ("groundhog.unclassified", "execution.groundhog"):
            self.assertEqual(self.policy["nodes"][node]["depends_on"], [])
        self.assertFalse(any(n.startswith(("mdk08.", "mdk011.")) for n in self.selected(template)))
