import json
import os
from selector_support import GitCase, ROOT, s


class RangeTests(GitCase):
    def test_rename_unions_both_paths(self):
        self.git("mv", "app/source.c", "core/moved.c")
        self.commit()
        result = self.select()
        self.assertEqual(result["changes"][0]["status"], "R100")
        self.assertIn("core.unit", result["selected_checks"])
        self.assertIn("app.unit", result["selected_checks"])

    def test_delete_uses_base_inventory(self):
        (self.repo / "app/source.c").unlink()
        self.commit()
        self.assertEqual(self.select()["selected_checks"], ["app.live", "app.unit"])

    def test_force_push_compares_trees_not_merge_base(self):
        self.write("app/source.c", "branch one\n")
        branch_one = self.commit()
        self.git("checkout", "-q", self.base)
        self.write("core/source.c", "branch two\n")
        branch_two = self.commit()
        result = self.select(head=branch_two, base=branch_one)
        self.assertEqual({c["new_path"] for c in result["changes"]}, {"app/source.c", "core/source.c"})

    def test_new_branch_merge_base_and_missing_default(self):
        self.git("branch", "default", self.base)
        self.write("app/source.c", "new\n")
        self.commit()
        event = {"kind": "push", "base_sha": "0" * 64, "default_branch": "default"}
        self.assertEqual(self.select(event=event)["effective_mode"], "affected")
        event.pop("default_branch")
        self.assertEqual(self.select(event=event)["effective_mode"], "broad")

    def test_deleted_ref_no_execution_generic_zero_length(self):
        result = self.select(head="0" * 64, event={"kind": "push"})
        self.assertTrue(result["deleted_ref"])
        self.assertEqual(result["lanes"], [])

    def test_missing_history_is_broad_not_empty(self):
        result = self.select(base="f" * 40)
        self.assertEqual(result["effective_mode"], "broad")
        self.assertTrue(result["broadening_reasons"])
        self.assertTrue(result["selected_checks"])

    def test_newline_whitespace_unicode_filenames(self):
        name = "app/space tab\tnewline\n雪.c"
        self.write(name, "content\n")
        self.commit()
        self.assertEqual(self.select()["changes"][0]["new_path"], name)
        self.assertEqual(self.select()["selected_checks"], ["app.live", "app.unit"])

    def test_gitlink_classifies_pointer_not_contents(self):
        self.git("update-index", "--add", "--cacheinfo", f"160000,{self.base},core/vendor")
        self.git("commit", "-qm", "gitlink")
        result = self.select()
        self.assertTrue(result["changes"][0]["gitlink"])
        self.assertIn("app.unit", result["selected_checks"])

    def test_symlink_not_followed(self):
        os.symlink("/outside/not/read", self.repo / "app/link")
        self.commit()
        self.assertEqual(self.select()["selected_checks"], ["app.live", "app.unit"])

    def test_explicit_head_ignores_dirty_worktree(self):
        self.write("core/source.c", "dirty\n")
        self.write("app/untracked.c", "untracked\n")
        self.assertEqual(self.select(head=self.base)["changes"], [])
        self.assertTrue(self.git("status", "--porcelain"))

    def test_pr_union_records_pr_and_tested_merge_identity(self):
        default_branch = self.git("branch", "--show-current")
        self.git("checkout", "-qb", "feature")
        self.write("app/source.c", "feature\n")
        feature = self.commit()
        self.git("checkout", "-q", default_branch)
        self.write("core/source.c", "base advances\n")
        advanced = self.commit()
        self.git("merge", "-qm", "test merge", "feature")
        merged = self.git("rev-parse", "HEAD")
        event = {"kind": "pull_request", "base_sha": advanced, "pr_head_sha": feature, "tested_sha": merged}
        result = self.select(event=event, head=merged, context="ci")
        self.assertEqual(result["tested_sha"], merged)
        self.assertEqual(result["event"]["pr_head_sha"], feature)
        self.assertEqual(result["selected_checks"], ["app.live", "app.unit"])

    def test_event_tested_sha_mismatch_fatal(self):
        self.write("app/source.c", "new\n")
        self.commit()
        with self.assertRaises(s.ContractError):
            self.select(event={"kind": "merge_group", "tested_sha": self.base})

    def test_merge_group_uses_event_base(self):
        self.write("app/source.c", "merge group\n")
        self.commit()
        result = self.select(base=None, event={"kind": "merge_group", "base_sha": self.base})
        self.assertEqual(result["selected_checks"], ["app.live", "app.unit"])

    def test_multi_ref_range_union_deduplicates(self):
        self.write("app/source.c", "first\n")
        first = self.commit()
        self.write("core/source.c", "second\n")
        head = self.commit()
        events = [{"kind": "push", "base_sha": self.base, "tested_sha": head}, {"kind": "push", "base_sha": first, "tested_sha": head}]
        result = self.select(event=events)
        self.assertEqual(len(result["changes"]), 2)
        self.assertEqual(result["selected_checks"], ["app.live", "app.unit", "core.unit"])

    def test_inventory_change_broadens_even_if_new_rules_narrow(self):
        self.write("tests/selection/inventory.json", json.dumps(self.p, indent=2))
        self.commit()
        self.assertEqual(self.select()["effective_mode"], "broad")

    def test_bad_base_inventory_broadens(self):
        self.write("tests/selection/inventory.json", "{}"); before = self.commit()
        self.write("tests/selection/inventory.json", json.dumps(self.p)); self.commit()
        self.write("app/source.c", "changed\n"); head = self.commit()
        self.assertEqual(self.select(head=head, base=before)["effective_mode"], "broad")

    def test_real_shallow_clone_missing_ancestry_broadens(self):
        import subprocess
        import tempfile
        from pathlib import Path
        self.write("app/source.c", "later\n")
        head = self.commit()
        with tempfile.TemporaryDirectory() as temp:
            clone = Path(temp) / "shallow"
            subprocess.run(["git", "clone", "--quiet", "--depth=1", self.repo.as_uri(), str(clone)], check=True)
            self.assertEqual(s.git(clone, "rev-parse", "--is-shallow-repository").stdout.strip(), b"true")
            result = s.git_plan(clone, self.p, head=head, base=self.base)
            self.assertEqual(result["effective_mode"], "broad")
            self.assertTrue(result["broadening_reasons"])

    def test_custom_candidate_inventory_cannot_narrow_its_own_change(self):
        # Candidate rules cannot opt the policy file out of broad validation.
        self.p["path_rules"] = [r for r in self.p["path_rules"] if r.get("prefix") != "tests/selection"]
        self.p["path_rules"].append({"prefix": "tests/selection", "nodes": ["fixture"], "effect": "impact", "reason": "candidate narrowing"})
        self.write("tests/selection/inventory.json", json.dumps(self.p))
        self.commit()
        self.assertEqual(self.select()["effective_mode"], "broad")

    def test_diff_failure_fatal_not_empty(self):
        with self.assertRaises(s.ContractError):
            s.diff_records(self.repo, self.base, "does-not-exist")

    def test_deleted_executable_prose_broadens(self):
        self.write("docs/run.md", "#!/bin/sh\n")
        (self.repo / "docs/run.md").chmod(0o755)
        before = self.commit()
        (self.repo / "docs/run.md").unlink(); self.commit()
        self.assertEqual(self.select(base=before)["effective_mode"], "broad")
