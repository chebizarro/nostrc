import copy
import json
from pathlib import Path
import unittest
from selector_support import ROOT, s, policy, plan, change


class PolicyTests(unittest.TestCase):
    def test_consumer_change_does_not_select_prerequisite_tests(self):
        self.assertEqual(plan()["selected_checks"], ["app.live", "app.unit"])

    def test_shared_change_selects_downstream_and_expensive(self):
        result = plan(paths=("core/header.h",))
        self.assertEqual(result["selected_checks"], ["app.live", "app.unit", "core.unit"])
        self.assertEqual(result["witnesses"]["app"], ["core", "app"])

    def test_fixture_only_does_not_select_production_consumers(self):
        self.assertEqual(plan(paths=("fixture/vector.json",))["selected_checks"], ["fixture.unit"])

    def test_unknown_and_boundary_broaden(self):
        for path in ("unknown.c", "coreopsis/file.c", "docs/run.py"):
            with self.subTest(path=path):
                result = plan(paths=(path,))
                self.assertEqual(result["effective_mode"], "broad")
                self.assertEqual(len(result["selected_checks"]), 5)
                self.assertTrue(result["broadening_reasons"])

    def test_gitlink_prefix_without_slash(self):
        self.assertEqual(s.classify(policy(), "core")["nodes"], ["core"])

    def test_docs_and_no_changes_are_explicit_empty(self):
        self.assertEqual(plan(paths=("docs/guide.md",))["lanes"], [])
        self.assertEqual(plan(paths=())["lanes"], [])

    def test_executable_prose_cannot_use_docs_allowlist(self):
        p = policy()
        result = s.make_plan(p, [dict(change("docs/guide.md"), executable=True, old_executable=True)], head="a" * 40)
        self.assertEqual(result["effective_mode"], "broad")

    def test_longest_rule_replaces_parent_equal_rules_union(self):
        p = policy()
        p["path_rules"] += [{"exact": "app/vector", "nodes": [n], "effect": "impact", "reason": "specific"} for n in ("fixture", "other")]
        self.assertEqual(s.classify(p, "app/vector")["nodes"], ["fixture", "other"])

    def test_contradictory_rules_invalid(self):
        p = policy()
        p["path_rules"].append({"exact": "app", "nodes": ["other"], "effect": "global", "reason": "contradiction"})
        with self.assertRaises(s.ContractError):
            s.validate_inventory(p)

    def test_cycles_require_explicit_conservative_group(self):
        p = policy()
        p["nodes"]["core"]["depends_on"] = ["app"]
        with self.assertRaises(s.ContractError):
            s.validate_inventory(p)
        for n in ("core", "app"):
            p["nodes"][n].update(conservative=True, conservative_group="shared")
        self.assertEqual(plan(p, paths=("core/source.c",))["witnesses"]["app"], ["core", "app"])

    def test_malformed_inventory_fatal_not_fail_broad(self):
        for mutate in (lambda p: p.update(schema_version=2), lambda p: p["nodes"]["core"].update(depends_on=["missing"]), lambda p: p["checks"]["core.unit"].update(profiles=["missing"]), lambda p: p["checks"]["core.unit"].update(ctest_tests={"linux": []}), lambda p: p["path_rules"][0].update(prefix="../core")):
            p = policy()
            mutate(p)
            with self.assertRaises(s.ContractError):
                s.validate_inventory(p)

    def test_primary_executions_cannot_be_claimed_twice(self):
        p = policy()
        p["checks"]["duplicate"] = copy.deepcopy(p["checks"]["core.unit"])
        with self.assertRaises(s.ContractError):
            s.validate_inventory(p)

    def test_model_gui_same_executable_are_distinct_checks(self):
        p = policy()
        p["checks"]["app.gui"] = copy.deepcopy(p["checks"]["app.unit"])
        p["checks"]["app.gui"]["ctest_tests"] = {"linux": ["app.gui"]}
        self.assertIn("app.gui", plan(p)["selected_checks"])
        self.assertIn("app.unit", plan(p)["selected_checks"])

    def test_only_named_expected_incompatibility(self):
        p = policy()
        p["checks"]["app.live"]["expected_outcome"] = {"case": "generic_skip", "exit_code": 77}
        with self.assertRaises(s.ContractError):
            s.validate_inventory(p)

    def test_profile_projection_recorded_and_hashed(self):
        p = policy()
        p["profiles"]["macos"] = dict(p["profiles"]["linux"], platform="macos")
        p["checks"]["app.unit"]["profiles"].append("macos")
        p["checks"]["app.unit"]["ctest_tests"]["macos"] = ["app.unit"]
        result = plan(p, profiles=["linux"])
        self.assertEqual(result["partial_profiles"], ["linux"])
        self.assertEqual(result["expected_lane_ids"], ["linux"])
        self.assertEqual(result["lanes"][0]["profile_hash"], s.digest(p["profiles"]["linux"]))

    def test_base_and_head_policy_union(self):
        p, old = policy(), policy()
        old["path_rules"][1]["nodes"] = ["core"]
        result = s.make_plan(p, [change("app/source.c", "D")], head="a" * 40, base_policies=[old])
        self.assertIn("core.unit", result["selected_checks"])

    def test_determinism_and_dirty_digest_identity(self):
        a = plan(paths=("core/a.c", "app/b.c"))
        b = plan(paths=("app/b.c", "core/a.c"))
        self.assertEqual(s.canonical(a), s.canonical(b))
        d1 = plan(source_identity={"kind": "dirty", "sha": "a" * 40, "snapshot_digest": "one"})
        d2 = plan(source_identity={"kind": "dirty", "sha": "a" * 40, "snapshot_digest": "two"})
        self.assertNotEqual(d1["plan_id"], d2["plan_id"])

    def test_checked_inventory_golden_cases(self):
        p = s.load_inventory(ROOT / "tests/selection/inventory.json")
        cases = s.read_json(ROOT / "tests/selection/golden/selection.json")
        for case in cases:
            with self.subTest(case=case["name"]):
                result = plan(p, paths=case["paths"])
                self.assertEqual(result["effective_mode"], case["mode"])
                if "checks" in case:
                    expected_checks = case["checks"]
                    if case["name"] == "grotto_workflow_only":
                        from grotto_nip49_projection import grotto_nip49_current_workflow_checks
                        expected_checks = grotto_nip49_current_workflow_checks(
                            expected_checks, p["diagnostic_nonmember_subsets"]["grotto.linux"])
                    from supported_profile_support import supported_golden_checks
                    expected_checks = supported_golden_checks(case['name'], expected_checks, p)
                    self.assertEqual(result["selected_checks"], expected_checks)
                for prefix in case.get("required_prefixes", []):
                    self.assertTrue(any(x.startswith(prefix) for x in result["selected_checks"]))
                for prefix in case.get("excluded_prefixes", []):
                    self.assertFalse(any(x.startswith(prefix) for x in result["selected_checks"]))

    def test_inventory_covers_all_tracked_paths_or_explicit_broadening(self):
        p = s.load_inventory(ROOT / "tests/selection/inventory.json")
        import os
        paths = s.git(ROOT, "ls-files", "-z").stdout.split(b"\0")
        self.assertEqual([os.fsdecode(path) for path in paths if path and s.classify(p, os.fsdecode(path))["unclassified"]], [])
        self.assertFalse(p["enforcement_ready"])
        self.assertTrue(p["coverage_gaps"])

    def test_duplicate_json_keys_rejected(self):
        with self.assertRaises(s.ContractError):
            json.loads('{"checks":{},"checks":{}}', object_pairs_hook=s._unique_object)


if __name__ == "__main__":
    unittest.main()
