"""Authored link-statement/File API regressions, not production execution proof."""
import copy
from pathlib import Path
import re

from realization_support import Project, ROOT, r
from selector_support import change, s


EDGES = (
    ("nostr.gtk.portable", "nip34", "nips/nip34/src/nip34.c", "portable"),
    ("groundhog.identity_auth", "gnome.seahorse", "gnome/seahorse/secret_store.c", "identity"),
    ("groundhog.identity_auth", "groundhog.relay", "gnome/groundhog/src/relay/gh-relay-gnostr.c", "identity"),
    ("groundhog.group_ui", "groundhog.account_relays", "gnome/groundhog/src/app/gh-account-relays.c", "group"),
    ("groundhog.group_ui", "groundhog.net", "gnome/groundhog/src/net/gh-net-http.c", "group"),
)


class SharedDependencyEdges(Project):
    def setUp(self):
        super().setUp()
        self.policy = s.load_inventory(ROOT / "tests/selection/inventory.json")
        gh = (ROOT / "gnome/groundhog/CMakeLists.txt").read_text()
        gtk = (ROOT / "nostr-gtk/CMakeLists.txt").read_text()

        def statement(source, command, target, required):
            # Only these literal, parenthesis-free link statements are replayed.
            matches = re.findall(r"\b" + command + r"\(" + re.escape(target) + r"\s+[^()]*\)", source)
            matches = [value for value in matches if set(required) <= set(value[:-1].split()[1:])]
            self.assertEqual(len(matches), 1, (command, target, required))
            return matches[0]

        links = [
            statement(gtk, "nostrc_link_archives", "gn_issue_model", ["nip34"]),
            statement(gh, "target_link_libraries", "groundhog-identity", ["gnostr-secret", "nip19", "nostr"]),
            statement(gh, "target_link_libraries", "groundhog-identity", ["groundhog-nip46-session"]),
            statement(gh, "target_link_libraries", "groundhog-nip46-session", ["groundhog-relay", "nostr_nip46_core"]),
            statement(gh, "target_link_libraries", "groundhog-group-ui", ["groundhog-conversation-ui"]),
            statement(gh, "target_link_libraries", "groundhog-conversation-ui", ["groundhog-ui", "groundhog-dm-inbox"]),
            statement(gh, "target_link_libraries", "groundhog-dm-inbox", ["groundhog-account-relays", "groundhog-conversations"]),
            statement(gh, "target_link_libraries", "groundhog-conversation-ui", ["groundhog-net", "PkgConfig::GROUNDHOG_WEB_XML"]),
        ]
        self.assertIn("$<TARGET_OBJECTS:gn_content_parser> $<TARGET_OBJECTS:gn_issue_model>", gtk)
        root_cmake = (ROOT / "CMakeLists.txt").read_text()
        wrapper = re.search(r"function\(nostrc_link_archives target scope\).*?endfunction\(\)", root_cmake, re.S)
        self.assertIsNotNone(wrapper)
        leaves = {
            "nip34": EDGES[0][2], "gnostr-secret": EDGES[1][2],
            "groundhog-relay": EDGES[2][2], "groundhog-account-relays": EDGES[3][2],
            "groundhog-net": EDGES[4][2],
        }
        # Bodies are fixture code: only the actual target/link structure and
        # semantic input paths are under test, not these libraries' behavior.
        (self.root / "noop.c").write_text("int unused_fixture_body;\n")
        for path in leaves.values():
            file = self.root / path
            file.parent.mkdir(parents=True, exist_ok=True)
            file.with_suffix(".h").write_text("#define FIXTURE_DEPENDENCY 1\n")
            file.write_text('#include "' + file.with_suffix(".h").name + '"\nint dependency_fixture;\n')
        targets = set(leaves) | {
            "gn_issue_model", "groundhog-identity", "groundhog-nip46-session",
            "groundhog-group-ui", "groundhog-conversation-ui", "groundhog-dm-inbox",
            "groundhog-ui", "groundhog-nip29-service", "groundhog-recipient",
            "groundhog-privacy-summary", "groundhog-auth-policy", "groundhog-conversations",
            "nip19", "nostr", "nostr_nip46_core",
        }
        commands = ["cmake_minimum_required(VERSION 3.22)", "project(shared_edges C)",
                    "enable_testing()", "include(cmake/NostrcTestInventory.cmake)",
                    wrapper[0], "add_library(PkgConfig::GROUNDHOG_WEB_XML INTERFACE IMPORTED)"]
        for target in sorted(targets):
            kind = "OBJECT" if target == "gn_issue_model" else "STATIC"
            commands.append(f"add_library({target} {kind} {leaves.get(target, 'noop.c')})")
        commands += links + ["add_library(nostr_gtk_portable STATIC noop.c $<TARGET_OBJECTS:gn_issue_model>)"]
        consumers = {
            "portable": ("nostr_gtk_portable", "nostr.gtk.portable"),
            "identity": ("groundhog-identity", "groundhog.identity_auth"),
            "group": ("groundhog-group-ui", "groundhog.group_ui"),
            "omitted": ("groundhog-identity", "groundhog.identity_auth"),
        }
        for name, (target, owner) in consumers.items():
            commands += [f"add_executable(probe-{name} main.c)",
                         f"target_link_libraries(probe-{name} PRIVATE {target})",
                         f"add_test(NAME {name} COMMAND probe-{name})",
                         f"nostrc_describe_test({name} OWNER {owner} TARGETS probe-{name} INPUTS vector.txt)"]
        commands.append("nostrc_finalize_test_inventory()")
        (self.root / "CMakeLists.txt").write_text("\n".join(commands) + "\n")
        self.inventory = {
            "nodes": copy.deepcopy(self.policy["nodes"]),
            "path_rules": copy.deepcopy(self.policy["path_rules"]) + [
                {"exact": path, "nodes": [], "effect": "global"}
                for path in ("main.c", "noop.c", "shared.h")
            ] + [{"exact": "vector.txt", "nodes": ["vectors"]}],
            "checks": {name: {"kind": "ctest", "profiles": ["synthetic"],
                              "ctest_tests": {"synthetic": [name]}, "nodes": [owner, "vectors"]}
                       for name, (_, owner) in consumers.items() if name != "omitted"},
        }
        self.inventory["nodes"]["vectors"] = {"depends_on": []}

    def test_file_api_and_full_audits_require_each_exact_edge(self):
        realized = self.configure()
        by_name = {target["name"]: target for target in realized["targets"].values()}
        for _, _, path, consumer in EDGES:
            with self.subTest(consumer=consumer, path=path):
                closure = r._target_closure([by_name["probe-" + consumer]["id"]], realized)
                actual = {source["path"] for ident in closure for source in realized["targets"][ident]["sources"]}
                self.assertIn(str(self.root / path), actual)
        before_build = r.check_inputs(self.inventory, realized)
        self.assertEqual(before_build["gaps"], [
            {"kind": "unclaimed_registration", "test": "omitted"},
            {"kind": "missing_impact_edge", "test": "omitted", "input": "vector.txt",
             "input_kind": "runtime", "owners": ["groundhog.identity_auth"], "missing_nodes": ["vectors"]},
        ])
        self.build_all()
        good = self.audit(realized)
        self.assertGreater(good["header_entries"], 0)
        self.assertEqual(good["gaps"], before_build["gaps"])
        self.assertFalse(good["passed"])
        for owner, dependency, path, consumer in EDGES:
            with self.subTest(owner=owner, dependency=dependency):
                broken = copy.deepcopy(self.inventory)
                broken["nodes"][owner]["depends_on"].remove(dependency)
                bad = r.check_inputs(broken, realized, after_build=True)
                self.assertEqual(bad["input_hashes"], good["input_hashes"])
                self.assertEqual(bad["header_entries"], good["header_entries"])
                self.assertTrue(any(g.get("test") == consumer and g.get("input") == path
                                    and g["kind"] == "missing_impact_edge" for g in bad["gaps"]))
                self.assertTrue(any(g.get("test") == consumer and g.get("input") == str(Path(path).with_suffix('.h'))
                                    and g.get("input_kind") == "header" for g in bad["gaps"]))
                if consumer == "identity":
                    self.assertTrue(any(g.get("test") == "omitted" and g.get("input") == path for g in bad["gaps"]))
        runtime_broken = copy.deepcopy(self.inventory)
        runtime_broken["path_rules"][-1]["nodes"] = ["other-fixture"]
        runtime_broken["nodes"]["other-fixture"] = {"depends_on": []}
        self.assertTrue(any(g.get("input") == "vector.txt" and g.get("input_kind") == "runtime"
                            for g in r.check_inputs(runtime_broken, realized)["gaps"]))
        self.assertEqual(r.load_realization(self.build, self.identity), realized)

    def test_selector_edges_and_existing_boundaries(self):
        for owner, dependency, path, _ in EDGES:
            with self.subTest(owner=owner, dependency=dependency):
                policy = copy.deepcopy(self.policy)
                seeds = s.classify(policy, path)["nodes"]
                self.assertIn(owner, s.impact_closure(policy, seeds))
                policy["nodes"][owner]["depends_on"].remove(dependency)
                self.assertNotIn(owner, s.impact_closure(policy, seeds))
        # The shared secret-store's existing broad-recovery gap is not waived
        # merely because its concrete identity dependency is now represented.
        broad = s.make_plan(self.policy, [change(EDGES[1][2])], head="a" * 40, base="b" * 40, context="ci")
        self.assertEqual(broad["effective_mode"], "broad")
        self.assertTrue(broad["broadening_reasons"])
        header = "gnome/groundhog/src/app/gh-account-relays.h"
        self.assertIn('#include "gh-account-relays.h"', (ROOT / EDGES[3][2]).read_text())
        self.assertEqual(s.classify(self.policy, header)["nodes"], ["groundhog.account_relays"])
        for owner in ("groundhog.unclassified", "execution.groundhog"):
            self.assertEqual(self.policy["nodes"][owner]["depends_on"], [])
        for path in ("nostr-gtk/data/ui/gnostr-composer.blp", "nostr-gtk/src/note-card-binding-ctx.h"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40, base="b" * 40, context="ci")
            self.assertFalse(any(name.startswith(("mdk08.", "mdk011.")) for name in plan["selected_checks"]))
        plan = s.make_plan(self.policy, [change(EDGES[0][2])], head="a" * 40, base="b" * 40, context="ci")
        self.assertIn("groundhog.mls_service", plan["selected_checks"])
