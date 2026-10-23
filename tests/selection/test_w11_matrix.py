"""Finite matrix driver fixtures; never product benchmark or PR acceptance evidence."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("w11_matrix", Path(__file__).resolve().parents[2] / "scripts/w11-evidence.py")
w11 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(w11)


class MatrixTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        for args in (("init", "-q"), ("config", "user.email", "fixture@example.invalid"),
                     ("config", "user.name", "Fixture")):
            self.git(*args)
        (self.repo / "recipe.py").write_text("raise SystemExit(7)\n")
        self.git("add", ".")
        self.git("commit", "-qm", "not a real PR")
        exe = str(Path(sys.executable).resolve())
        self.contract = {"kind": "w11_physical_contract_v1", "candidate_sha": self.git("rev-parse", "HEAD").strip(),
                         "profile": "fixture-only", "pair_id": "fixture", "cache_evidence": {"fixture_only": True},
                         "environment": {"PATH": os.environ["PATH"], "HOME": str(self.root), "TMPDIR": str(self.root)},
                         "input_bindings": {"recipe.py": w11.digest((self.repo / "recipe.py").read_bytes())},
                         "tool_probes": [{"argv": [exe, "--version"], "executable_sha256": w11.digest(Path(exe).read_bytes())}],
                         "phases": [{"phase": "workload", "argv": [exe, "recipe.py"],
                                     "executable_sha256": w11.digest(Path(exe).read_bytes()), "timeout_seconds": 5}]}
        self.manifest = {"kind": "w11_physical_matrix_v1", "repository": "fixture/repo", "entries": []}
        for condition in w11.CONDITIONS:
            for cache in ("clean", "incremental"):
                for sample in range(1, 6):
                    for route in ("legacy", "selected"):
                        c = dict(self.contract, condition=condition, cache_state=cache, sample=sample, route=route)
                        path = self.root / f"contract-{len(self.manifest['entries'])}.json"
                        w11.write(path, c)
                        self.manifest["entries"].append({"repo": str(self.repo), "pull": 1, "contract": str(path),
                                                         "contract_sha256": w11.digest(path.read_bytes())})

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], env=w11.environment()).decode()

    def run_matrix(self, execute=False):
        path = self.root / "manifest.json"
        w11.write(path, self.manifest)
        out = self.root / "out"
        out.mkdir()
        return w11.matrix(SimpleNamespace(manifest=str(path), execute=execute), out), out

    def change(self, index, **fields):
        entry = self.manifest["entries"][index]
        path = Path(entry["contract"])
        c = json.loads(path.read_bytes())
        c.update(fields)
        w11.write(path, c)
        entry["contract_sha256"] = w11.digest(path.read_bytes())

    @staticmethod
    def fixture_sample(args, out):
        c = json.loads(Path(args.contract).read_bytes())
        result = {k: c[k] for k in ("condition", "cache_state", "sample", "route", "pair_id", "profile", "environment")}
        result.update(kind="w11_physical_sample_v1", source={"sha": c["candidate_sha"]}, runner={"fixture": True},
                      provenance={"fixture_only": True}, execution_passed=True, serial_wall_seconds=3,
                      tool_probes=[{"argv": ["fixture", "--version"], "stdout_sha256": "fixture", "stderr_sha256": "fixture"}],
                      phases=[{"phase": "workload", "status": "passed", "process": {"exit_code": 0, "elapsed_seconds": 1}}],
                      complete=True)  # A forged child completeness field must not promote the cohort.
        w11.write(out / "result.json", result)
        return result

    def test_checked_in_outstanding_matrix_names_every_real_missing_slot(self):
        path = Path(__file__).resolve().parents[2] / "docs/reviews/dependency-aware-testing-w11-p3-matrix-2026-10-24.json"
        manifest = json.loads(path.read_bytes())
        self.assertEqual("w11_physical_matrix_v1", manifest["kind"])
        self.assertEqual([], manifest["entries"])
        cells = manifest["outstanding_recipe_cells"]
        self.assertEqual({(condition, cache) for condition in w11.CONDITIONS for cache in ("clean", "incremental")},
                         {(c["condition"], c["cache_state"]) for c in cells})
        self.assertEqual(14, len(cells))
        for cell in cells:
            self.assertFalse(cell["recipe_approved"])
            self.assertTrue(cell["native_feasibility"])
            self.assertEqual([1, 2, 3, 4, 5], [s["sample"] for s in cell["slots"]])
            for slot in cell["slots"]:
                for key in ("legacy_contract", "selected_contract", "legacy_sample", "selected_sample"):
                    self.assertIsNone(slot[key])
                self.assertTrue(all(v is None for v in slot["metrics"].values()))
        out = self.root / "outstanding"
        out.mkdir()
        with patch.object(w11, "execute_locked") as execute:
            result = w11.matrix(SimpleNamespace(manifest=str(path), execute=True), out)
        execute.assert_not_called()
        self.assertFalse(result["schedule_ready"])

    def test_full_schedule_only_never_executes_or_claims_measurements(self):
        with patch.object(w11, "execute_locked") as execute:
            result, out = self.run_matrix()
        execute.assert_not_called()
        self.assertTrue(result["schedule_ready"])
        self.assertEqual(14, len(result["cells"]))
        self.assertTrue(all(c["planned_pairs"] == 5 for c in result["cells"]))
        self.assertNotIn("measurement_report", result)
        self.assertEqual(140, len(list((out / "contracts").iterdir())))
        self.assertFalse(result["complete_gate_eligible"])

    def test_incomplete_schedule_refuses_before_any_workload(self):
        self.manifest["entries"].pop()
        with patch.object(w11, "execute_locked") as execute:
            result, _ = self.run_matrix(execute=True)
        execute.assert_not_called()
        self.assertFalse(result["schedule_ready"])
        self.assertEqual(4, result["cells"][-1]["planned_pairs"])
        self.assertTrue(all(e["status"] == "not_run" for e in result["entries"]))

    def test_malformed_manifest_type_retains_cli_failure(self):
        path = self.root / "bad-manifest.json"
        w11.write(path, [])
        out = self.root / "bad-out"
        proc = subprocess.run([sys.executable, w11.__file__, "matrix", "--manifest", str(path),
                               "--output", str(out)], capture_output=True)
        self.assertEqual(1, proc.returncode)
        self.assertEqual([], json.loads((out / "manifest.json").read_bytes()))
        self.assertIn("entries required", json.loads((out / "failure.json").read_bytes())["error"])

    def test_retained_contract_drift_stops_before_workload(self):
        original = w11.git
        def git(*args):
            (self.root / "out/contracts/0000.json").write_text("{}")
            return original(*args)
        with patch.object(w11, "git", side_effect=git), patch.object(w11, "execute_locked") as execute:
            result, _ = self.run_matrix(execute=True)
        execute.assert_not_called()
        self.assertFalse(result["execution_passed"])
        self.assertIn("contract changed", result["cohort_error"])
        self.assertEqual("failed", result["entries"][0]["status"])

    def test_contract_digest_drift_refuses(self):
        Path(self.manifest["entries"][0]["contract"]).write_text("{}")
        with patch.object(w11, "execute_locked") as execute, self.assertRaisesRegex(ValueError, "digest"):
            self.run_matrix(execute=True)
        execute.assert_not_called()

    def test_renaming_pair_cannot_turn_retry_into_sample(self):
        entry = copy.deepcopy(self.manifest["entries"][0])
        self.manifest["entries"].append(entry)
        with self.assertRaisesRegex(ValueError, "duplicate matrix sample/retry"):
            self.run_matrix()

    def test_pair_cannot_use_different_source_or_pull(self):
        self.change(1, candidate_sha="1" * 40)
        with self.assertRaisesRegex(ValueError, "source/PR"):
            self.run_matrix()

    def test_cohort_holds_common_repository_lock(self):
        import fcntl
        with (self.repo / ".git/w11-measurement.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with patch.object(w11, "execute_locked") as execute:
                result, out = self.run_matrix(execute=True)
        execute.assert_not_called()
        self.assertFalse(result["execution_passed"])
        self.assertIn("overlapping", result["cohort_error"])
        self.assertTrue((out / "failure.json").exists())

    def test_real_failed_workload_stops_cohort_and_retains_exact_process(self):
        with patch.object(w11, "provenance", return_value={"fixture_only": True}):
            result, out = self.run_matrix(execute=True)
        self.assertFalse(result["execution_passed"])
        self.assertEqual("failed", result["entries"][0]["status"])
        self.assertTrue(all(e["status"] == "not_run" for e in result["entries"][1:]))
        self.assertEqual(7, json.loads((out / "sample-0000/workload.process.json").read_bytes())["exit_code"])
        self.assertFalse(result["measurement_report"]["measurement_matrix_satisfied"])
        self.assertEqual(1, len(result["measurement_report"]["unpaired_samples"]))

    def test_preflight_failure_and_cancel_preserve_contract_and_not_run(self):
        with patch.object(w11, "execute_locked", side_effect=KeyboardInterrupt("fixture cancel")):
            result, out = self.run_matrix(execute=True)
        self.assertFalse(result["execution_passed"])
        failure = json.loads((out / "sample-0000/failure.json").read_bytes())
        self.assertEqual("fixture cancel", failure["error"])
        report = result["measurement_report"]
        self.assertEqual(1, len(report["preflight_failures"]))
        self.assertIsNotNone(report["preflight_failures"][0]["contract_sha256"])
        self.assertTrue(all(e["status"] == "not_run" for e in result["entries"][1:]))

    def test_mocked_matrix_statistics_are_fixture_only_never_acceptance(self):
        with patch.object(w11, "execute_locked", side_effect=self.fixture_sample) as execute:
            result, _ = self.run_matrix(execute=True)
        self.assertEqual(140, execute.call_count)
        self.assertTrue(result["execution_passed"])
        self.assertFalse(result["complete"])
        report = result["measurement_report"]
        self.assertTrue(report["measurement_matrix_satisfied"])
        self.assertEqual(0, report["qualifying_comparisons"])
        for cell in report["cells"]:
            self.assertEqual(5, cell["legacy"]["samples"])
            self.assertEqual([3, 3], cell["legacy"]["range"])
            self.assertEqual(0, cell["legacy"]["iqr"])
            self.assertFalse(cell["full_phase_breakdown"])

    def test_cancellation_during_discovery_is_finalized(self):
        with patch.object(w11, "git", side_effect=KeyboardInterrupt("discovery cancelled")):
            result, out = self.run_matrix(execute=True)
        self.assertTrue(result["cancelled"])
        self.assertFalse(result["execution_passed"])
        self.assertTrue(all(e["status"] == "not_run" for e in result["entries"]))
        self.assertTrue((out / "failure.json").exists())

    def test_cancellation_during_child_preparation_is_finalized(self):
        original = Path.mkdir
        def mkdir(path, *args, **kwargs):
            if path.name == "sample-0000":
                raise KeyboardInterrupt("prepare cancelled")
            return original(path, *args, **kwargs)
        with patch.object(Path, "mkdir", mkdir), patch.object(w11, "execute_locked") as execute:
            result, _ = self.run_matrix(execute=True)
        execute.assert_not_called()
        self.assertTrue(result["cancelled"])
        self.assertEqual("failed", result["entries"][0]["status"])
        self.assertTrue(all(e["status"] == "not_run" for e in result["entries"][1:]))

    def test_cancellation_during_aggregation_retains_completed_children(self):
        with patch.object(w11, "execute_locked", side_effect=self.fixture_sample), patch.object(
                w11, "measurement_report", side_effect=KeyboardInterrupt("aggregation cancelled")):
            result, out = self.run_matrix(execute=True)
        self.assertTrue(result["cancelled"])
        self.assertFalse(result["execution_passed"])
        self.assertEqual(140, len(result["retained_sample_paths"]))
        self.assertTrue(all(e["status"] == "passed" for e in result["entries"]))
        self.assertTrue((out / "sample-0139/result.json").exists())

    def test_unsatisfied_nested_report_propagates_to_cli_exit(self):
        path = self.root / "manifest.json"
        w11.write(path, self.manifest)
        out = self.root / "cli-unsatisfied"
        args = [w11.__file__, "matrix", "--manifest", str(path), "--execute", "--output", str(out)]
        with patch.object(sys, "argv", args), patch.object(w11, "execute_locked", side_effect=self.fixture_sample), patch.object(
                w11, "measurement_report", return_value={"measurement_matrix_satisfied": False}):
            code = w11.main()
        self.assertEqual(1, code)
        result = json.loads((out / "result.json").read_bytes())
        self.assertTrue(result["execution_passed"])
        self.assertFalse(result["measurement_matrix_satisfied"])

    def test_reporting_retry_with_new_pair_id_still_refuses(self):
        args = SimpleNamespace(contract=self.manifest["entries"][0]["contract"])
        first = self.root / "first"
        first.mkdir()
        self.fixture_sample(args, first)
        second = self.root / "second"
        second.mkdir()
        sample = json.loads((first / "result.json").read_bytes())
        sample["pair_id"] = "renamed retry"
        w11.write(second / "result.json", sample)
        with self.assertRaisesRegex(ValueError, "duplicate sample/retry"):
            w11.measurement_report([first / "result.json", second / "result.json"])

    def test_cli_empty_matrix_and_partial_measurements_return_nonzero_with_receipt(self):
        for operation in ("matrix", "measurements"):
            out = self.root / ("cli-" + operation)
            if operation == "matrix":
                path = self.root / "empty.json"
                w11.write(path, dict(self.manifest, entries=[]))
                args = ["matrix", "--manifest", str(path)]
            else:
                child = self.root / "failure.json"
                w11.write(child, {"error": "fixture preflight", "complete": False})
                args = ["measurements", "--sample", str(child)]
            proc = subprocess.run([sys.executable, w11.__file__, *args, "--output", str(out)], capture_output=True)
            self.assertEqual(1, proc.returncode, proc.stderr)
            self.assertTrue((out / "result.json").exists())
            self.assertFalse(json.loads((out / "result.json").read_bytes())["complete"])


if __name__ == "__main__":
    unittest.main()
