"""Production source evidence regressions; no miniature or hosted-pass equivalence."""
import copy
import json
import shutil
import tempfile
from pathlib import Path
import unittest

from selector_support import ROOT, change, s
from profile_contract_support import source_job_blocks, validate_job_accounting


class ProductionInventory(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / "tests/selection/inventory.json")

    def prerequisites(self, nodes):
        result = set(nodes)
        pending = list(nodes)
        while pending:
            for node in self.policy["nodes"][pending.pop()].get("depends_on", []):
                if node not in result:
                    result.add(node)
                    pending.append(node)
        return result



    def test_schema_selection_regression_detects_removal_of_its_real_check_edge(self):
        policy = copy.deepcopy(self.policy)
        check = "sanitizer.groundhog_account"
        schema = "apps/gnostr/data/schemas/org.gnostr.gnostr.gschema.xml"
        policy["checks"][check]["nodes"].remove("gnostr.schema_inputs")
        bad = s.make_plan(policy, [change(schema)], head="a" * 40, base="b" * 40, context="ci")
        self.assertNotIn(check, bad["selected_checks"])
        good = s.make_plan(self.policy, [change(schema)], head="a" * 40, base="b" * 40, context="ci")
        self.assertIn(check, good["selected_checks"])

    def test_nostr_gobject_nip_link_family_selects_groundhog_media(self):
        # Failed hosted source 2743f17354ac13527d8d8919f81abc711396c959,
        # run 39132650260, artifact 12138633221: these seven source witnesses
        # each reported missing_impact_edge for groundhog.media / groundhog-media.
        # nostr-gobject/CMakeLists.txt's guarded NIP loop forwards these archives
        # to executable consumers via the root nostrc_link_archives helper.
        witnesses = {
            "nip04": "nips/nip04/src/nip04.c",
            "nip06": "nips/nip06/src/nip06.c",
            "nip11": "nips/nip11/src/nip11.c",
            "nip19": "nips/nip19/src/bech32.c",
            "nip44": "nips/nip44/src/core/nip44.c",
            "nip46": "nips/nip46/src/core/nip46_envelope.c",
            "nip49": "nips/nip49/src/core/nip49.c",
        }
        check = "groundhog.media"
        self.assertEqual(self.policy["checks"][check]["ctest_tests"]["groundhog.linux"],
                         ["groundhog-media"])
        self.assertTrue(set(witnesses) <= set(self.policy["nodes"]["nostr.gobject"]["depends_on"]))
        for node, path in witnesses.items():
            with self.subTest(node=node, input=path):
                self.assertEqual(s.classify(self.policy, path)["nodes"], [node])
                plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                                   base="b" * 40, context="ci")
                self.assertEqual(plan["effective_mode"], "affected")
                self.assertFalse(plan["broadening_reasons"])
                self.assertIn(check, plan["selected_checks"])
                self.assertNotIn("libgo.channel", plan["selected_checks"])
                broken = copy.deepcopy(self.policy)
                # Remove all prerequisite routes, including newer vault/blossom
                # routes that legitimately reach NIP-44 transitively.
                broken["nodes"]["nostr.gobject"]["depends_on"] = []
                bad = s.make_plan(broken, [change(path)], head="a" * 40,
                                  base="b" * 40, context="ci")
                self.assertNotIn(check, bad["selected_checks"])
        self.assertEqual(self.policy["nodes"]["groundhog.unclassified"]["depends_on"], [])
        self.assertEqual(self.policy["nodes"]["execution.groundhog"]["depends_on"], [])

    def test_conversations_portable_dependency_selects_transitive_consumers(self):
        # PR4 run 39522018295, source 033cc630b1b7f3b728166fe96026c796a70bd301:
        # both ordinary full preaudits report these portable sources through
        # groundhog-conversations (CMake PUBLIC NostrGtk::Portable, gh-message.c).
        consumers = {"groundhog.media", "groundhog.mls_service", "groundhog.mls_files",
                     "groundhog.mls_media", "groundhog.mls_ui"}
        broken = copy.deepcopy(self.policy)
        broken["nodes"]["groundhog.conversations"]["depends_on"] = [
            node for node in broken["nodes"]["groundhog.conversations"]["depends_on"]
            if node != "nostr.gtk.portable"]
        for path in ("nostr-gtk/src/gn-nostr-reference.c",
                     "nostr-gtk/include/nostr-gtk-1.0/gn-nostr-reference.h"):
            with self.subTest(input=path):
                self.assertEqual(s.classify(self.policy, path)["nodes"], ["nostr.gtk.portable"])
                good = s.make_plan(self.policy, [change(path)], head="a" * 40,
                                   base="b" * 40, context="ci")
                self.assertEqual(good["effective_mode"], "affected")
                self.assertFalse(good["broadening_reasons"])
                self.assertTrue(consumers <= set(good["selected_checks"]))
                self.assertNotIn("libgo.channel", good["selected_checks"])
                bad = s.make_plan(broken, [change(path)], head="a" * 40,
                                  base="b" * 40, context="ci")
                self.assertTrue(consumers.isdisjoint(bad["selected_checks"]))
        for node in ("groundhog.unclassified", "execution.groundhog"):
            self.assertEqual(self.policy["nodes"][node]["depends_on"], [])

    def test_test_vector_edges_do_not_become_production_dependencies(self):
        for node, spec in self.policy["nodes"].items():
            if node.endswith(".test_inputs") or node == "marmot.vector_inputs":
                continue
            self.assertFalse(any(dep.endswith(".test_inputs") or dep == "marmot.vector_inputs"
                                 for dep in spec.get("depends_on", [])), node)
        check = self.policy["checks"]["root.asan.nip46"]
        self.assertIn("nip46.test_inputs", check["nodes"])
        self.assertNotIn("nip46.test_inputs", self.policy["nodes"]["nip46"]["depends_on"])

    def test_linux_libgo_exact_membership_and_effective_environment(self):
        checks = self.policy["checks"]
        native = checks["libgo.native_standalone"]["ctest_tests"]
        self.assertEqual(set(native["libgo.linux.asan"]), set(native["libgo.macos.asan"]) | {"GoChannelStressTest"})
        members = [name for check in checks.values() for name in check.get("ctest_tests", {}).get("libgo.linux.asan", [])]
        self.assertEqual(len(members), 33)
        self.assertEqual(len(set(members)), 33)
        profile = self.policy["profiles"]["libgo.linux.asan"]
        self.assertEqual(profile["environment"]["ASAN_OPTIONS"], "detect_leaks=0")
        self.assertEqual(profile["workflow_contract"]["cmake_version"], "3.31.6")
        self.assertEqual(profile["setup"], ".github/workflows/libgo-ci.yml:build-and-test")
        self.assertNotIn("GoChannelStressTest", native["libgo.macos.asan"])
        self.assertFalse(self.policy["enforcement_ready"])

    def test_every_job_binds_its_setup_and_has_an_explicit_remainder(self):
        blocks = source_job_blocks(ROOT)
        self.assertEqual(set(blocks), set(self.policy['workflow_jobs']))
        self.assertIn("{source}/tests/selection/test_broad_setup_contracts.py",
                      self.policy["checks"]["selection.contract"]["runtime_inputs"])
        self.assertEqual(validate_job_accounting(self.policy, ROOT),
                         {key: spec["matrix_source"] for key, spec in blocks.items()})
        for key, record in self.policy["workflow_jobs"].items():
            self.assertEqual(record["source_sha256"], blocks[key]["source_sha256"])
            self.assertEqual(record["workflow_sha256"], blocks[key]["workflow_sha256"])
            self.assertTrue(record["unsupported_reason"].strip(), key)
            self.assertIn("coverage_gap", record)
        bad = copy.deepcopy(self.policy)
        bad["workflow_jobs"][".github/workflows/dependency-worker.yml:execute"]["source_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "setup/steps changed"):
            validate_job_accounting(bad, ROOT)
        bad = copy.deepcopy(self.policy)
        bad["workflow_jobs"][".github/workflows/libgo-ci.yml:build-and-test"]["unsupported_reason"] = ""
        with self.assertRaisesRegex(ValueError, "specific unsupported reason"):
            validate_job_accounting(bad, ROOT)

    def test_inherited_workflow_inputs_cannot_reuse_stale_job_accounting(self):
        original = source_job_blocks(ROOT)
        for field in ("environment", "defaults", "permissions", "call_input_default"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                shutil.copytree(ROOT / ".github/workflows", root / ".github/workflows")
                filename = "dependency-worker.yml" if field == "call_input_default" else "windows-build.yml"
                path = root / ".github/workflows" / filename
                text = path.read_text()
                if field == "environment":
                    self.assertIn("BUILD_TYPE: Release", text)
                    text = text.replace("BUILD_TYPE: Release", "BUILD_TYPE: Debug", 1)
                elif field == "defaults":
                    text = text.replace("jobs:\n", "defaults:\n  run:\n    shell: bash\njobs:\n", 1)
                elif field == "permissions":
                    self.assertIn("  contents: read", text)
                    text = text.replace("  contents: read", "  contents: write", 1)
                else:
                    text = text.replace("permissions:\n", "      build_type:\n        type: string\n        default: Debug\npermissions:\n", 1)
                path.write_text(text)
                changed = source_job_blocks(root)
                # Effective inherited inputs drift even though every literal job is identical.
                self.assertEqual({key: block["source_sha256"] for key, block in original.items()},
                                 {key: block["source_sha256"] for key, block in changed.items()})
                with self.assertRaisesRegex(ValueError, "source/inherited settings changed"):
                    validate_job_accounting(self.policy, root)
        bad = copy.deepcopy(self.policy)
        bad["workflow_jobs"][".github/workflows/windows-build.yml:build"]["workflow_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "source/inherited settings changed"):
            validate_job_accounting(bad, ROOT)

    def test_worker_repairs_real_hosted_setup_failures_without_waivers(self):
        worker = (ROOT / ".github/workflows/dependency-worker.yml").read_text()
        for command in ["gettext locales", "gnome-keyring", "gstreamer1.0-plugins-bad", "cmake==3.31.6",
                        "blueprint-compiler-0.22.2.tar.xz", "231d72efbac931c235ac3e022fe94982095c20d88721d9a8dcf60152f2017e07",
                        "sha256sum -c", "blueprint-compiler --version | grep -x 0.22.2",
                        "libsecret-1-0-dbgsym_${version}_${arch}.ddeb"]:
            self.assertIn(command, worker)
        self.assertNotIn("detect_leaks=0", worker)
        self.assertNotIn("continue-on-error", worker)
        self.assertNotIn("ctest --repeat", worker)


if __name__ == "__main__":
    unittest.main()
