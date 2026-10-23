"""Small local process/API fixtures. None of these are W11 acceptance samples."""
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
from unittest.mock import patch, MagicMock

SPEC = importlib.util.spec_from_file_location("w11_measurement", Path(__file__).resolve().parents[2] / "scripts/w11-evidence.py")
w11 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(w11)


class MeasurementTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.git("init", "-q")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "user.name", "Fixture")
        (self.repo / "recipe.py").write_text("print('physical fixture only')\n")
        self.git("add", ".")
        self.git("commit", "-qm", "fixture source, not a real PR")
        self.sha = self.git("rev-parse", "HEAD").strip()
        self.exe = str(Path(sys.executable).resolve())
        self.exe_hash = w11.digest(Path(self.exe).read_bytes())
        self.contract = {
            "kind": "w11_physical_contract_v1", "candidate_sha": self.sha,
            "condition": "grotto", "cache_state": "clean", "sample": 1,
            "pair_id": "fixture-only", "route": "legacy", "profile": "fixture-only",
            "cache_evidence": {"fixture": True},
            "environment": {"PATH": os.environ["PATH"], "HOME": str(self.root), "TMPDIR": str(self.root)},
            "input_bindings": {"recipe.py": w11.digest((self.repo / "recipe.py").read_bytes())},
            "tool_probes": [{"argv": [self.exe, "--version"], "executable_sha256": self.exe_hash}],
            "phases": [{"phase": p, "argv": [self.exe, "recipe.py"],
                        "executable_sha256": self.exe_hash, "timeout_seconds": 5} for p in w11.PHASES]}

    def git(self, *argv):
        return subprocess.check_output(["git", "-C", str(self.repo), *argv], env=w11.environment()).decode()

    def execute(self, contract=None):
        out = self.root / ("out-" + str(len(list(self.root.glob("out-*")))))
        out.mkdir()
        cpath = self.root / "contract.json"
        w11.write(cpath, contract or self.contract)
        args = SimpleNamespace(repo=str(self.repo), contract=str(cpath), repository="fixture/repo", pull=1)
        with patch.object(w11, "provenance", return_value={"fixture_only": True}):
            result = w11.execute(args, out)
        return result, out

    def test_runs_four_phases_and_versions_without_inheriting_skip(self):
        with patch.dict(os.environ, {"NOSTRC_SKIP_PRE_PUSH": "1"}):
            result, out = self.execute()
        self.assertTrue(result["execution_passed"])
        self.assertFalse(result["complete"])
        self.assertEqual(0, result["qualifying_comparisons"])
        self.assertNotIn("NOSTRC_SKIP_PRE_PUSH", result["environment"])
        self.assertEqual(list(w11.PHASES), [p["phase"] for p in result["phases"]])
        self.assertIn("Python", (out / "tool-0.stdout").read_text())
        for phase in result["phases"]:
            self.assertEqual(0, phase["process"]["exit_code"])
            self.assertEqual(self.exe, phase["process"]["argv"][0])

    def test_intact_adapter_is_measured_without_fabricating_phase_timings(self):
        self.contract["phases"] = [dict(self.contract["phases"][0], phase="workload")]
        result, _ = self.execute()
        self.assertTrue(result["execution_passed"])
        self.assertFalse(result["phase_breakdown_available"])
        self.assertEqual(["workload"], [p["phase"] for p in result["phases"]])

    def test_repository_lock_refuses_overlapping_measurements(self):
        import fcntl
        with (self.repo / ".git/w11-measurement.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError, "overlapping"):
                self.execute()

    def test_failed_phase_retains_streams_and_remaining_not_run(self):
        self.contract["phases"][1]["argv"] = [self.exe, "-c", "import sys; print('failure'); sys.exit(7)"]
        result, out = self.execute()
        self.assertFalse(result["execution_passed"])
        self.assertEqual(["passed", "failed", "not_run", "not_run"], [p["status"] for p in result["phases"]])
        self.assertEqual(7, result["phases"][1]["process"]["exit_code"])
        self.assertEqual("failure\n", (out / "configure.stdout").read_text())

    def test_dirty_source_wrong_executable_and_binding_refuse(self):
        for kind in ("dirty", "executable", "binding"):
            with self.subTest(kind=kind):
                c = copy.deepcopy(self.contract)
                if kind == "dirty":
                    (self.repo / "recipe.py").write_text("changed\n")
                elif kind == "executable":
                    c["phases"][0]["executable_sha256"] = "0" * 64
                else:
                    c["input_bindings"]["recipe.py"] = "0" * 64
                with self.assertRaises(ValueError):
                    self.execute(c)
                self.git("checkout", "--", "recipe.py")

    def test_index_assume_unchanged_cannot_hide_source_drift(self):
        self.git("update-index", "--assume-unchanged", "recipe.py")
        (self.repo / "recipe.py").write_text("hidden change")
        with self.assertRaisesRegex(ValueError, "physical source bytes"):
            w11.source_state(self.repo, self.sha)

    def test_modify_restore_does_not_claim_source_immutability(self):
        self.contract["phases"] = [dict(self.contract["phases"][0], phase="workload", argv=[self.exe, "-c",
            "from pathlib import Path; p=Path('recipe.py'); b=p.read_bytes(); p.write_text('changed'); p.write_bytes(b)"])]
        result, _ = self.execute()
        self.assertTrue(result["execution_passed"])
        self.assertFalse(result["source_immutability_verified"])
        self.assertIn("within_phase_mutation_unverified", result["source_verification"])

    def test_source_mutation_during_phase_fails(self):
        self.contract["phases"][0]["argv"] = [self.exe, "-c", "open('recipe.py','w').write('changed')"]
        result, _ = self.execute()
        self.assertFalse(result["execution_passed"])
        self.assertEqual("failed", result["phases"][0]["status"])

    def test_cleanup_timeout_retains_later_cumulative_output(self):
        for first in (subprocess.TimeoutExpired(["fixture"], 1, output=b"early"), KeyboardInterrupt()):
            proc = MagicMock()
            proc.pid = 123
            proc.communicate.side_effect = [first, subprocess.TimeoutExpired(["fixture"], 5, output=b"early-later", stderr=b"error-later")]
            prefix = self.root / ("cancel" if isinstance(first, KeyboardInterrupt) else "timeout")
            with patch.object(w11.subprocess, "Popen", return_value=proc), patch.object(w11.os, "killpg"):
                with self.assertRaises(w11.CommandTimeout):
                    w11.command(["fixture"], self.root, prefix)
            self.assertEqual(b"early-later", prefix.with_suffix(".stdout").read_bytes())
            self.assertEqual(b"error-later", prefix.with_suffix(".stderr").read_bytes())

    def test_pr_patch_uses_merge_base_not_diverged_base_endpoint(self):
        common = self.sha
        (self.repo / "pr-only").write_text("PR change")
        self.git("add", ".")
        self.git("commit", "-qm", "PR side")
        head = self.git("rev-parse", "HEAD").strip()
        self.git("checkout", "-q", "--detach", common)
        (self.repo / "base-only").write_text("base advanced")
        self.git("add", ".")
        self.git("commit", "-qm", "base side")
        base = self.git("rev-parse", "HEAD").strip()
        self.git("checkout", "-q", "--detach", head)
        pr = {"number": 1, "base": {"sha": base, "repo": {"full_name": "fixture/repo"}},
              "head": {"sha": head}, "merge_commit_sha": None, "html_url": "https://example.invalid/fixture"}
        commit = {"sha": head, "tree": {"sha": self.git("rev-parse", "HEAD^{tree}").strip()}, "parents": [{"sha": common}]}
        with patch.object(w11, "api", side_effect=[pr, commit]):
            result = w11.provenance(SimpleNamespace(repository="fixture/repo", pull=1), self.repo, head, self.root)
        self.assertEqual(common, result["pr_merge_base"])
        diff = (self.root / "original-change.patch").read_text()
        self.assertIn("pr-only", diff)
        self.assertNotIn("base-only", diff)

    def test_provenance_refuses_transplanted_source(self):
        args = SimpleNamespace(repository="fixture/repo", pull=1)
        pr = {"number": 1, "base": {"repo": {"full_name": "fixture/repo"}},
              "head": {"sha": "1" * 40}, "merge_commit_sha": "2" * 40}
        with patch.object(w11, "api", return_value=pr):
            with self.assertRaisesRegex(ValueError, "not a transplanted"):
                w11.provenance(args, self.repo, self.sha, self.root)

    def test_pairs_fourteen_cells_five_samples_and_failed_pair_retention(self):
        paths = []
        for sample in range(1, 7):
            for route in ("legacy", "selected"):
                c = copy.deepcopy(self.contract)
                c.update(sample=sample, route=route)
                if sample == 6:
                    c["phases"][0]["argv"] = [self.exe, "-c", "raise SystemExit(4)"]
                _, out = self.execute(c)
                paths.append(str(out / "result.json"))
        report = w11.measurement_report(paths)
        self.assertEqual(14, len(report["cells"]))
        self.assertEqual(5, report["cells"][0]["successful_pairs"])
        self.assertEqual(1, len(report["cells"][0]["failed_pairs"]))
        self.assertEqual(5, report["cells"][0]["legacy"]["samples"])
        self.assertEqual(0, report["qualifying_comparisons"])
        self.assertFalse(report["complete_gate_eligible"])
        with self.assertRaisesRegex(ValueError, "duplicate sample/retry"):
            w11.measurement_report(paths + paths[:1])
        sample = json.loads(Path(paths[1]).read_text())
        sample["source"]["sha"] = "3" * 40
        w11.write(Path(paths[1]), sample)
        with self.assertRaisesRegex(ValueError, "identity"):
            w11.measurement_report(paths)

    def test_api_pages_reject_drift_duplicates_and_missing_page(self):
        for responses in ([{"total_count": 2, "jobs": [{"id": 1}]}, {"total_count": 3, "jobs": []}],
                          [{"total_count": 2, "jobs": [{"id": 1}, {"id": 1}]}],
                          [{"total_count": 2, "jobs": [{"id": 1}]}, {"total_count": 2, "jobs": []}]):
            with patch.object(w11, "api", side_effect=responses):
                with self.assertRaises(ValueError):
                    w11.api_pages("fixture", "jobs", self.root, "fixture")

    def test_cutover_patch_is_unapplied_and_preserves_unrelated_required_checks(self):
        workflows = self.repo / ".github/workflows"
        workflows.mkdir(parents=True)
        legacy = "name: fixture\non:\n  workflow_call:\n  push:\n    branches: [master]\n  pull_request:\n  schedule:\n    - cron: '17 3 * * *'\n  workflow_dispatch:\njobs: {}\n"
        for name in w11.LEGACY_TRIGGER_FILES:
            (workflows / name).write_text(legacy)
        (workflows / "dependency-tests.yml").write_text(
            "# NOT required yet. Keep legacy required checks, duplicate triggers and schedules through W11.\non:\n  workflow_dispatch:\njobs: {}\n")
        (workflows / "dependency_contract.py").write_text(
            '    selection.require(name in {"workflow_dispatch", "workflow_call"}, "unsupported Actions event")\n')
        (self.repo / "scripts").mkdir()
        (self.repo / "scripts/pre-push").write_text(
            '#!/usr/bin/env bash\nset -euo pipefail\n'
            'if [ "${NOSTRC_SELECTIVE_GATE:-0}" = 1 ]; then\n'
            '    echo "Pre-push: explicit diagnostic shared gate (does not replace mandatory rollout gates)"\n'
            '    bash "$repo_root/scripts/test-gate.sh" --repo "$repo_root" --pre-push --refs-file "$tmp/refs"\nfi\n')
        (self.repo / "AGENTS.md").write_text("# Agent Instructions\n")
        (self.repo / "docs").mkdir()
        (self.repo / "docs/TESTING.md").write_text("# Testing\n")
        self.git("add", ".")
        self.git("commit", "-qm", "cutover source fixture")
        sha = self.git("rev-parse", "HEAD").strip()
        protection = self.root / "protection.json"
        w11.write(protection, {"required_status_checks": {"strict": True, "checks": [
            {"context": "legacy", "app_id": 15368}, {"context": "unrelated-security", "app_id": 1}]}})
        out = self.root / "cutover"
        out.mkdir()
        args = SimpleNamespace(repo=str(self.repo), sha=sha, protection=str(protection), app_id=15368, obsolete_context=["legacy"])
        result = w11.prepare_cutover(args, out)
        self.assertFalse(result["applied"])
        self.assertEqual("", self.git("status", "--porcelain"))
        self.git("apply", "--check", str(out / "w12-forward.patch"))
        selected = json.loads((out / "required-selected.json").read_text())
        self.assertEqual(["unrelated-security", "Dependency gate"], [c["context"] for c in selected["checks"]])
        self.assertIn("+    tags: ['v*']", (out / "w12-forward.patch").read_text())
        self.assertNotIn('+  schedule:', (out / "w12-forward.patch").read_text())
        self.assertNotIn('+    - cron:', (out / "w12-forward.patch").read_text())
        w11.write(protection, {"status": "404", "message": "Branch not protected"})
        result = w11.prepare_cutover(args, out)
        self.assertEqual("blocked", result["protection"]["status"])

    def test_preflight_failure_retains_error_without_timing_credit(self):
        w11.write(self.root / "failure.json", {"error": "candidate drift", "complete": False})
        w11.write(self.root / "contract.json", self.contract)
        report = w11.measurement_report([str(self.root / "failure.json")])
        self.assertEqual("candidate drift", report["preflight_failures"][0]["error"])
        self.assertTrue(all(c["successful_pairs"] == 0 for c in report["cells"]))

    def test_job_log_from_foreign_attempt_refuses_before_fetch(self):
        run = {"id": 123, "run_attempt": 1}
        args = SimpleNamespace(repository="fixture/repo", run=123, attempt=1, artifact=[], job_log=[999], max_artifact_bytes=1024)
        with patch.object(w11, "api", return_value=run), patch.object(w11, "api_pages", return_value=[]):
            with self.assertRaisesRegex(ValueError, "this exact attempt"):
                w11.collect(args, self.root)

    def test_original_artifact_requires_api_digest_and_keeps_failed_zip(self):
        import io
        import zipfile
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr("receipt.json", '{"complete": false}')
        raw = buffer.getvalue()
        run = {"id": 123, "run_attempt": 1, "head_sha": "a" * 40, "status": "completed", "conclusion": "failure"}
        artifact = {"id": 4, "name": "fixture", "expired": False, "size_in_bytes": len(raw), "digest": "sha256:" + w11.digest(raw)}
        args = SimpleNamespace(repository="fixture/repo", run=123, attempt=1, artifact=[4], job_log=[], max_artifact_bytes=1024)
        with patch.object(w11, "api", return_value=run), patch.object(w11, "api_pages", side_effect=[[], [artifact]]), patch.object(w11, "command", return_value=raw):
            result = w11.collect(args, self.root)
        self.assertEqual([4], result["downloaded_artifact_ids"])
        self.assertEqual(raw, (self.root / "artifact-4.zip").read_bytes())
        artifact["digest"] = "sha256:" + "0" * 64
        with patch.object(w11, "api", return_value=run), patch.object(w11, "api_pages", side_effect=[[], [artifact]]), patch.object(w11, "command", return_value=raw):
            with self.assertRaisesRegex(ValueError, "digest/size mismatch"):
                w11.collect(args, self.root)
        self.assertEqual(raw, (self.root / "artifact-4.zip").read_bytes())

    def test_collector_failed_root_not_promoted_by_successful_child(self):
        run = {"id": 123, "run_attempt": 2, "head_sha": "a" * 40, "status": "completed", "conclusion": "failure"}
        job = {"id": 2, "run_id": 123, "run_attempt": 2, "name": "child", "status": "completed",
               "conclusion": "success", "started_at": "2026-10-23T00:00:00Z", "completed_at": "2026-10-23T00:01:00Z"}
        args = SimpleNamespace(repository="fixture/repo", run=123, attempt=2, artifact=[], job_log=[], max_artifact_bytes=1024)
        with patch.object(w11, "api", return_value=run), patch.object(w11, "api_pages", side_effect=[[job], []]):
            result = w11.collect(args, self.root)
        self.assertEqual("failure", result["conclusion"])
        self.assertEqual(0, result["qualifying_comparisons"])
        self.assertEqual(1, result["timing"]["observed_job_minutes"])


if __name__ == "__main__":
    unittest.main()
