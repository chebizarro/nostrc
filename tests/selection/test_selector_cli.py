import importlib.util
import json
import subprocess
import sys
import unittest
from selector_support import GitCase, ROOT, s, plan, policy

spec = importlib.util.spec_from_file_location("selection_cli", ROOT / "scripts/test-plan.py")
cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cli)


class CLITests(GitCase):
    def invoke(self, command, *args):
        return subprocess.run([sys.executable, str(ROOT / "scripts/test-plan.py"), command, "--repo", str(self.repo), *args], capture_output=True, text=True)

    def test_plan_committed_inventory_ignores_dirty_malformed_policy(self):
        self.write("tests/selection/inventory.json", "this dirty policy is not JSON")
        result = self.invoke("plan", "--head", self.base, "--base", self.base, "--context", "local")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["lanes"], [])

    def test_exit_codes_for_malformed_and_validation_failure(self):
        result = self.invoke("plan", "--head", "missing", "--context", "local")
        self.assertEqual(result.returncode, 2)
        self.write("unclassified/source.c", "new\n"); self.commit()
        result = self.invoke("validate")
        self.assertEqual(result.returncode, 1)
        self.assertIn("unclassified/source.c", json.loads(result.stdout)["unclassified_paths"])

    def test_atomic_output_and_explanation(self):
        self.write("app/source.c", "changed\n"); head = self.commit()
        out = self.repo / "plan.json"
        result = self.invoke("plan", "--head", head, "--base", self.base, "--context", "ci", "--output", str(out))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(s.read_json(out)["selected_checks"], ["app.live", "app.unit"])
        result = self.invoke("explain", "--head", head, "--base", self.base, "--context", "local")
        self.assertIn("affected -> affected", result.stdout)

    def test_missing_backend_never_succeeds(self):
        # In this isolated task tree no sibling module is installed. Once merged,
        # the real module instead rejects the nonexistent realization (exit 1).
        result = self.invoke("check-inputs", "--build-dir", str(self.repo / "absent"), "--profile", "linux")
        self.assertIn(result.returncode, (1, 2))
        self.assertNotEqual(result.returncode, 0)

    def test_aggregate_outputs_identity_for_release_chain(self):
        from selector_support import receipt
        selected = plan(self.p, context="ci")
        s.write_json(self.repo / "plan.json", selected)
        s.write_json(self.repo / "receipt.json", receipt(self.p, selected))
        s.write_json(self.repo / "jobs.json", {"linux": "success"})
        result = self.invoke("aggregate", "--plan", str(self.repo / "plan.json"), "--receipt", str(self.repo / "receipt.json"), "--job-results", str(self.repo / "jobs.json"), "--selection-conclusion", "success")
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        self.assertEqual(value["source_identity"], selected["source_identity"])
        self.assertTrue(value["complete"])

    def test_receipt_normalization_rejects_backend_contradiction(self):
        selected = plan(self.p)
        resolved = {"profile_id": "linux", "source_identity": selected["source_identity"], "inventory_hash": selected["inventory_hash"], "profile_fingerprint": "fp", "resolved_hash": "resolved", "tests": ["app.live", "app.unit"]}
        raw = dict(resolved, schema_version=1, plan_hash=s.digest(selected), outcome="passed", input_audit={"passed": True, "after_build": True}, observed_executions={"app.live": "unexpected_skip", "app.unit": "passed"}, checks={"app.live": {"outcome": "passed", "executions": ["app.live"]}, "app.unit": {"outcome": "passed", "executions": ["app.unit"]}})
        with self.assertRaises(s.ContractError):
            cli.normalize_receipt(self.p, selected, resolved, raw)
