"""Production source evidence regressions; no miniature or hosted-pass equivalence."""
import copy
import json
import unittest

from selector_support import ROOT, s
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

    def test_all_observed_failed_hosted_source_edges_reach_their_checks(self):
        evidence = json.loads((ROOT / "tests/selection/fixtures/hosted_audit_edges.json").read_text())
        self.assertEqual(len(evidence["audits"]), 4)
        for audit in evidence["audits"]:
            if audit["profile"] == "groundhog.linux.sanitizers":
                self.assertEqual(audit["source_sha"], "b8dd3583766893e3b97e017dc063da5ed1c47413")
                self.assertIn(audit["evidence_kind"], {"native_production_configure_input_audit_failed", "native_production_postbuild_input_audit_failed"})
                if audit["evidence_kind"] == "native_production_postbuild_input_audit_failed":
                    self.assertEqual(audit["header_entries"], 7605712)
                    self.assertEqual(audit["input_hash_count"], 3316)
                    self.assertTrue(any(edge["input_kind"] == "header" for edge in audit["edges"]))
                self.assertEqual(audit["actual_architecture"], "aarch64")
                self.assertEqual(audit["declared_architecture"], "x86_64")
            else:
                self.assertEqual(audit["source_sha"], "8a031f443ac84850345a4cf0fb6ab5a8c3b32814")
                self.assertEqual(audit["evidence_kind"], "configure_source_input_audit_failed")
            self.assertGreater(audit["original_gap_count"], 1000)
            for edge in audit["edges"]:
                check = self.policy["checks"][edge["check"]]
                with self.subTest(profile=audit["profile"], check=edge["check"], input=edge["input"]):
                    self.assertIn(edge["test"], check["ctest_tests"][audit["profile"]])
                    self.assertTrue(set(edge["required_nodes"]) <= self.prerequisites(check["nodes"]))
                    self.assertTrue(set(s.classify(self.policy, edge["input"])["nodes"]) <= self.prerequisites(check["nodes"]))

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
        self.assertEqual(len(blocks), 87)
        self.assertEqual(validate_job_accounting(self.policy, ROOT),
                         {key: spec["matrix_source"] for key, spec in blocks.items()})
        for key, record in self.policy["workflow_jobs"].items():
            self.assertEqual(record["source_sha256"], blocks[key]["source_sha256"])
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
