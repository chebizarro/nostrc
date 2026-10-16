from realization_support import Project, r


class InputTests(Project):
    def test_complete_native_source_runtime_header_audit(self):
        realized = self.configure()
        self.build_all()
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        self.assertGreater(audit["header_entries"], 0)
        self.assertIn(str(self.root / "shared.h"), audit["input_hashes"])

    def test_discovered_header_missing_edge_blocks_even_omitted_test(self):
        realized = self.configure()
        self.build_all()
        self.inventory["nodes"]["application"]["depends_on"].remove("core")
        audit = self.audit(realized)
        self.assertFalse(audit["passed"])
        self.assertTrue(audit["requires_broad"])
        self.assertTrue(any(g["kind"] == "missing_impact_edge" and g["test"] == "omitted"
                            and g["input"] == "shared.h" for g in audit["gaps"]))

    def test_missing_runtime_file_is_not_ignored(self):
        realized = self.configure()
        (self.root / "vector.txt").unlink()
        audit = r.check_inputs(self.inventory, realized)
        self.assertTrue(any(g["kind"] == "missing_input" for g in audit["gaps"]))

    def test_unowned_registration_is_explicit_gap(self):
        realized = self.configure()
        realized["descriptors"] = [d for d in realized["descriptors"] if d["name"] != "omitted"]
        audit = r.check_inputs(self.inventory, realized)
        self.assertIn({"kind": "unowned_registration", "test": "omitted"}, audit["gaps"])

    def test_no_header_index_is_not_a_passing_audit(self):
        realized = self.configure()
        audit = self.audit(realized)
        self.assertTrue(any(g["kind"] == "missing_discovered_header_index" for g in audit["gaps"]))

    def test_dirty_input_identity_must_match(self):
        realized = self.configure()
        with self.assertRaisesRegex(r.RealizationError, "identity mismatch"):
            r.check_inputs(self.inventory, realized, dict(self.identity, snapshot_digest="changed"))

    def test_deleted_file_in_runtime_directory_remains_required(self):
        directory = self.root / "runtime-dir"
        directory.mkdir()
        (directory / "required-schema.xml").write_text("schema")
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("INPUTS vector.txt", "INPUTS runtime-dir"))
        self.inventory["path_rules"].append({"prefix": "runtime-dir/", "nodes": ["vectors"]})
        realized = self.configure()
        (directory / "required-schema.xml").unlink()
        audit = r.check_inputs(self.inventory, realized)
        self.assertTrue(any(g["kind"] == "missing_input" and g["input"].endswith("required-schema.xml") for g in audit["gaps"]))
        self.assertTrue(any(g["kind"] == "empty_runtime_directory" for g in audit["gaps"]))

    def test_runtime_symlink_cannot_escape_checkout(self):
        realized = self.configure()
        path = self.root / "vector.txt"
        path.unlink()
        external = self.root.parent / "external"
        external.write_text("not source snapshot content")
        path.symlink_to(external)
        audit = r.check_inputs(self.inventory, realized)
        self.assertTrue(any(g["kind"] == "external_symlink_input" for g in audit["gaps"]))
