"""Physical-Git replay and refusal tests for the W11 diagnostic evidence tool."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/w11-evidence.py"
spec = importlib.util.spec_from_file_location("w11_evidence", SCRIPT)
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)


class W11EvidenceTests(unittest.TestCase):
    def job(self, identity=1, **updates):
        return dict({"id": identity, "name": "nested / execute", "status": "completed", "conclusion": "success",
                     "started_at": "2026-10-23T00:00:00Z", "completed_at": "2026-10-23T00:01:00Z"}, **updates)

    def test_failed_pending_skipped_are_not_acceptance(self):
        jobs = [self.job(), self.job(2, conclusion="failure"),
                self.job(3, status="queued", conclusion=None, started_at=None, completed_at=None),
                self.job(4, conclusion="skipped", started_at=None, completed_at=None)]
        result = evidence.summarize_jobs({"total_count": 4, "jobs": jobs})
        self.assertEqual(result["observed_job_minutes"], 2)
        self.assertEqual(result["pending_job_ids"], [3])
        self.assertEqual(result["skipped_job_ids"], [4])
        self.assertFalse(result["complete"])
        self.assertFalse(result["complete_gate_eligible"])
        self.assertEqual(result["qualifying_comparisons"], 0)

    def test_missing_page_duplicate_and_bad_timestamps_refuse(self):
        for payload in ({"total_count": 2, "jobs": [self.job()]},
                        {"total_count": 2, "jobs": [self.job(), self.job()]},
                        {"total_count": 1, "jobs": [self.job(completed_at=None)]},
                        {"total_count": 1, "jobs": [self.job(completed_at="2026-10-22T00:00:00Z")]},
                        {"total_count": 1, "jobs": [self.job(started_at="2026-10-23T00:00:00")]}):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                evidence.summarize_jobs(payload)

    def test_nonzero_process_keeps_stdout_stderr_and_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefix = Path(tmp) / "failed"
            with self.assertRaises(ValueError):
                evidence.command([sys.executable, "-c", "import sys; print('out'); print('err', file=sys.stderr); sys.exit(7)"],
                                 None, prefix)
            self.assertEqual(prefix.with_suffix(".stdout").read_text(), "out\n")
            self.assertEqual(prefix.with_suffix(".stderr").read_text(), "err\n")
            self.assertEqual(json.loads(prefix.with_suffix(".process.json").read_text())["exit_code"], 7)

    def test_timeout_kills_child_holding_pipes_before_delayed_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ready, marker = root / "ready", root / "escaped-write"
            child = (f"from pathlib import Path; import time; Path({str(ready)!r}).write_text('ready'); "
                     f"time.sleep(10); Path({str(marker)!r}).write_text('survived')")
            parent = (f"import subprocess,sys,time; subprocess.Popen([sys.executable, '-c', {child!r}]); "
                      "time.sleep(60)")
            prefix = root / "timeout"
            with self.assertRaises(evidence.CommandTimeout):
                evidence.command([sys.executable, "-c", parent], None, prefix, timeout=2)
            self.assertTrue(ready.exists(), "child must really start and hold inherited pipes")
            self.assertFalse(marker.exists())
            process = json.loads(prefix.with_suffix(".process.json").read_text())
            self.assertEqual(process["exit_code"], 124)
            self.assertEqual(process["termination"], "owned_process_group_sigkill")
            self.assertTrue(process["timed_out"])

    def test_five_sample_statistics_and_refusals(self):
        result = evidence.statistics([5, 1, 2, 4, 3])
        self.assertEqual((result["median"], result["min"], result["max"], result["iqr"]), (3, 1, 5, 2))
        for values in ([1, 2, 3, 4], [1, 2, 3, 4, float("nan")], [1, 2, 3, 4, -1], [1, 2, 3, 4, True]):
            with self.subTest(values=values), self.assertRaises(ValueError):
                evidence.statistics(values)

    def test_inventory_register_distinguishes_profile_claims_not_global_ownership(self):
        names = [f"GoFixture{i}" for i in range(33)]
        policy = {"diagnostic_nonmember_subsets": {p: dict(registrations=list(names), profile_hash="p",
                  universe_hash="u", candidate_contract_hash="c", toolchain_contract_hash="t")
                  for p in ("groundhog.linux", "grotto.linux")},
                  "profiles": {p: {} for p in ("groundhog.linux", "grotto.linux", "core.linux")},
                  "checks": {"go": {"kind": "ctest", "profiles": ["core.linux"],
                                     "ctest_tests": {"core.linux": names}}},
                  "enforcement_ready": False, "coverage_gaps": [{"id": "unresolved"}]}
        result = evidence.inventory_register(policy)
        self.assertEqual(result["ownership_by_profile"]["core.linux"]["claimed"], 33)
        self.assertEqual(result["ownership_by_profile"]["groundhog.linux"]["claimed"], 0)
        self.assertFalse(result["complete_gate_eligible"])
        self.assertEqual(result["coverage_gaps"], policy["coverage_gaps"])
        policy["diagnostic_nonmember_subsets"]["grotto.linux"]["registrations"].pop()
        with self.assertRaises(ValueError):
            evidence.inventory_register(policy)

    def test_real_git_replay_uses_original_policy_and_refuses_reused_output(self):
        with tempfile.TemporaryDirectory(prefix="w11-evidence-test-") as tmp:
            root = Path(tmp)
            repo = root / "repo"
            repo.mkdir()

            def git(*args):
                return subprocess.check_output(["git", "-C", str(repo), *args], stderr=subprocess.PIPE).decode().strip()

            git("init", "-b", "main")
            git("config", "user.email", "fixture@example.invalid")
            git("config", "user.name", "W11 fixture")
            git("config", "core.hooksPath", "/dev/null")
            (repo / "scripts").mkdir()
            (repo / "tests/selection").mkdir(parents=True)
            (repo / "tests/selection/inventory.json").write_text('{"original": true}\n')
            # The fixture's real executable observes its checked-out policy and
            # Git HEAD. No production coverage or legacy runner is simulated.
            (repo / "scripts/test-plan.py").write_text('''import json, subprocess
from pathlib import Path
sha = subprocess.check_output(["git", "rev-parse", "HEAD"]).decode().strip()
assert json.loads(Path("tests/selection/inventory.json").read_text())["original"]
print(json.dumps({"tested_sha": sha, "source_identity": {"kind": "committed", "sha": sha},
"plan_id": "fixture-plan", "inventory_hash": "fixture-policy", "effective_mode": "affected",
"selected_checks": ["fixture-check"], "expected_lane_ids": ["fixture-profile"], "broadening_reasons": []}))
''')
            git("add", ".")
            git("commit", "-m", "base")
            base = git("rev-parse", "HEAD")
            git("checkout", "-b", "pr")
            (repo / "change.txt").write_text("real branch change\n")
            git("add", ".")
            git("commit", "-m", "change")
            head = git("rev-parse", "HEAD")
            git("checkout", "main")
            git("merge", "--no-ff", "pr", "-m", "real merge")
            merged = git("rev-parse", "HEAD")
            prs = [{"number": 1, "title": "fixture", "html_url": "https://example.invalid/pull/1",
                    "merged_at": "2026-10-23T00:00:00Z", "base": {"sha": base}, "head": {"sha": head},
                    "merge_commit_sha": merged}]
            source = root / "prs.json"
            source.write_text(json.dumps(prs))
            output = root / "output"
            argv = [sys.executable, str(SCRIPT), "replay", "--repo", str(repo), "--prs", str(source),
                    "--output", str(output)]
            run = subprocess.run(argv, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            result = json.loads((output / "result.json").read_text())
            self.assertEqual(result["candidates"][0]["replay_status"], "logical_only")
            self.assertEqual(result["candidates"][0]["selected_checks"], ["fixture-check"])
            self.assertEqual(result["qualifying_comparisons"], 0)
            self.assertFalse(result["complete"])
            self.assertEqual(git("status", "--porcelain"), "")
            first = (output / "result.json").read_bytes()
            self.assertNotEqual(subprocess.run(argv, capture_output=True).returncode, 0)
            self.assertEqual((output / "result.json").read_bytes(), first)
            # A valid caller-owned external index contains a staged change.
            # Inherited routing must not redirect census/clone/checkout into it.
            external_index = root / "caller-index"
            routing = dict(evidence.environment(), GIT_INDEX_FILE=str(external_index))
            subprocess.run(["git", "-C", str(repo), "read-tree", "HEAD"], env=routing, check=True)
            (repo / "change.txt").write_text("caller staged sentinel\n")
            subprocess.run(["git", "-C", str(repo), "add", "change.txt"], env=routing, check=True)
            git("checkout", "--", "change.txt")
            original_index = (repo / ".git/index").read_bytes()
            sentinel = external_index.read_bytes()
            routed = subprocess.run(argv[:-1] + [str(root / "routed-output")], capture_output=True,
                                    env=dict(routing, GIT_DIR=str(repo / ".git"), GIT_WORK_TREE=str(repo)))
            self.assertEqual(routed.returncode, 0, routed.stderr)
            self.assertEqual(external_index.read_bytes(), sentinel)
            self.assertEqual((repo / ".git/index").read_bytes(), original_index)
            self.assertEqual(git("status", "--porcelain"), "")
            # Whitespace-only instability must fail too; semantic JSON equality
            # is not the contract's byte-stable selection requirement.
            git("checkout", "-b", "unstable")
            planner = repo / "scripts/test-plan.py"
            planner.write_text('''from pathlib import Path
p = Path("counter")
p.write_text(p.read_text() + " " if p.exists() else " ")
print(p.read_text(), end="")
''' + planner.read_text())
            git("add", "scripts/test-plan.py")
            git("commit", "-m", "unstable output")
            unstable = git("rev-parse", "HEAD")
            git("checkout", "main")
            git("merge", "--no-ff", "unstable", "-m", "unstable merge")
            source.write_text(json.dumps([dict(prs[0], base={"sha": merged}, head={"sha": unstable},
                                              merge_commit_sha=git("rev-parse", "HEAD"))]))
            bad_output = root / "unstable-output"
            bad = subprocess.run(argv[:-1] + [str(bad_output)], capture_output=True)
            self.assertNotEqual(bad.returncode, 0)
            failed = json.loads((bad_output / "result.json").read_text())
            self.assertIn("not byte-stable", failed["candidates"][0]["error"])
            self.assertTrue((bad_output / "pr-1/plan-2.stdout").exists())
            for altered in ([prs[0], prs[0]], [dict(prs[0], base={"sha": head})],
                            [dict(prs[0], merged_at=None)], [dict(prs[0], merge_commit_sha="HEAD")]):
                with self.subTest(altered=altered), self.assertRaises(ValueError):
                    evidence.candidates(altered, repo)


if __name__ == "__main__":
    unittest.main()
