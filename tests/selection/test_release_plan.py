import copy
from selector_support import GitCase, ROOT, s


class ReleaseTests(GitCase):
    def setUp(self):
        super().setUp()
        self.p["release_components"] = {
            "app": {"manifest_name": "App", "publisher": "main", "package_jobs": ["source", "linux.x86_64", "linux.aarch64"], "version_sources": [{"path": "app/version", "pattern": r"version=([0-9.]+)"}, {"path": "app/meson", "pattern": r"version=([0-9.]+)"}]},
            "library": {"manifest_name": "Library", "publisher": "none", "package_jobs": [], "version_sources": [{"path": "core/version", "pattern": r"version=([0-9.]+)"}]},
            "groundhog": {"manifest_name": "Groundhog", "publisher": "groundhog", "package_jobs": ["source", "deb.x86_64", "deb.aarch64", "nix"], "version_sources": [{"path": "groundhog/version", "pattern": r"version=([0-9.]+)"}], "full_version_source": {"path": "groundhog/version", "pattern": r"prerelease=([^\n]*)"}},
        }
        self.p["legacy_global"] = {"publishers": ["main", "windows"], "package_jobs": ["source", "linux.x86_64", "linux.aarch64", "windows.build"]}
        self.write("VERSION_MANIFEST.md", "| Component | Path | Declared version | Latest release | Release tag | Sources |\n| App | app | 1.2.3 | Unreleased | - | app/version |\n| Library | core | 9.8.7 | Unreleased | - | core/version |\n| Groundhog | groundhog | 0.12.0 | Unreleased | - | groundhog/version |\n")
        self.write("app/version", "version=1.2.3\n"); self.write("app/meson", "version=1.2.3\n")
        self.write("core/version", "version=9.8.7\n")
        self.write("groundhog/version", "version=0.12.0\nprerelease=alpha8\n")
        self.head = self.commit()

    def tagged(self, name, annotated=False):
        self.git("tag", *( ["-am", "annotated"] if annotated else []), name, self.head)
        return s.release_plan(self.repo, self.p, head=self.head, tag=name)

    def validation(self, release):
        return {"passed": True, "complete": True, "source_identity": release["source_identity"], "tested_sha": release["tested_sha"], "inventory_hash": release["inventory_hash"], "mode": "broad", "tag": release["tag"], "partial_profiles": []}

    def test_annotated_validation_only_tag_has_no_publisher(self):
        result = self.tagged("library-v9.8.7-preview", annotated=True)
        self.assertEqual(result["tested_sha"], self.head)
        self.assertTrue(result["validation_only"])
        self.assertFalse(result["publish"])
        self.assertEqual(result["required_package_jobs"], [])

    def test_independent_versions_not_compared_to_global_tag_number(self):
        result = self.tagged("v123.0.0")
        self.assertEqual(result["identity"], "legacy_global")
        self.assertEqual(result["verified_versions"], {"app": "1.2.3", "library": "9.8.7", "groundhog": "0.12.0"})
        self.assertEqual(result["publishers"], ["main", "windows"])

    def test_unknown_prefix_never_falls_back_to_global(self):
        with self.assertRaises(s.ContractError):
            self.tagged("unrecognized-v1.2.3")

    def test_wrong_tag_sha_and_base_version_fail(self):
        self.git("tag", "app-v1.2.3", self.base)
        with self.assertRaises(s.CheckFailure):
            s.release_plan(self.repo, self.p, head=self.head, tag="app-v1.2.3")
        with self.assertRaises(s.CheckFailure):
            self.tagged("app-v1.2.4")

    def test_component_prerelease_full_source_enforced(self):
        result = self.tagged("groundhog-v0.12.0-alpha8")
        self.assertEqual(result["identity"], "groundhog")
        with self.assertRaises(s.CheckFailure):
            self.tagged("groundhog-v0.12.0-alpha7")

    def test_bad_semver(self):
        for value in ("01.2.3", "1.2", "1.2.3-01", "1.2.3-", "1.2.3.4"):
            with self.subTest(value=value), self.assertRaises(s.ContractError):
                self.tagged("app-v" + value)

    def test_source_manifest_mismatch_and_duplicate_source_disagreement(self):
        self.write("app/meson", "version=1.2.4\n")
        self.head = self.commit()
        with self.assertRaises(s.CheckFailure):
            self.tagged("app-v1.2.3")

    def test_dirty_version_files_do_not_change_tag_validation(self):
        self.write("app/version", "version=99.99.99\n")
        self.assertEqual(self.tagged("app-v1.2.3")["verified_versions"]["app"], "1.2.3")

    def test_branch_and_tag_dry_runs_never_publish(self):
        result = s.release_plan(self.repo, self.p, head=self.head, component="app", dry_run=True, run_id="manual-123")
        self.assertFalse(result["publish"])
        self.assertIsNone(result["version"])
        self.git("tag", "app-v1.2.3", self.head)
        result = s.release_plan(self.repo, self.p, head=self.head, tag="app-v1.2.3", dry_run=True)
        self.assertFalse(result["publish"])

    def test_release_aggregate_requires_every_matrix_row(self):
        result = self.tagged("app-v1.2.3")
        jobs = {j: "success" for j in result["required_package_jobs"]}
        valid = self.validation(result)
        self.assertTrue(s.aggregate_release(result, valid, jobs)["publish"])
        for status in ("failure", "cancelled", "skipped", None):
            incomplete = dict(jobs, **{"linux.aarch64": status})
            self.assertFalse(s.aggregate_release(result, valid, incomplete)["passed"])
        missing = dict(jobs); missing.pop("linux.aarch64")
        self.assertFalse(s.aggregate_release(result, valid, missing)["passed"])
        jobs["unselected.package"] = "not_applicable"
        self.assertTrue(s.aggregate_release(result, valid, jobs)["passed"])
        jobs["unselected.package"] = "success"
        self.assertFalse(s.aggregate_release(result, valid, jobs)["passed"])

    def test_release_validation_exact_tag_sha_broad_identity(self):
        result = self.tagged("app-v1.2.3")
        jobs = {j: "success" for j in result["required_package_jobs"]}
        valid = self.validation(result)
        baseline = s.aggregate_release(result, valid, jobs)
        self.assertTrue(baseline["passed"])
        self.assertTrue(baseline["publish"])
        self.assertEqual(baseline["errors"], [])
        for key, wrong in (("tested_sha", self.base), ("tag", "other"), ("mode", "affected"), ("inventory_hash", "stale"), ("passed", False), ("partial_profiles", ["linux"])):
            with self.subTest(key=key):
                invalid = copy.deepcopy(valid)
                invalid[key] = wrong
                rejected = s.aggregate_release(result, invalid, jobs)
                self.assertFalse(rejected["passed"])
                self.assertFalse(rejected["publish"])
                self.assertEqual(rejected["errors"], ["missing/mismatched broad exact-tag validation"])

    def test_checked_in_version_sources_match_independent_manifest(self):
        p = s.load_inventory(ROOT / "tests/selection/inventory.json")
        result = s.release_plan(ROOT, p, head="HEAD", component="global", dry_run=True, run_id="test-only")
        self.assertEqual(len(result["verified_versions"]), 17)
        self.assertFalse(result["publish"])
