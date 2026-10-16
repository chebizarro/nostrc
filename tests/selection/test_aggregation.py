import copy
import unittest
from selector_support import s, policy, plan, receipt, GitCase


class AggregateTests(unittest.TestCase):
    def setUp(self):
        self.p = policy()
        self.plan = plan(self.p)
        self.receipt = receipt(self.p, self.plan)

    def aggregate(self, *receipts, **kwargs):
        return s.aggregate(self.p, self.plan, list(receipts), **kwargs)

    def test_complete_pass(self):
        self.assertTrue(self.aggregate(self.receipt)["passed"])

    def test_missing_duplicate_unexpected_lanes_fail(self):
        self.assertFalse(self.aggregate()["passed"])
        self.assertFalse(self.aggregate(self.receipt, self.receipt)["passed"])
        self.receipt["lane_id"] = "unexpected"
        self.assertFalse(self.aggregate(self.receipt)["passed"])

    def test_selected_outcomes_closed_and_nonpassing_rejected(self):
        for outcome in ("failed", "cancelled", "missing", "unexpected_skip", "not_applicable", "advisory_failure", "expected_incompatibility", "quarantined_failure"):
            with self.subTest(outcome=outcome):
                self.receipt["checks"]["app.unit"]["outcome"] = outcome
                self.assertFalse(self.aggregate(self.receipt)["passed"])
        self.receipt["checks"]["app.unit"]["outcome"] = "skipped_but_fine"
        with self.assertRaises(s.ContractError):
            self.aggregate(self.receipt)

    def test_identity_mismatches_fail(self):
        for key in ("plan_id", "inventory_hash", "source_identity", "profile", "profile_hash", "profile_fingerprint"):
            with self.subTest(key=key):
                r = copy.deepcopy(self.receipt)
                r[key] = "wrong" if key != "profile_fingerprint" else ""
                self.assertFalse(self.aggregate(r)["passed"])

    def test_missing_skipped_cancelled_worker_and_audits_fail(self):
        for key in ("job_conclusion", "registration_audit", "input_audit"):
            for state in (None, "skipped", "cancelled", "failed"):
                r = copy.deepcopy(self.receipt)
                r[key] = state
                self.assertFalse(self.aggregate(r)["passed"])

    def test_expected_observed_names_exact_and_not_duplicate(self):
        for names in ([], ["other"], ["app.unit", "app.unit"]):
            r = copy.deepcopy(self.receipt)
            r["checks"]["app.unit"]["observed_executions"] = names
            self.assertFalse(self.aggregate(r)["passed"])

    def test_missing_checks_fail(self):
        self.receipt["checks"].pop("app.live")
        self.assertFalse(self.aggregate(self.receipt)["passed"])

    def test_partial_plan_and_dirty_cannot_satisfy_complete_gate(self):
        for kwargs in ({"profiles": ["linux"]}, {"source_identity": {"kind": "dirty", "sha": "a" * 40, "snapshot_digest": "content"}}):
            selected = plan(self.p, **kwargs)
            result = s.aggregate(self.p, selected, [receipt(self.p, selected)])
            self.assertFalse(result["passed"])
            self.assertTrue(s.aggregate(self.p, selected, [receipt(self.p, selected)], complete=False)["passed"])

    def test_ci_requires_independent_actual_worker_conclusions(self):
        selected = plan(self.p, context="ci")
        r = receipt(self.p, selected)
        self.assertFalse(s.aggregate(self.p, selected, [r])["passed"])
        for conclusions in ({}, {"linux": "skipped"}, {"linux": "cancelled"}, {"linux": "failure"}):
            self.assertFalse(s.aggregate(self.p, selected, [r], job_conclusions=conclusions)["passed"])
        self.assertTrue(s.aggregate(self.p, selected, [r], job_conclusions={"linux": "success"}, selection_conclusion="success")["passed"])

    def test_selection_failure_blocks_even_empty_plan(self):
        selected = plan(self.p, paths=("docs/guide.md",))
        self.assertFalse(s.aggregate(self.p, selected, [], selection_conclusion="failure")["passed"])
        self.assertTrue(s.aggregate(self.p, selected, [])["passed"])

    def test_advisory_failure_visible_not_clean(self):
        self.p["checks"]["app.live"]["requirement"] = "advisory"
        self.plan = plan(self.p)
        self.receipt = receipt(self.p, self.plan)
        self.receipt["checks"]["app.live"]["outcome"] = "advisory_failure"
        result = self.aggregate(self.receipt)
        self.assertTrue(result["passed"])
        self.assertEqual(result["non_clean_results"][0]["outcome"], "advisory_failure")

    def test_retry_remains_visible(self):
        self.receipt["checks"]["app.unit"]["initial_failure"] = True
        result = self.aggregate(self.receipt)
        self.assertTrue(result["passed"])
        self.assertTrue(result["non_clean_results"][0]["initial_failure"])

    def test_sanitizer_report_cannot_hide_behind_pass_or_retry(self):
        self.receipt["checks"]["app.unit"].update(initial_failure=True, sanitizer_reports=["leak"])
        self.assertFalse(self.aggregate(self.receipt)["passed"])

    def test_named_incompatibility_only_exact_case_and_exit(self):
        self.p["checks"]["app.live"]["expected_outcome"] = {"case": "mdk09_incompatibility", "exit_code": 77}
        self.plan = plan(self.p)
        self.receipt = receipt(self.p, self.plan)
        value = self.receipt["checks"]["app.live"]
        self.assertFalse(self.aggregate(self.receipt)["passed"])
        value.update(outcome="expected_incompatibility", exception_case="mdk09_incompatibility", exit_code=77)
        self.assertTrue(self.aggregate(self.receipt)["passed"])
        value["exit_code"] = 0
        self.assertFalse(self.aggregate(self.receipt)["passed"])

    def test_plan_tampering_not_accepted(self):
        self.plan["selected_checks"] = []
        with self.assertRaises(s.ContractError):
            self.aggregate(self.receipt)


class QuarantineTests(GitCase):
    def test_exact_tested_tree_quarantine_and_no_sanitizer_report(self):
        self.write("scripts/grotto-ci-quarantine.tsv", "linux\tapp.unit\tnostrc-test\n")
        self.commit()
        self.write("app/source.c", "changed\n")
        self.commit()
        selected = self.select()
        r = receipt(self.p, selected)
        r["checks"]["app.unit"].update(outcome="quarantined_failure", failed_executions=["app.unit"], sanitizer_reports=[], quarantine_beads=["nostrc-test"])
        self.assertTrue(s.aggregate(self.p, selected, [r], repo=self.repo)["passed"])
        r["checks"]["app.unit"]["sanitizer_reports"] = ["leak"]
        self.assertFalse(s.aggregate(self.p, selected, [r], repo=self.repo)["passed"])
