from unittest import mock
from realization_support import Project, r


class SourceIdentityTests(Project):
    def committed(self):
        self.git("add", "-A")
        self.git("-c", "core.hooksPath=/dev/null", "commit", "--allow-empty", "-qm", "committed source")
        self.identity = {"kind": "committed", "sha": self.git("rev-parse", "HEAD").decode().strip()}
        self.plan["source_identity"] = self.identity
        return r.configure_profile(self.root, self.build, "synthetic", self.profile,
                                   self.identity, "inventory-fixture")

    def test_matching_committed_checkout_reuses_realization(self):
        realized = self.committed()
        self.assertEqual(r.load_realization(self.build, self.identity), realized)
        alias = self.build.parent / "build-alias"
        alias.symlink_to(self.build, target_is_directory=True)
        self.assertEqual(r.load_realization(alias, self.identity), realized)

    def test_same_checkout_head_advance_rejected_before_build_or_run(self):
        realized = self.committed()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        (self.root / "shared.h").write_text("#define SHARED 99\n")
        self.git("add", "shared.h")
        self.git("-c", "core.hooksPath=/dev/null", "commit", "-qm", "advanced source")
        with mock.patch.object(r, "_execute") as execute:
            with self.assertRaisesRegex(r.RealizationError, "source checkout HEAD"):
                r.load_realization(self.build, self.identity)
            with self.assertRaisesRegex(r.RealizationError, "source checkout HEAD"):
                r.build_resolved(resolved, self.build)
            with self.assertRaisesRegex(r.RealizationError, "source checkout HEAD"):
                r.run_resolved(resolved, self.build, self.evidence)
            execute.assert_not_called()
        self.assertFalse(list(self.evidence.rglob("receipt.json")))

    def test_committed_source_edits_and_untracked_inputs_rejected(self):
        self.committed()
        (self.root / "shared.h").write_text("#define SHARED 99\n")
        with self.assertRaisesRegex(r.RealizationError, "tracked changes"):
            r.load_realization(self.build)
        self.git("restore", "shared.h")
        (self.root / "injected.cmake").write_text("# unexpected input\n")
        with self.assertRaisesRegex(r.RealizationError, "untracked input"):
            r.load_realization(self.build)

    def test_assume_unchanged_index_cannot_hide_committed_input_edits(self):
        self.committed()
        self.git("update-index", "--assume-unchanged", "shared.h")
        (self.root / "shared.h").write_text("#define SHARED 99\n")
        self.assertFalse(self.git("status", "--porcelain"))
        with self.assertRaisesRegex(r.RealizationError, "committed source input changed"):
            r.load_realization(self.build, self.identity)

    def test_configure_cannot_certify_wrong_head_or_snapshot(self):
        for identity, message in (({"kind": "committed", "sha": "not-the-head"}, "source checkout HEAD"),
                                  (dict(self.identity, snapshot_digest="not-the-bytes"), "snapshot digest")):
            with self.subTest(identity=identity):
                with self.assertRaisesRegex(r.RealizationError, message):
                    r.configure_profile(self.root, self.build, "synthetic", self.profile,
                                        identity, "inventory-fixture")
        self.assertFalse((self.build / "nostrc-test-realization.json").exists())

    def test_dirty_snapshot_is_revalidated_against_actual_bytes_and_modes(self):
        (self.root / "vector.txt").write_text("materialized dirty input\n")
        realized = self.configure()
        self.assertEqual(r.load_realization(self.build, self.identity), realized)
        (self.root / "vector.txt").write_text("different bytes, identical stored identity\n")
        with self.assertRaisesRegex(r.RealizationError, "actual source snapshot digest"):
            r.load_realization(self.build, self.identity)
        (self.root / "vector.txt").write_text("materialized dirty input\n")
        (self.root / "vector.txt").chmod(0o755)
        with self.assertRaisesRegex(r.RealizationError, "file mode changed"):
            r.load_realization(self.build, self.identity)

    def test_dirty_snapshot_added_deleted_and_renamed_inputs_are_not_omitted(self):
        (self.root / "added input.txt").write_text("new snapshot member")
        (self.root / "vector.txt").rename(self.root / "renamed-vector.txt")
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("vector.txt", "renamed-vector.txt"))
        self.configure()
        r.load_realization(self.build, self.identity)
        (self.root / "added input.txt").unlink()
        with self.assertRaisesRegex(r.RealizationError, "snapshot input missing"):
            r.load_realization(self.build, self.identity)

    def test_snapshot_symlink_content_cannot_change_or_escape(self):
        link = self.root / "snapshot-link"
        link.symlink_to("vector.txt")
        self.configure()
        r.load_realization(self.build, self.identity)
        link.unlink()
        link.symlink_to("shared.h")
        with self.assertRaisesRegex(r.RealizationError, "snapshot digest"):
            r.load_realization(self.build, self.identity)
        link.unlink()
        link.symlink_to(self.root.parent / "outside")
        with self.assertRaisesRegex(r.RealizationError, "symlink escapes"):
            r.load_realization(self.build, self.identity)

    def test_metadata_source_checkout_must_exist(self):
        self.configure()
        moved = self.root.parent / "moved-source"
        self.root.rename(moved)
        with self.assertRaisesRegex(r.RealizationError, "source checkout"):
            r.load_realization(self.build, self.identity)
