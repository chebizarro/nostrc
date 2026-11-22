#!/usr/bin/env python3
"""Fixture + real temporary Git checks for the read-only reconciliation CLI."""
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/beads-reconcile.py"
FIXTURE = ROOT / "tests/fixtures/beads-reconcile.json"
spec = importlib.util.spec_from_file_location("beads_reconcile", SCRIPT)
reconcile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reconcile)
NOW = datetime(2026, 11, 21, tzinfo=timezone.utc)


class Reconciliation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix=".beads-reconcile-test-", dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "git"
        self.repo.mkdir()
        self.git("init", "-b", "master")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "user.name", "Fixture")
        self.git("config", "core.hooksPath", "/dev/null")
        self.git("config", "commit.gpgsign", "false")
        (self.repo / "file").write_text("fixture\n")
        self.git("add", "file")
        self.git("commit", "-m", "initial")
        initial = self.git("rev-parse", "HEAD")
        blob = self.git("rev-parse", "HEAD:file")
        self.git("checkout", "-b", "marmot-track-b")
        self.git("commit", "--allow-empty", "-m", "landed work")
        landed = self.git("rev-parse", "HEAD")
        self.git("checkout", "master")
        self.git("merge", "--no-ff", "marmot-track-b", "-m", "Merge branch 'marmot-track-b'")
        self.git("branch", "-d", "marmot-track-b")
        self.git("branch", "landed-tip")
        self.git("checkout", "-b", "other")
        self.git("commit", "--allow-empty", "-m", "not landed")
        other = self.git("rev-parse", "HEAD")
        self.git("checkout", "master")
        dangling = self.git("commit-tree", "HEAD^{tree}", "-m", "unreferenced")
        raw = FIXTURE.read_text()
        for name, sha in dict(MASTER=initial, LANDED=landed, OTHER=other, DANGLING=dangling, BLOB=blob).items():
            raw = raw.replace(f"@{name}@", sha)
        self.issues = json.loads(raw)
        self.tracker = self.root / "tracker"
        self.tracker.mkdir()
        self.payload = self.root / "fixture.json"
        self.payload.write_text(raw)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.bd = self.bin / "bd"
        self.bd.write_text(f'''#!/bin/sh
test "$PWD" = "{self.tracker}" || exit 9
test "$*" = '--readonly --sandbox list --all -n 0 --json' || exit 10
/bin/cat "{self.payload}"
''')
        self.bd.chmod(0o755)

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.repo, check=True, text=True,
                              capture_output=True).stdout.strip()

    def report(self, issues=None, **kwargs):
        return reconcile.reconcile(self.issues if issues is None else issues,
                                   reconcile.Git(self.repo), now=NOW, **kwargs)

    def cli(self, *args, env=None):
        return subprocess.run([sys.executable, str(SCRIPT), "--repo", str(self.repo),
                               "--tracker-repo", str(self.tracker), *args], cwd=ROOT,
                              env=env or {**os.environ, "PATH": f"{self.bin}:{os.environ['PATH']}"},
                              text=True, capture_output=True)

    def test_claim_threshold_and_missing_timestamp(self):
        report = self.report()
        self.assertEqual([f["id"] for f in report["findings"] if f["check"] == "stale_claim"], ["nostrc-old"])
        self.assertEqual(self.report(idle_days=60)["counts"]["stale_claim"], 0)
        report = self.report([{"id": "bad", "status": "in_progress"}])
        self.assertIn("cannot assess claim age", report["warnings"][0])

    def test_parent_edges_dotted_ids_and_non_parent_blocks(self):
        parents = {f["id"]: f for f in self.report()["findings"] if f["check"] == "parent_complete"}
        self.assertEqual(set(parents), {"nostrc-empty", "nostrc-parent"})
        self.assertEqual(parents["nostrc-parent"]["children"], ["nostrc-child", "nostrc-parent.1"])
        self.assertEqual(parents["nostrc-empty"]["reason"], "no_children")

    def test_missing_child_is_not_all_closed(self):
        report = self.report([{"id": "p", "status": "open", "issue_type": "epic",
                               "dependencies": [{"type": "parent-child", "issue_id": "missing", "depends_on_id": "p"}]}])
        self.assertEqual(report["counts"]["parent_complete"], 0)
        self.assertIn("missing children", report["warnings"][0])

    def test_deleted_merged_branch_and_existing_tip(self):
        branches = {f["branch"]: f for f in self.report()["findings"] if f["check"] == "landed_branch"}
        self.assertEqual(set(branches), {"marmot-track-b", "landed-tip"})
        self.assertEqual(branches["marmot-track-b"]["evidence"], "historical_merge")
        self.assertEqual(branches["landed-tip"]["evidence"], "branch_tip_ancestor")

    def test_commit_reachability_and_noise(self):
        report = self.report()
        citations = [f for f in report["findings"] if f["check"] == "commit_citation"]
        self.assertEqual(report["commit_reachability_counts"], {"master": 3, "other_refs": 1, "unreachable": 2})
        missing = next(f for f in citations if f["citation"] == "b171bda1")
        self.assertFalse(missing["verified_commit"])
        self.assertIsNone(missing["commit"])
        self.assertEqual(sum(f["verified_commit"] and f["reachability"] == "unreachable" for f in citations), 1)
        self.assertGreaterEqual(report["ignored_unresolved_hex_tokens"], 2)
        self.assertNotIn("nostrc-closed", {f["id"] for f in citations})

    def test_duplicates_only_recorded_and_active(self):
        report = self.report()
        duplicates = [f for f in report["findings"] if f["check"] == "duplicate_keeper_closed"]
        self.assertEqual([(f["id"], f["keeper"]) for f in duplicates], [("nostrc-dup", "nostrc-keeper")])
        self.assertTrue(self.report([])["duplicate_check"].startswith("skipped:"))

    def test_later_explicit_citation_and_branch_ending_hyphen(self):
        self.git("branch", "landed-")
        report = self.report([{"id": "test", "status": "open",
                               "notes": "deadbeef noise; commit deadbeef. Work on branch `landed-`."}])
        self.assertEqual(report["counts"]["commit_citation"], 1)
        self.assertEqual(report["counts"]["landed_branch"], 1)

    def test_real_cli_json_human_and_read_only(self):
        before = (self.git("show-ref"), self.git("status", "--porcelain"),
                  (self.repo / ".git/index").read_bytes(), sorted(str(p) for p in self.root.rglob("*")))
        result = self.cli("--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["tracker_repo"], str(self.tracker))
        self.assertEqual(report["git_repo"], str(self.repo))
        self.assertEqual(report["counts"]["landed_branch"], 2)
        human = self.cli()
        self.assertEqual(human.returncode, 0, human.stderr)
        self.assertIn("Tracker (read-only bd cwd)", human.stdout)
        self.assertIn("unverified explicit citations", human.stdout)
        citation_rows = [line for line in human.stdout.splitlines() if line.startswith("[commit_citation]")]
        self.assertEqual(len(citation_rows), 3)
        self.assertFalse(any('"reachability": "master"' in line for line in citation_rows))
        self.assertTrue(any('"reachability": "other_refs"' in line for line in citation_rows))
        self.assertTrue(any('"reachability": "unreachable"' in line for line in citation_rows))
        counts = json.loads(next(line.removeprefix("Counts: ") for line in human.stdout.splitlines()
                                 if line.startswith("Counts: ")))
        self.assertEqual(counts["commit_reachability"], report["commit_reachability_counts"])
        full = self.cli("--all-citations")
        self.assertEqual(full.returncode, 0, full.stderr)
        full_rows = [line for line in full.stdout.splitlines() if line.startswith("[commit_citation]")]
        self.assertEqual(len(full_rows), report["counts"]["commit_citation"])
        self.assertTrue(any('"reachability": "master"' in line for line in full_rows))
        self.assertEqual(json.loads(self.cli("--json", "--all-citations").stdout), report)
        after = (self.git("show-ref"), self.git("status", "--porcelain"),
                 (self.repo / ".git/index").read_bytes(), sorted(str(p) for p in self.root.rglob("*")))
        self.assertEqual(before, after)

    def test_landed_work_hint_requires_all_citations_on_master(self):
        master = self.git("rev-parse", "master")
        other = self.git("rev-parse", "other")
        issues = [
            {"id": "landed", "status": "open", "notes": f"commit {master}",
             "description": f"Same commit {master}; initial {self.git('rev-parse', 'master~1')}"},
            {"id": "mixed", "status": "open", "notes": f"commit {master}", "description": f"commit {other}"},
            {"id": "missing", "status": "open", "notes": f"commit {master}; commit b171bda1"},
            {"id": "none", "status": "open"},
            {"id": "closed", "status": "closed", "notes": f"commit {master}"},
        ]
        report = self.report(issues)
        hints = [f for f in report["findings"] if f["check"] == "landed_work"]
        self.assertEqual([f["id"] for f in hints], ["landed"])
        self.assertEqual(hints[0]["commit_count"], 2)
        self.assertEqual(report["counts"]["landed_work"], 1)
        self.payload.write_text(json.dumps(issues))
        human = self.cli()
        self.assertEqual(human.returncode, 0, human.stderr)
        hint_rows = [line for line in human.stdout.splitlines() if line.startswith("[landed_work]")]
        self.assertEqual(len(hint_rows), 1)
        self.assertIn("cited work has landed — check whether the bead can close", hint_rows[0])

    def test_bd_failure_and_bad_json_are_visible(self):
        self.bd.write_text("#!/bin/sh\necho unavailable >&2\nexit 7\n")
        result = self.cli("--json")
        self.assertEqual(result.returncode, 2)
        self.assertIn("unavailable", result.stderr)
        self.assertEqual(result.stdout, "")
        self.bd.write_text("#!/bin/sh\necho invalid-json\n")
        self.assertEqual(self.cli().returncode, 2)
        self.bd.write_text("#!/bin/sh\necho '{}'\n")
        self.assertIn("array of issues", self.cli().stderr)

    def test_missing_executables_and_master_fail(self):
        empty_env = {**os.environ, "PATH": str(self.bin)}
        result = self.cli(env=empty_env)
        self.assertEqual(result.returncode, 2)
        self.assertIn("git", result.stderr)
        self.bd.unlink()
        (self.bin / "git").symlink_to(shutil.which("git"))
        result = self.cli(env=empty_env)
        self.assertEqual(result.returncode, 2)
        self.assertIn("bd", result.stderr)
        self.git("branch", "-m", "master", "not-master")
        result = self.cli()
        self.assertEqual(result.returncode, 2)
        self.assertIn("refs/heads/master", result.stderr)

    def test_invalid_threshold_rejected(self):
        for value in ("-1", "nan", "inf"):
            result = self.cli("--idle-days", value)
            self.assertEqual(result.returncode, 2)
            self.assertIn("finite nonnegative", result.stderr)

    def test_inventory_selects_only_reconciliation_regression(self):
        import test_selection as selection
        policy = selection.load_inventory(ROOT / "tests/selection/inventory.json")
        check = policy["checks"]["beads.reconcile.regressions"]
        for runtime_input in check["runtime_inputs"]:
            path = runtime_input.removeprefix("{source}/")
            for context in ("local", "ci"):
                plan = selection.make_plan(policy, [{"status": "M", "old_path": path, "new_path": path}],
                                           head="a" * 40, base="b" * 40, context=context)
                self.assertEqual(plan["effective_mode"], "affected")
                self.assertEqual(plan["selected_checks"], ["beads.reconcile.regressions"])


if __name__ == "__main__":
    unittest.main()
