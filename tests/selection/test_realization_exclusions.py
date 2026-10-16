"""Narrow baseline accounting: an excluded registration is never a passed test."""
import copy
import json
import unittest
from unittest import mock
from realization_support import ROOT, r


def baseline_fixture():
    checked = json.loads((ROOT / "tests/selection/inventory.json").read_text())
    profile_id = "libgo.macos.asan"
    profile = copy.deepcopy(checked["profiles"][profile_id])
    record = copy.deepcopy(next(g for g in checked["coverage_gaps"]
                               if g["id"] == "libgo_macos_asan_stress_exclusion"))
    ident = "stress-target-id"
    test = {"name": "GoChannelStressTest", "command": ["/build/go_channel_stress_test"], "properties": []}
    descriptor = {"name": test["name"], "owner": ["libgo"], "targets": ["go_channel_stress_test"],
                  "inputs": [], "requires": [], "expected": [], "declared_depends": []}
    policy = {"profiles": {profile_id: profile}, "coverage_gaps": [record], "checks": {},
              "nodes": {"libgo": {}}, "path_rules": []}
    realization = {"profile_id": profile_id, "profile": profile, "repo_root": "/source", "build_dir": "/build",
                   "source_identity": {"kind": "committed", "sha": "fixture"}, "inventory_hash": "fixture",
                   "profile_fingerprint": "fixture-fingerprint",
                   "phase_context": {"template_inputs": []}, "phase_context_hash": "fixture-phase-context",
                   "tests": [test], "descriptors": [descriptor],
                   "targets": {ident: {"id": ident, "name": "go_channel_stress_test", "type": "EXECUTABLE",
                                        "artifacts": test["command"], "dependencies": [],
                                        "compile_fragments": ["-fsanitize=address -fsanitize=undefined"],
                                        "sources": [{"path": "/source/libgo/tests/go_channel_stress_test.c", "generated": False}]}}}
    return policy, realization


def exclusions(policy, realization):
    tests, descriptors = r._metadata(policy, realization)
    return r._baseline_exclusions(policy, realization, tests, descriptors)


class BaselineExclusionTests(unittest.TestCase):
    def test_exact_baseline_is_accounted_but_never_completed(self):
        policy, realization = baseline_fixture()
        result = exclusions(policy, realization)
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["registrations"], ["GoChannelStressTest"])
        self.assertEqual(result[0]["status"], "not_executed")
        self.assertEqual(result[0]["audit_build_targets"], ["go_channel_stress_test"])
        self.assertNotIn("outcome", result[0])
        self.assertNotIn("passed", result[0])

    def test_exception_is_not_applied_to_another_profile(self):
        policy, realization = baseline_fixture()
        realization["profile_id"] = "libgo.linux.asan"
        realization["tests"] = []
        realization["descriptors"] = []
        self.assertEqual(exclusions(policy, realization), [])

    def test_malformed_records_never_become_waivers(self):
        changes = [
            {"id": "arbitrary_exclusion"}, {"profile": "libgo.linux.asan"},
            {"registrations": ["*"]}, {"registrations": []}, {"registrations": "GoChannelStressTest"},
            {"registrations": ["GoChannelStressTest", "UnknownTest"]},
            {"reason": ""}, {"tracking": ""}, {"allow_unclaimed": True},
        ]
        for change in changes:
            with self.subTest(change=change):
                policy, realization = baseline_fixture()
                policy["coverage_gaps"][0].update(change)
                with self.assertRaises(r.RealizationError):
                    exclusions(policy, realization)
        policy, realization = baseline_fixture()
        policy["coverage_gaps"].append(copy.deepcopy(policy["coverage_gaps"][0]))
        with self.assertRaisesRegex(r.RealizationError, "duplicate"):
            exclusions(policy, realization)

    def test_changed_profile_or_test_mapping_invalidates_exclusion(self):
        for change in ({"instrumentation": "none"}, {"source_dir": "."}, {"architecture": "x86_64"},
                       {"configure_args": ["-DGO_ENABLE_ASAN=ON"]}):
            with self.subTest(change=change):
                policy, realization = baseline_fixture()
                policy["profiles"]["libgo.macos.asan"].update(change)
                with self.assertRaises(r.RealizationError):
                    exclusions(policy, realization)
        policy, realization = baseline_fixture()
        realization["targets"]["stress-target-id"]["sources"][0]["path"] = "/source/libgo/tests/not_the_stress_test.c"
        with self.assertRaisesRegex(r.RealizationError, "source mapping"):
            exclusions(policy, realization)
        policy, realization = baseline_fixture()
        realization["targets"]["stress-target-id"]["compile_fragments"] = ["-fsanitize=thread"]
        with self.assertRaisesRegex(r.RealizationError, "instrumentation differs"):
            exclusions(policy, realization)
        policy, realization = baseline_fixture()
        policy["profiles"]["libgo.macos.asan"]["configure_args"].append("-DGO_ENABLE_ASAN=OFF")
        with self.assertRaisesRegex(r.RealizationError, "native variant"):
            exclusions(policy, realization)
        policy, realization = baseline_fixture()
        realization["tests"][0]["properties"] = [{"name": "DISABLED", "value": True}]
        with self.assertRaisesRegex(r.RealizationError, "metadata changed"):
            exclusions(policy, realization)
        policy, realization = baseline_fixture()
        realization["tests"] = []
        realization["descriptors"] = []
        with self.assertRaisesRegex(r.RealizationError, "registration is missing"):
            exclusions(policy, realization)

    def test_claimed_or_required_stress_case_cannot_be_excluded(self):
        policy, realization = baseline_fixture()
        policy["checks"]["stress"] = {"profiles": ["libgo.macos.asan"], "ctest_tests": {"libgo.macos.asan": ["GoChannelStressTest"]}}
        with self.assertRaisesRegex(r.RealizationError, "also claimed"):
            exclusions(policy, realization)
        policy["checks"] = {"regular": {"kind": "ctest", "nodes": ["libgo"], "profiles": ["libgo.macos.asan"],
                                          "ctest_tests": {"libgo.macos.asan": ["Regular"]}}}
        realization["tests"].append({"name": "Regular", "properties": [{"name": "DEPENDS", "value": ["GoChannelStressTest"]}]})
        realization["descriptors"].append({"name": "Regular", "owner": ["libgo"], "declared_depends": ["GoChannelStressTest"]})
        # This policy-only fixture deliberately has nonexistent /source paths;
        # native phase custody is covered by real phase-environment projects.
        with mock.patch.object(r, "_validate_phase_context"), self.assertRaisesRegex(r.RealizationError, "required by selected"):
            r.resolve_checks(policy, {"selected_checks": ["regular"]}, realization, "libgo.macos.asan")

    def test_excluded_sources_and_unknown_registrations_are_still_audited(self):
        policy, realization = baseline_fixture()
        realization["tests"].append({"name": "UnknownTest", "command": ["/build/go_channel_stress_test"], "properties": []})
        realization["descriptors"].append({"name": "UnknownTest", "owner": ["libgo"], "targets": ["go_channel_stress_test"]})
        with mock.patch.object(r, "_validate_phase_context"):
            audit = r.check_inputs(policy, realization)
        self.assertFalse(audit["passed"])
        self.assertFalse(audit["complete_gate_eligible"])
        self.assertEqual([g["test"] for g in audit["gaps"] if g["kind"] == "unclaimed_registration"], ["UnknownTest"])
        self.assertTrue(any(g["kind"] == "missing_input" and g["test"] == "GoChannelStressTest" for g in audit["gaps"]))
        self.assertEqual(audit["baseline_exclusions"][0]["status"], "not_executed")
