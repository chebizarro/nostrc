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


class RelayJsonDependencyEdges(Project):
    """Replay current authored links; fixture bodies do not prove relay behavior."""

    edges = (("relayd", "libjson", "test_relay_req_contract", "libjson/src/json.c",
              "libjson/include/nostr_jansson.h"),
             ("shared.tests", "libjson", "test_connection_recv_drain", "libjson/src/json.c",
              "libjson/include/nostr_jansson.h"),
             ("grelay", "relayd", "grelay_ingress_security", "apps/relayd/src/relay_ingress.c",
              "apps/relayd/include/relay_ingress.h"))

    def setUp(self):
        super().setUp()
        self.policy = s.load_inventory(ROOT / "tests/selection/inventory.json")

        def statement(path, command, target, required=()):
            source = (ROOT / path).read_text()
            matches = re.findall(r"\b" + command + r"\(" + re.escape(target) + r"\s+[^()]*\)", source)
            matches = [value for value in matches if set(required) <= set(value[:-1].split()[1:])]
            self.assertEqual(len(matches), 1, (path, command, target, required))
            return matches[0]

        relay = "apps/relayd/CMakeLists.txt"
        self.security = statement(relay, "add_library", "relay_security")
        server = statement(relay, "add_library", "nostr-relay-server")
        json_target = statement("libjson/CMakeLists.txt", "add_library", "nostr_json")
        self.assertEqual(json_target, "add_library(nostr_json SHARED src/json.c)")
        # grelay's fallback must retain the same three relay-owned source inputs.
        fallback = statement("apps/grelay/CMakeLists.txt", "add_library", "relay_security")
        self.assertEqual(set(re.findall(r"src/[\w.-]+\.c", self.security)),
                         set(re.findall(r"src/[\w.-]+\.c", fallback)))
        self.assertIn('#include "nostr_jansson.h"', (ROOT / self.edges[0][3]).read_text())
        self.assertIn('#include "relay_ingress.h"', (ROOT / self.edges[2][3]).read_text())
        libraries = ["add_library(nostr INTERFACE)", "add_library(libnostr INTERFACE)"]
        libraries += [f"add_library({name} INTERFACE IMPORTED)" for name in
                      ("OpenSSL::SSL", "OpenSSL::Crypto", "Threads::Threads")]
        (self.root / "libjson/src").mkdir(parents=True)
        (self.root / "libjson/CMakeLists.txt").write_text(json_target + "\n")
        relay_dir = self.root / "apps/relayd"
        relay_dir.mkdir(parents=True)
        (relay_dir / "CMakeLists.txt").write_text("\n".join([
            self.security, server,
            statement(relay, "target_link_libraries", "relay_security"),
            statement(relay, "target_link_libraries", "nostr-relay-server", ["nostr_json"]),
        ]) + "\n")
        sources = {"libjson/src/json.c"}
        sources.update("apps/relayd/" + path for path in
                       re.findall(r"src/[\w.-]+\.c", self.security + server))
        for path in sorted(sources):
            source = self.root / path
            source.parent.mkdir(parents=True, exist_ok=True)
            header = (self.root / "libjson/include/nostr_jansson.h" if path.startswith("libjson/")
                      else self.root / "apps/relayd/include/relay_ingress.h")
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text("#define FIXTURE_VALUE 1\n")
            source.write_text(f'#include "{header}"\nstatic int unused_fixture_body;\n')
        consumers = (
            ("test_relay_req_contract", "apps/relayd/tests/CMakeLists.txt", "relayd"),
            ("test_connection_recv_drain", "tests/CMakeLists.txt", "shared.tests"),
            ("grelay_ingress_security", "apps/grelay/tests/CMakeLists.txt", "grelay"),
        )
        commands = ["cmake_minimum_required(VERSION 3.22)", "project(relay_json_edges C)",
                    "enable_testing()", "include(cmake/NostrcTestInventory.cmake)",
                    *libraries, "add_subdirectory(libjson)", "add_subdirectory(apps/relayd)"]
        for name, path, owner in consumers:
            commands += [f"add_executable({name} main.c)",
                         statement(path, "target_link_libraries", name),
                         f"add_test(NAME {name} COMMAND {name})",
                         f"nostrc_describe_test({name} OWNER {owner} TARGETS {name} INPUTS vector.txt)"]
        commands += ["add_test(NAME omitted COMMAND test_relay_req_contract)",
                     "nostrc_describe_test(omitted OWNER relayd TARGETS test_relay_req_contract INPUTS vector.txt)",
                     "nostrc_finalize_test_inventory()"]
        (self.root / "CMakeLists.txt").write_text("\n".join(commands) + "\n")
        self.inventory = {
            "nodes": copy.deepcopy(self.policy["nodes"]),
            "path_rules": copy.deepcopy(self.policy["path_rules"]) + [
                {"exact": path, "nodes": [], "effect": "global"} for path in ("main.c", "shared.h")
            ] + [{"exact": "vector.txt", "nodes": ["vectors"]}],
            "checks": {name: {"kind": "ctest", "profiles": ["synthetic"],
                              "ctest_tests": {"synthetic": [name]}, "nodes": [owner, "vectors"]}
                       for name, _, owner in consumers},
        }
        self.inventory["nodes"]["vectors"] = {"depends_on": []}

    def test_authored_link_closures_and_full_source_header_mutations(self):
        realized = self.configure()
        by_name = {target["name"]: target for target in realized["targets"].values()}
        for _, _, consumer, source, _ in self.edges:
            closure = r._target_closure([by_name[consumer]["id"]], realized)
            inputs = {v["path"] for key in closure for v in realized["targets"][key]["sources"]}
            self.assertIn(str(self.root / source), inputs)
        pre = r.check_inputs(self.inventory, realized)
        self.build_all()
        post = self.audit(realized)
        expected = [{"kind": "unclaimed_registration", "test": "omitted"},
                    {"kind": "missing_impact_edge", "test": "omitted", "input": "vector.txt",
                     "input_kind": "runtime", "owners": ["relayd"], "missing_nodes": ["vectors"]}]
        self.assertEqual(pre["gaps"], expected)
        self.assertEqual(post["gaps"], expected)
        self.assertFalse(post["passed"])
        self.assertGreater(post["header_entries"], 0)
        for owner, dependency, consumer, source, header in self.edges:
            broken = copy.deepcopy(self.inventory)
            broken["nodes"][owner]["depends_on"].remove(dependency)
            for after_build, good in ((False, pre), (True, post)):
                with self.subTest(owner=owner, dependency=dependency, after_build=after_build):
                    bad = r.check_inputs(broken, realized, after_build=after_build)
                    self.assertEqual(bad["input_hashes"], good["input_hashes"])
                    self.assertEqual(bad["header_entries"], good["header_entries"])
                    lost_sources = (("apps/relayd/src/rate_limit.c", "apps/relayd/src/relay_ingress.c",
                                     "apps/relayd/src/relay_policy.c") if owner == "grelay" else (source,))
                    lost_inputs = [(path, "source") for path in lost_sources]
                    if after_build:
                        lost_inputs += [(path, kind) for path in lost_sources for kind in ("header", "ninja")]
                        lost_inputs.append((header, "header"))
                    mutation_expected = copy.deepcopy(expected)
                    for path, kind in lost_inputs:
                        for test in ([consumer, "omitted"] if owner == "relayd" else [consumer]):
                            mutation_expected.append({"kind": "missing_impact_edge", "test": test,
                                                      "input": path, "input_kind": kind, "owners": [owner],
                                                      "missing_nodes": [dependency]})
                        mutation_expected.append({"kind": "missing_impact_edge", "check": consumer,
                                                  "test": consumer, "support_test": consumer, "input": path,
                                                  "input_kind": kind, "owners": [owner, "vectors"],
                                                  "missing_nodes": [dependency]})
                    self.assertCountEqual(bad["gaps"], mutation_expected)
                    self.assertFalse(bad["passed"])
        self.assertEqual(r.load_realization(self.build, self.identity), realized)

    def test_actual_selector_plan_deltas_and_ui_mdk_isolation(self):
        from supported_profile_support import before_supported_batch_policy
        self.policy = before_supported_batch_policy(self.policy)
        previous = copy.deepcopy(self.policy)
        for owner, dependency, _, _, _ in self.edges:
            previous["nodes"][owner]["depends_on"].remove(dependency)
        for path, additions in (
            ("libjson/src/json.c", {"sanitizer.groundhog_store_key_keyring"}),
            ("libjson/include/nostr_jansson.h", {"sanitizer.groundhog_store_key_keyring"}),
            ("apps/relayd/src/relay_ingress.c", set()),
            ("apps/relayd/include/relay_ingress.h", set()),
            ("nostr-gtk/data/ui/gnostr-composer.blp", set()),
            ("nostr-gtk/src/note-card-binding-ctx.h", set()),
        ):
            with self.subTest(path=path):
                before, after = [s.make_plan(policy, [change(path)], head="a" * 40, base="b" * 40, context="ci")
                                 for policy in (previous, self.policy)]
                self.assertEqual(after["effective_mode"], "affected")
                self.assertEqual(after["effective_mode"], before["effective_mode"])
                self.assertEqual(set(after["selected_checks"]) - set(before["selected_checks"]), additions)
                self.assertEqual(set(before["selected_checks"]) - set(after["selected_checks"]), set())
                if path.startswith("nostr-gtk/"):
                    self.assertFalse(any(name.startswith(("mdk08.", "mdk011.")) for name in after["selected_checks"]))
        for owner in ("groundhog.unclassified", "execution.groundhog"):
            self.assertEqual(self.policy["nodes"][owner]["depends_on"], [])


class OrdinaryMediaOwnership(Project):
    """Replay authored target/descriptor metadata; stub bodies are not product proof."""

    def setUp(self):
        super().setUp()
        self.policy = s.load_inventory(ROOT / "tests/selection/inventory.json")
        source = (ROOT / "gnome/groundhog/CMakeLists.txt").read_text()

        def statement(command, target):
            matches = re.findall(r"\b" + command + r"\(" + re.escape(target) + r"\s+[^()]*\)", source)
            self.assertEqual(len(matches), 1, (command, target))
            return matches[0]

        statements = [
            statement("add_library", "groundhog-conversations"),
            *re.findall(r"\btarget_sources\(groundhog-conversations\s+[^()]*\)", source),
            statement("add_library", "groundhog-media"),
            statement("target_sources", "groundhog-media"),
            statement("target_link_libraries", "groundhog-media"),
            statement("add_executable", "test-groundhog-media"),
            statement("target_link_libraries", "test-groundhog-media"),
            statement("add_test", "NAME groundhog-media"),
            statement("set_tests_properties", "groundhog-media"),
        ]
        self.assertEqual(len(statements), 10)
        gh = self.root / "gnome/groundhog"
        gh.mkdir(parents=True)
        (gh / "TestInventory.cmake").write_bytes((ROOT / "gnome/groundhog/TestInventory.cmake").read_bytes())
        self.inputs = sorted(set(re.findall(r"(?:src|tests)/[\w/.-]+\.c", "\n".join(statements))))
        self.unclassified = []
        for path in self.inputs:
            relative = "gnome/groundhog/" + path
            if s.classify(self.policy, relative)["nodes"] == ["groundhog.unclassified"]:
                self.unclassified.append(relative)
            target = gh / path
            target.parent.mkdir(parents=True, exist_ok=True)
            if path == "tests/media/test_media.c":
                target.write_text("int main(void) { return 0; }\n")
            elif path == "src/app/gh-agent-event.c":
                target.with_suffix(".h").write_text("#define FIXTURE_VALUE 1\n")
                target.write_text('#include "gh-agent-event.h"\nint fixture_value = FIXTURE_VALUE;\n')
            else:
                target.write_text("static int unused_fixture_body;\n")
        (gh / "CMakeLists.txt").write_text("\n".join([
            "add_library(groundhog-net INTERFACE)", "add_library(groundhog-store INTERFACE)",
            "add_library(OpenSSL::Crypto INTERFACE IMPORTED)", *statements,
            "include(TestInventory.cmake)",
        ]) + "\n")
        (self.root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.22)
project(ordinary_media_ownership C)
enable_testing()
include(cmake/NostrcTestInventory.cmake)
add_subdirectory(gnome/groundhog)
add_executable(omitted main.c)
add_test(NAME omitted COMMAND omitted)
nostrc_describe_test(omitted OWNER groundhog.unclassified TARGETS omitted INPUTS vector.txt)
nostrc_finalize_test_inventory()
''')
        check = copy.deepcopy(self.policy["checks"]["groundhog.media"])
        check.update(profiles=["synthetic"], ctest_tests={"synthetic": ["groundhog-media"]})
        self.inventory = {
            "nodes": copy.deepcopy(self.policy["nodes"]),
            "path_rules": copy.deepcopy(self.policy["path_rules"]) + [
                {"exact": path, "nodes": [], "effect": "global"}
                for path in ("main.c", "shared.h", "CMakeLists.txt")
            ] + [{"exact": "vector.txt", "nodes": ["vectors"]}],
            "checks": {"groundhog.media": check},
        }
        self.inventory["nodes"]["vectors"] = {"depends_on": []}

    def test_authored_descriptor_and_transitive_source_header_audits(self):
        realized = self.configure()
        tests, descriptors = r._test_maps(realized)
        descriptor = descriptors["groundhog-media"]
        self.assertEqual(descriptor["owner"], ["groundhog.unclassified"])
        self.assertEqual(descriptor["targets"], [])
        closure = r._target_closure(r._mapped_targets(tests["groundhog-media"], descriptor, realized), realized)
        actual = {v["path"] for key in closure for v in realized["targets"][key]["sources"]}
        self.assertEqual(actual, {str(self.root / "gnome/groundhog" / path) for path in self.inputs})
        self.assertEqual(len(self.unclassified), 11)
        before_build = r.check_inputs(self.inventory, realized)
        self.assertEqual({g["kind"] for g in before_build["gaps"]},
                         {"unclaimed_registration", "missing_impact_edge"})
        self.assertTrue(all(g.get("test") == "omitted" for g in before_build["gaps"]))
        self.build_all()
        good = self.audit(realized)
        self.assertGreater(good["header_entries"], 0)
        self.assertEqual(good["gaps"], before_build["gaps"])
        self.assertFalse(good["passed"])
        broken = copy.deepcopy(self.inventory)
        broken["checks"]["groundhog.media"]["nodes"].remove("groundhog.unclassified")
        for after_build in (False, True):
            with self.subTest(after_build=after_build):
                bad = r.check_inputs(broken, realized, after_build=after_build)
                reference = good if after_build else before_build
                self.assertEqual(bad["input_hashes"], reference["input_hashes"])
                self.assertEqual(bad["header_entries"], reference["header_entries"])
                self.assertEqual([g for g in bad["gaps"] if g.get("test") == "omitted"], good["gaps"])
                media = [g for g in bad["gaps"] if g.get("test") == "groundhog-media"]
                self.assertEqual([g for g in media if g["kind"] == "check_descriptor_owner_disagreement"], [
                    {"kind": "check_descriptor_owner_disagreement", "test": "groundhog-media",
                     "owners": ["groundhog.unclassified"]}])
                self.assertEqual({g["input"] for g in media if g.get("input_kind") == "source"}, set(self.unclassified))
                headers = {g["input"] for g in media if g.get("input_kind") == "header"}
                self.assertEqual(headers, set(self.unclassified) | {"gnome/groundhog/src/app/gh-agent-event.h"}
                                 if after_build else set())
        self.assertEqual(r.load_realization(self.build, self.identity), realized)

    def test_exact_check_owner_and_existing_selector_boundaries(self):
        from supported_profile_support import before_supported_batch_policy
        self.policy = before_supported_batch_policy(self.policy)
        check = self.policy["checks"]["groundhog.media"]
        self.assertEqual(check["nodes"], ["groundhog.media", "execution.groundhog", "groundhog.unclassified"])
        self.assertEqual(check["profiles"], ["groundhog.linux"])
        self.assertEqual(check["ctest_tests"], {"groundhog.linux": ["groundhog-media"]})
        self.assertEqual(check["build_targets"], ["test-groundhog-media"])
        for owner in ("groundhog.unclassified", "execution.groundhog"):
            self.assertEqual(self.policy["nodes"][owner]["depends_on"], [])
        for path in self.unclassified + ["gnome/groundhog/src/ui/gh-recipient.c"]:
            impacted = set(s.impact_closure(self.policy, s.classify(self.policy, path)["nodes"]))
            self.assertTrue(set(check["nodes"]) & impacted)
            self.assertFalse((set(check["nodes"]) - {"groundhog.unclassified"}) & impacted)
        for path in ("nostr-gtk/data/ui/gnostr-composer.blp", "nostr-gtk/src/note-card-binding-ctx.h"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40, base="b" * 40, context="ci")
            self.assertFalse(any(name.startswith(("mdk08.", "mdk011.")) for name in plan["selected_checks"]))
