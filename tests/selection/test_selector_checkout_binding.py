"""R1: real CLI reuse must execute only the checkout admitted by its plan."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import platform
import subprocess

from realization_support import Project, ROOT, r
from selector_support import s

spec = importlib.util.spec_from_file_location("checkout_binding_cli", ROOT / "scripts/test-plan.py")
cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cli)


class CheckoutBindingTests(Project):
    def setUp(self):
        super().setUp()
        # Use the real native fixture and backend, with complete policy metadata
        # and the support-input edge R2 requires. No fixture/backend is mocked.
        compiler = subprocess.check_output([os.environ.get("CC", "cc"), "--version"], text=True)
        self.profile.update(platform={"Darwin": "macos", "Linux": "linux", "Windows": "windows"}.get(platform.system(), platform.system().lower()),
                            architecture=platform.machine(), compiler="clang" if "clang" in compiler.lower() else "gcc",
                            build_type="Debug", instrumentation="none", setup="native-regression-fixture",
                            local_availability="host", environment={"TEST_DISPLAY": "1"})
        self.profile["configure_args"] += ["-DCMAKE_BUILD_TYPE=Debug"]
        self.inventory.update(schema_version=1, profiles={"synthetic": self.profile}, release_components={})
        self.inventory["nodes"]["application"]["depends_on"].append("testing")
        for node in self.inventory["nodes"].values():
            node.update(owner_paths=["CMakeLists.txt"], rationale="Native fixture source and support dependencies")
        for rule in self.inventory["path_rules"]:
            rule.setdefault("effect", "impact")
            rule["reason"] = "Native fixture ownership"
        self.inventory["checks"] = {ident.replace("-", "_"): check for ident, check in self.inventory["checks"].items()}
        for check in self.inventory["checks"].values():
            check.update(cost="ordinary", requirement="blocking", expected_outcome="pass")
        self.git(self.root, "init", "-q")
        self.git(self.root, "config", "user.name", "Selection Test")
        self.git(self.root, "config", "user.email", "selection@example.invalid")
        self.head = self.commit(self.root)
        self.identity = {"kind": "committed", "sha": self.head}
        self.plan = s.make_plan(self.inventory, [{"status": "M", "old_path": "main.c", "new_path": "main.c"}], head=self.head)
        # Keep generated inputs outside both committed checkouts.
        self.planfile = self.root.parent / "plan.json"
        self.policyfile = self.root.parent / "policy.json"
        self.output = self.root.parent / "lane-receipt.json"
        s.write_json(self.planfile, self.plan)
        s.write_json(self.policyfile, self.inventory)
        r.configure_profile(self.root, self.build, "synthetic", self.profile, self.identity, s.digest(self.inventory))
        self.build_all()
        self.binary = self.build / "shared"
        self.binary_before = self.binary.read_bytes()

    def git(self, root, *args):
        return subprocess.check_output(["git", "-C", str(root), *args], stderr=subprocess.PIPE, text=True).strip()

    def commit(self, root):
        self.git(root, "add", "-A")
        self.git(root, "commit", "-qm", "native checkout binding fixture")
        return self.git(root, "rev-parse", "HEAD")

    def clone(self):
        clone = self.root.parent / "other-checkout"
        subprocess.run(["git", "clone", "--quiet", str(self.root), str(clone)], check=True)
        return clone

    def advance(self):
        (self.root / "shared.h").write_text("#define SHARED 99\n")
        return self.commit(self.root)

    def invoke(self, repo=None, command="run"):
        args = [command, "--repo", str(repo or self.root), "--inventory", str(self.policyfile),
                "--plan", str(self.planfile), "--profile", "synthetic", "--build-dir", str(self.build),
                "--output", str(self.output), "--evidence-dir", str(self.evidence)]
        error = io.StringIO()
        with contextlib.redirect_stderr(error), contextlib.redirect_stdout(io.StringIO()):
            status = cli.main(args)
        return status, error.getvalue()

    def assert_no_execution(self):
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.evidence.rglob("receipt.json")))
        self.assertFalse((self.build / "vectors.jsonl").exists())
        self.assertEqual(self.binary.read_bytes(), self.binary_before)

    def test_cross_checkout_same_commit_is_rejected_before_execution(self):
        other = self.clone()
        for command in ("resolve", "check-inputs", "validate", "run"):
            with self.subTest(command=command):
                status, error = self.invoke(other, command)
                self.assertEqual(status, 1, error)
                self.assertIn("repo_root does not match admitted execution checkout", error)
                self.assert_no_execution()

    def test_cross_checkout_advanced_original_cannot_attest_old_plan(self):
        other = self.clone()
        advanced = self.advance()
        self.assertNotEqual(advanced, self.head)
        self.assertEqual(self.git(other, "rev-parse", "HEAD"), self.head)
        status, error = self.invoke(other)
        self.assertEqual(status, 1, error)
        self.assert_no_execution()

    def test_same_checkout_advancement_rejects_old_plan_without_receipt(self):
        self.advance()
        status, error = self.invoke()
        self.assertNotEqual(status, 0, error)
        self.assertIn("not the selected committed SHA", error)
        self.assert_no_execution()

    def test_matching_checkout_reuses_real_build_and_receipt(self):
        status, error = self.invoke()
        self.assertEqual(status, 0, error)
        receipt = s.read_json(self.output)
        self.assertEqual(receipt["source_identity"], self.identity)
        self.assertEqual(receipt["job_conclusion"], "success")
        self.assertEqual((self.build / "vectors.jsonl").read_text().splitlines(), ["evidence-42"] * 3)

    def test_canonical_symlink_alias_is_matching_checkout(self):
        alias = self.root.parent / "source-alias"
        alias.symlink_to(self.root, target_is_directory=True)
        status, error = self.invoke(alias)
        self.assertEqual(status, 0, error)
        self.assertEqual(s.read_json(self.output)["source_identity"], self.identity)
