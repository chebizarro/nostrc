"""Policy adversaries only; these receipts are not native execution evidence."""
import copy
import json
from selector_support import ROOT, s, policy, plan, receipt
import unittest


class DiagnosticEligibilityTests(unittest.TestCase):
    def native_shape(self):
        p = policy()
        checked = json.loads((ROOT / "tests/selection/inventory.json").read_text())
        profile = "libgo.macos.asan"
        p["profiles"] = {profile: checked["profiles"][profile]}
        for check in p["checks"].values():
            check["profiles"] = [profile]
            check["ctest_tests"][profile] = check["ctest_tests"].pop("linux")
        known = copy.deepcopy(next(g for g in checked["coverage_gaps"] if g["id"] == "libgo_macos_asan_stress_exclusion"))
        p["coverage_gaps"] = [known]
        selected = plan(p)
        result = receipt(p, selected)
        result.update(diagnostic_eligible=True, complete_gate_eligible=False, coverage_gaps=[known],
            baseline_exclusions=[dict(known, status="not_executed", authority="existing_workflow_baseline",
                                      target_ids=["stress-target-id"], audit_build_targets=["go_channel_stress_test"])])
        return p, selected, result

    def test_approved_exact_exclusion_is_partial_only_never_stress_pass(self):
        p, selected, result = self.native_shape()
        partial = s.aggregate(p, selected, [result], complete=False)
        self.assertTrue(partial["passed"], partial)
        self.assertTrue(partial["diagnostic_eligible"])
        self.assertFalse(partial["complete_gate_eligible"])
        complete = s.aggregate(p, selected, [result])
        self.assertFalse(complete["passed"])
        self.assertFalse(complete["complete_gate_eligible"])
        self.assertTrue(any("diagnostic/incomplete" in e for e in complete["errors"]))
        self.assertEqual(result["baseline_exclusions"][0]["status"], "not_executed")
        self.assertNotIn("GoChannelStressTest", result["checks"])

    def test_unknown_mutated_duplicate_and_claimed_exclusions_still_block_partial(self):
        for change in ({"id": "new_waiver"}, {"status": "passed"}, {"registrations": ["Unknown"]},
                       {"authority": "caller"}, {"audit_build_targets": []}, {"extra": "waiver"}):
            with self.subTest(change=change):
                p, selected, result = self.native_shape()
                result["baseline_exclusions"][0].update(change)
                self.assertFalse(s.aggregate(p, selected, [result], complete=False)["passed"])
        p, selected, result = self.native_shape()
        result["baseline_exclusions"] *= 2
        self.assertFalse(s.aggregate(p, selected, [result], complete=False)["passed"])

    def test_selected_skip_missing_check_or_cancelled_worker_still_blocks_partial(self):
        for outcome in ("unexpected_skip", "missing", "cancelled", "failed"):
            with self.subTest(outcome=outcome):
                p, selected, result = self.native_shape()
                next(iter(result["checks"].values()))["outcome"] = outcome
                self.assertFalse(s.aggregate(p, selected, [result], complete=False)["passed"])
        p, selected, result = self.native_shape()
        result["checks"].pop(next(iter(result["checks"])))
        self.assertFalse(s.aggregate(p, selected, [result], complete=False)["passed"])
        p, selected, result = self.native_shape()
        jobs = {lane["id"]: "cancelled" for lane in selected["lanes"]}
        self.assertFalse(s.aggregate(p, selected, [result], complete=False, job_conclusions=jobs)["passed"])

    def test_nested_diagnostic_flags_cannot_be_stripped_for_complete_gate(self):
        p = policy()
        selected = plan(p)
        result = receipt(p, selected)
        result["backend_receipt"] = {"complete_gate_eligible": False, "diagnostic_eligible": True}
        self.assertFalse(s.aggregate(p, selected, [result])["passed"])
        result["complete_gate_eligible"] = True
        self.assertFalse(s.aggregate(p, selected, [result], complete=False)["passed"])

    def test_omitted_outer_diagnostic_flag_cannot_hide_nested_negative(self):
        p, selected, result = self.native_shape()
        result.pop("diagnostic_eligible")
        result["backend_receipt"] = {"diagnostic_eligible": False, "complete_gate_eligible": False}
        partial = s.aggregate(p, selected, [result], complete=False)
        self.assertFalse(partial["passed"], partial)
        self.assertFalse(partial["diagnostic_eligible"])
        self.assertFalse(partial["complete_gate_eligible"])
        self.assertTrue(any("diagnostic execution did not qualify" in e for e in partial["errors"]))
        self.assertFalse(s.aggregate(p, selected, [result])["passed"])

    def test_omitted_outer_positive_backend_remains_diagnostic_only(self):
        p, selected, result = self.native_shape()
        result.pop("diagnostic_eligible")
        result["backend_receipt"] = {"diagnostic_eligible": True, "complete_gate_eligible": False}
        partial = s.aggregate(p, selected, [result], complete=False)
        self.assertTrue(partial["passed"], partial)
        self.assertFalse(partial["complete_gate_eligible"])
        self.assertFalse(s.aggregate(p, selected, [result])["passed"])

    def test_inventory_gaps_alone_block_complete_even_if_receipt_drops_flags(self):
        p, selected, result = self.native_shape()
        legacy = receipt(p, selected)
        self.assertFalse(s.aggregate(p, selected, [legacy])["passed"])

    def test_release_cannot_accept_partial_diagnostic_validation(self):
        p, selected, result = self.native_shape()
        partial = s.aggregate(p, selected, [result], complete=False)
        release = {"schema_version": 1, "tested_sha": selected["tested_sha"], "source_identity": selected["source_identity"],
                   "inventory_hash": selected["inventory_hash"], "tag": None, "required_package_jobs": [], "publish": False}
        # Release aggregate's complete/mode/readiness guards are independent of
        # a successful diagnostic job or a caller's optimistic passed flag.
        validation = dict(partial, passed=True, mode="broad")
        self.assertFalse(s.aggregate_release(release, validation, {})["passed"])
