"""Physical completeness is not W11/operator rollout or publication authority.

Synthetic receipts exercise the contract; they are not hosted coverage evidence.
"""
import copy
import unittest

from selector_support import ROOT, receipt, s
from test_workflow_contracts import ci, ready_policy


ADMIN = {"id": "shadow_cutover", "reason": "W11 evidence and operator no-gap cutover remain pending",
         "tracking": "nostrc-0cfsg.5", "classification": "administrative_rollout_prerequisite",
         "authority": "reviewed_shadow_cutover_v1"}
NEW_FIELDS = ("physical_coverage_gaps", "rollout_prerequisites", "rollout_readiness")
TECHNICAL_IDS = ("workflow_profile_union", "root_asan_execution_contract",
                 "groundhog_sanitizer_execution_contract", "libgo_macos_asan_stress_exclusion",
                 "supported_profile_union", "prepared_peer_linux_contract", "homed_unit_profile_contract",
                 "worker_setup_and_grotto_execution", "supported_profile_execution_remainder")


class RolloutPrerequisiteTests(unittest.TestCase):
    def setUp(self):
        self.p = ready_policy()
        self.p.update(enforcement_ready=False, coverage_gaps=[copy.deepcopy(ADMIN)])
        self.plan = self.make_plan()
        self.receipt = receipt(self.p, self.plan)

    def make_plan(self):
        return s.make_plan(self.p, [{"status": "M", "old_path": "app/source.c", "new_path": "app/source.c"}],
                           head="a" * 40, context="ci")

    def aggregate(self, value=None, complete=True, plan=None):
        selected = self.plan if plan is None else plan
        return s.aggregate(self.p, selected, [self.receipt if value is None else value], complete=complete,
                           selection_conclusion="success", job_conclusions={"core.linux": "success"})

    def release(self, policy=None):
        p = self.p if policy is None else policy
        return dict(source_identity=self.plan["source_identity"], tested_sha=self.plan["tested_sha"],
                    inventory_hash=s.digest(p), tag="fixture-v1.0.0", publish=True,
                    required_package_jobs=[], publishers=["main"], **s.coverage_state(p))

    def validation(self, release):
        return dict(source_identity=release["source_identity"], tested_sha=release["tested_sha"],
                    inventory_hash=release["inventory_hash"], tag=release["tag"], mode="broad", passed=True,
                    complete=True, complete_gate_eligible=True, partial_profiles=[], **s.coverage_state(release))

    def test_complete_physical_shadow_preserves_unready_rollout(self):
        result = self.aggregate()
        self.assertTrue(result["passed"], result["errors"])
        self.assertTrue(result["complete"])
        self.assertTrue(result["complete_gate_eligible"])
        self.assertEqual(result["coverage_gaps"], [ADMIN])
        self.assertEqual(result["physical_coverage_gaps"], [])
        self.assertEqual(result["rollout_prerequisites"], [ADMIN])
        self.assertFalse(result["enforcement_ready"])
        self.assertEqual(result["rollout_readiness"], {"schema_version": 1, "enforcement_ready": False,
                         "pending_prerequisite_ids": ["shadow_cutover"], "ready": False})
        needs = {job: {"result": "skipped"} for job in ci.LANES.values()}
        needs.update(select={"result": "success"}, core={"result": "success"}, broad={"result": "skipped"})
        hosted = ci.aggregate(ROOT, self.p, self.plan, needs, [self.receipt])
        self.assertTrue(hosted["passed"], hosted["errors"])
        self.assertEqual(hosted["rollout_readiness"], result["rollout_readiness"])
        self.assertFalse(self.aggregate(complete=False)["complete_gate_eligible"])

    def test_every_current_technical_row_still_blocks_physical_complete(self):
        for ident in TECHNICAL_IDS:
            with self.subTest(gap=ident):
                self.p["coverage_gaps"] = [copy.deepcopy(ADMIN), {"id": ident, "reason": "actual missing coverage"}]
                self.plan = self.make_plan()
                self.receipt = receipt(self.p, self.plan)
                result = self.aggregate()
                self.assertFalse(result["passed"])
                self.assertFalse(result["complete_gate_eligible"])
                self.assertEqual([g["id"] for g in result["physical_coverage_gaps"]], [ident])

    def test_untyped_legacy_shadow_row_and_old_plan_never_promote(self):
        self.p["coverage_gaps"] = [{k: v for k, v in ADMIN.items() if k not in {"classification", "authority"}}]
        self.plan = self.make_plan()
        self.receipt = receipt(self.p, self.plan)
        self.assertFalse(self.aggregate()["passed"])
        for key in NEW_FIELDS:
            self.plan.pop(key)
        self.plan.pop("plan_id")
        self.plan["plan_id"] = s.digest(self.plan)
        self.assertFalse(self.aggregate()["complete_gate_eligible"])

    def test_only_exact_reviewed_admin_declaration_is_admitted(self):
        mutations = [dict(ADMIN, id="workflow_profile_union"), dict(ADMIN, authority="ignore_all"),
                     dict(ADMIN, classification="ignore"), dict(ADMIN, tracking="nostrc-0cfsg.9"),
                     dict(ADMIN, ignore=True), {k: v for k, v in ADMIN.items() if k != "authority"},
                     dict(ADMIN, reason="")]
        for item in mutations:
            with self.subTest(item=item), self.assertRaises(s.ContractError):
                s.coverage_state(dict(self.p, coverage_gaps=[item]))
        with self.assertRaises(s.ContractError):
            s.coverage_state(dict(self.p, coverage_gaps=[ADMIN, ADMIN]))
        with self.assertRaises(s.ContractError):
            s.coverage_state(dict(self.p, enforcement_ready=1))
        # A legacy ignore flag is not administrative classification.
        self.assertTrue(s.coverage_state(dict(self.p, coverage_gaps=[{"id": "runtime", "ignore": True}]))
                        ["physical_coverage_gaps"])

    def test_rehashed_forged_or_missing_plan_state_refuses(self):
        for key, value in (("coverage_gaps", []), ("physical_coverage_gaps", [ADMIN]),
                           ("rollout_prerequisites", []), ("enforcement_ready", True),
                           ("rollout_readiness", {"ready": True})):
            with self.subTest(key=key), self.assertRaises(s.ContractError):
                changed = copy.deepcopy(self.plan)
                changed[key] = value
                changed.pop("plan_id")
                changed["plan_id"] = s.digest(changed)
                self.aggregate(plan=changed)
        for key in NEW_FIELDS:
            changed = copy.deepcopy(self.plan)
            changed.pop(key)
            changed.pop("plan_id")
            changed["plan_id"] = s.digest(changed)
            with self.subTest(missing=key), self.assertRaises(s.ContractError):
                self.aggregate(plan=changed)

    def test_old_receipts_and_forged_or_missing_backend_readiness_refuse(self):
        old = copy.deepcopy(self.receipt)
        for key in s.COVERAGE_STATE_FIELDS:
            old.pop(key)
        self.assertFalse(self.aggregate(old)["complete_gate_eligible"])
        normalized = copy.deepcopy(self.receipt)
        normalized["backend_receipt"] = copy.deepcopy(self.receipt)
        for key in s.COVERAGE_STATE_FIELDS:
            if key != "coverage_gaps":
                normalized.pop(key)
        self.assertTrue(self.aggregate(normalized)["passed"])
        for key in s.COVERAGE_STATE_FIELDS:
            changed = copy.deepcopy(normalized)
            changed["backend_receipt"].pop(key)
            self.assertFalse(self.aggregate(changed)["complete_gate_eligible"], key)
        changed = copy.deepcopy(normalized)
        changed["backend_receipt"]["rollout_readiness"]["ready"] = True
        self.assertFalse(self.aggregate(changed)["complete_gate_eligible"])
        normalized["coverage_gaps"] = []
        self.assertFalse(self.aggregate(normalized)["complete_gate_eligible"])

    def test_nested_partial_and_diagnostic33_never_promote(self):
        for nested in ({"complete": False}, {"complete_gate_eligible": False},
                       {"complete": 0}, {"complete_gate_eligible": 0},
                       {"partial_profiles": ["groundhog.linux"]},
                       {"authority": s.subset.AUTHORITY, "complete": False}):
            changed = copy.deepcopy(self.receipt)
            changed["retained_child"] = nested
            self.assertFalse(self.aggregate(changed)["complete_gate_eligible"], nested)

    def test_physical_success_cannot_authorize_any_enforced_publisher(self):
        for publisher in ("main", "groundhog", "windows"):
            release = dict(self.release(), publishers=[publisher])
            needs = {j: {"result": "skipped"} for j in ci.PUBLISHER_JOBS[publisher]}
            needs["validation"] = {"result": "success"}
            valid = self.validation(release)
            self.assertFalse(s.aggregate_release(release, valid, {})["publish"])
            with self.subTest(publisher=publisher), self.assertRaises(s.ContractError):
                ci.publish_gate(release, publisher, valid, needs, "enforce")
            forged = self.validation(release)
            forged.update(s.coverage_state({"enforcement_ready": True, "coverage_gaps": []}))
            with self.assertRaises(s.ContractError):
                ci.publish_gate(release, publisher, forged, needs, "enforce")
            for mode in ("disabled", "shadow"):
                staged = ci.publish_gate(release, publisher, None, needs, mode)
                self.assertFalse(staged["validation_required"])
                self.assertEqual(staged["validation_status"], "staged_" + mode)

    def test_ready_release_requires_all_bound_new_state_and_rejects_old_nested_data(self):
        ready = dict(self.p, enforcement_ready=True, coverage_gaps=[])
        release = self.release(ready)
        valid = self.validation(release)
        self.assertTrue(s.aggregate_release(release, valid, {})["publish"])
        for key in (*s.COVERAGE_STATE_FIELDS, "complete_gate_eligible"):
            changed = copy.deepcopy(valid)
            changed.pop(key)
            self.assertFalse(s.aggregate_release(release, changed, {})["publish"], key)
        for nested in ({"complete": False}, {"complete": 0}, {"complete_gate_eligible": 0},
                       {"authority": s.subset.AUTHORITY},
                       {"rollout_prerequisites": [ADMIN]}, {"rollout_readiness": {"ready": False}}):
            changed = dict(valid, retained_child=nested)
            self.assertFalse(s.aggregate_release(release, changed, {})["publish"], nested)

    def test_invalid_declared_full_ownership_refuses_instead_of_omission_fallback(self):
        p = s.load_inventory(ROOT / "tests/selection/inventory.json")
        p["checks"]["supported.libgo"][s.subset.FULL_FIELD] = {}
        with self.assertRaises(s.ContractError):
            s.validate_inventory(p)

    def test_full_evidence_on_undeclared_profile_refuses(self):
        changed = copy.deepcopy(self.receipt)
        changed[s.subset.FULL_FIELD] = {"profile": "groundhog.linux"}
        self.assertFalse(self.aggregate(changed)["complete_gate_eligible"])

    def test_full_ownership_validator_refusal_is_not_bypassed(self):
        from unittest.mock import patch
        # Routing-only refusal injection, not full-ownership or physical proof.
        with patch.object(s.subset, "full_profiles", return_value={"core.linux"}), \
             patch.object(s.subset, "validate_full_receipt", side_effect=ValueError("full audit refused")) as validator:
            result = self.aggregate()
        validator.assert_called_once()
        self.assertFalse(result["complete_gate_eligible"])
        self.assertTrue(any("full audit refused" in error for error in result["errors"]))

    def test_actual_production_policy_cannot_borrow_forged_ready_validation(self):
        p = s.load_inventory(ROOT / "tests/selection/inventory.json")
        self.assertIs(p["enforcement_ready"], False)
        release = s.release_plan(ROOT, p, head="HEAD", component="gnostr", dry_run=True, run_id="unit-refusal")
        forged = self.validation(release)
        forged.update(s.coverage_state({"enforcement_ready": True, "coverage_gaps": []}))
        self.assertFalse(s.aggregate_release(release, forged,
                         {j: "success" for j in release["required_package_jobs"]})["passed"])


if __name__ == "__main__":
    unittest.main()
