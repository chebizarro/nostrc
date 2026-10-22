"""CTest runtime constraints must agree with the sole descriptor authority."""
import copy
import hashlib
from types import SimpleNamespace
import unittest

from realization_support import Project, r
from selector_support import ROOT, policy, s


class CTestRuntimeSchemaTests(unittest.TestCase):
    def test_00_actual_production_inventory_validates_before_negatives(self):
        inventory = s.load_inventory(ROOT / "tests/selection/inventory.json")
        self.assertTrue(inventory["checks"]["sanitizer.groundhog_background"]["runtime_inputs"])
        self.assertTrue(inventory["checks"]["groundhog.packaging_version"]["runtime_inputs"])

    def test_malformed_check_local_runtime_constraints_refuse(self):
        s.load_inventory(ROOT / "tests/selection/inventory.json")
        for value in ([""], ["a", "a"], ["bad\x00input"], [1], "SPEC_SOURCE", None, {}, False):
            with self.subTest(value=value):
                inventory = policy()
                inventory["checks"]["core.unit"]["runtime_inputs"] = value
                with self.assertRaisesRegex(s.ContractError, "invalid runtime_inputs"):
                    s.validate_inventory(inventory)

    def test_ctest_consistency_constraints_remain_schema_compatible(self):
        inventory = policy()
        s.validate_inventory(inventory)
        for value in ([], ["core/data"], ["{source}/core/data"], ["{build}/generated"]):
            inventory["checks"]["core.unit"]["runtime_inputs"] = value
            s.validate_inventory(inventory)

    def test_command_runtime_inputs_are_still_supported_and_validated(self):
        inventory = policy()
        check = inventory["checks"]["core.unit"]
        check.update(kind="command", command=["python3", "{source}/core/read.py"],
                     working_directory="{source}", environment={}, runtime_inputs=["{source}/core/data"])
        check.pop("ctest_tests")
        s.validate_inventory(inventory)
        check["runtime_inputs"] = "{source}/core/data"
        with self.assertRaisesRegex(s.ContractError, "invalid runtime_inputs"):
            s.validate_inventory(inventory)


class DescriptorRuntimeTests(Project):
    def setUp(self):
        super().setUp()
        self.runtime = self.root / "SPEC_SOURCE"
        self.runtime.write_text("physically consumed runtime bytes\n")
        main = self.root / "main.c"
        main.write_text(main.read_text().replace('  FILE *f = fopen(', '''  FILE *input = fopen("''' + str(self.runtime) + '''", "r"); if (!input) return 2;
  char bytes[128]; size_t size = fread(bytes, 1, sizeof(bytes), input); fclose(input);
  FILE *output = fopen("consumed.txt", "w"); if (!output) return 3;
  fwrite(bytes, 1, size, output); fclose(output);
  FILE *f = fopen('''))
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("INPUTS vector.txt)", "INPUTS vector.txt SPEC_SOURCE)"))
        self.inventory["path_rules"].append({"exact": "SPEC_SOURCE", "nodes": ["vectors"]})
        self.inventory["checks"]["a-model"]["runtime_inputs"] = ["SPEC_SOURCE"]

    def ready(self):
        realized = self.configure()
        pre = r.check_inputs(self.inventory, realized)
        self.assertTrue(pre["passed"], pre["gaps"])
        self.build_all()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        self.assertTrue(audit["after_build"])
        self.assertEqual(audit["input_hashes"][str(self.runtime)],
                         hashlib.sha256(self.runtime.read_bytes()).hexdigest())
        return realized, resolved, audit

    def run_baseline(self):
        realized, resolved, audit = self.ready()
        receipt = r.run_resolved(resolved, self.build, self.evidence / "baseline", input_audit=audit)
        self.assertEqual(receipt["outcome"], "passed", receipt)
        self.assertEqual((self.build / "consumed.txt").read_bytes(), self.runtime.read_bytes())
        return realized, resolved, audit

    def test_physical_read_resolves_and_full_audits_descriptor_bytes(self):
        _, resolved, audit = self.run_baseline()
        self.assertIn(str(self.runtime), resolved["runtime_inputs"])
        self.assertIn(str(self.runtime), audit["descriptor_inputs"]["a-model"])
        self.assertEqual(resolved["support_tests"], ["cleanup", "reset", "second-setup", "z-first"])
        self.assertIn(str(self.root / "setup.cmake"), resolved["runtime_inputs"])

    def test_declared_input_cannot_extend_canonical_runtime_authority(self):
        s.load_inventory(ROOT / "tests/selection/inventory.json")
        self.run_baseline()
        unlisted = self.root / "owned-unlisted"
        unlisted.write_text("owned but not a runtime dependency\n")
        self.inventory["path_rules"].append({"exact": "owned-unlisted", "nodes": ["vectors"]})
        self.inventory["checks"]["a-model"]["runtime_inputs"] = ["{source}/owned-unlisted"]
        realized = self.configure()
        with self.assertRaisesRegex(r.RealizationError, "CTest runtime input consistency.*absent from authoritative"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        audit = self.audit(realized)
        self.assertFalse(audit["passed"])
        self.assertTrue(any(g["kind"] == "ctest_runtime_input_consistency" for g in audit["gaps"]), audit["gaps"])
        self.assertNotIn(str(unlisted), audit["input_hashes"])

    def test_runtime_constraints_include_actual_fixture_closure(self):
        self.inventory["checks"]["a-model"]["runtime_inputs"] = ["{source}/setup.cmake"]
        _, resolved, audit = self.run_baseline()
        self.assertEqual(resolved["ctest_runtime_constraints"],
                         {"a-model": [str(self.root / "setup.cmake")]})
        self.assertEqual(resolved["ctest_runtime_constraints"], audit["ctest_runtime_constraints"])

    def test_escaping_or_unknown_runtime_constraint_refuses(self):
        realized, _, _ = self.run_baseline()
        for value in ("../outside", "{source}/../outside", str(self.root.parent / "outside"), "{unsupported}/SPEC_SOURCE"):
            with self.subTest(value=value):
                self.inventory["checks"]["a-model"]["runtime_inputs"] = [value]
                with self.assertRaisesRegex(r.RealizationError, "CTest runtime input consistency"):
                    r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
                self.assertFalse(self.audit(realized)["passed"])

    def test_missing_constraint_record_cannot_reuse_audit(self):
        _, resolved, audit = self.run_baseline()
        resolved.pop("ctest_runtime_constraints")
        resolved["resolved_hash"] = r._hash({k: v for k, v in resolved.items() if k != "resolved_hash"})
        with self.assertRaisesRegex(r.RealizationError, "runtime consistency constraints differ"):
            r.run_resolved(resolved, self.build, self.evidence / "missing-constraint-record", input_audit=audit)

    def test_changed_bytes_refuse_validation_and_rerun_after_success(self):
        _, resolved, audit = self.run_baseline()
        self.runtime.write_text("changed after passing execution\n")
        with self.assertRaisesRegex(r.RealizationError, "source snapshot digest changed"):
            r.load_realization(self.build, self.identity)
        with self.assertRaisesRegex(r.RealizationError, "source snapshot digest changed"):
            r.run_resolved(resolved, self.build, self.evidence / "changed", input_audit=audit)

    def test_missing_file_blocks_full_pre_and_post_audits(self):
        self.run_baseline()
        self.runtime.unlink()
        realized = self.configure()
        for after in (False, True):
            audit = r.check_inputs(self.inventory, realized, after_build=after)
            self.assertFalse(audit["passed"])
            self.assertTrue(any(g["kind"] == "missing_input" and g["input"] == str(self.runtime)
                                for g in audit["gaps"]), audit["gaps"])

    def test_escaping_symlink_refuses_at_source_identity_guard(self):
        self.run_baseline()
        outside = self.root.parent / "outside"
        outside.write_text(self.runtime.read_text())
        self.runtime.unlink()
        self.runtime.symlink_to(outside)
        with self.assertRaisesRegex(r.RealizationError, "source snapshot symlink escapes checkout"):
            self.configure()

    def test_external_descriptor_input_blocks_full_audit(self):
        self.run_baseline()
        outside = self.root.parent / "outside"
        outside.write_text(self.runtime.read_text())
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("INPUTS vector.txt SPEC_SOURCE)",
                                                 f'INPUTS vector.txt "{outside}")'))
        audit = self.audit(self.configure())
        self.assertFalse(audit["passed"])
        self.assertTrue(any(g["kind"] == "external_input" and g["input"] == str(outside)
                            for g in audit["gaps"]), audit["gaps"])

    def test_empty_directory_is_not_a_file_input(self):
        self.run_baseline()
        self.runtime.unlink()
        self.runtime.mkdir()
        audit = self.audit(self.configure())
        self.assertFalse(audit["passed"])
        self.assertTrue(any(g["kind"] == "empty_runtime_directory" for g in audit["gaps"]), audit["gaps"])

    def test_owner_and_impact_are_not_bypassed_by_descriptor(self):
        realized, _, _ = self.run_baseline()
        self.inventory["path_rules"][-1]["nodes"] = ["other"]
        audit = self.audit(realized)
        self.assertFalse(audit["passed"])
        self.assertTrue(any(g["kind"] == "missing_impact_edge" and g["input"] == "SPEC_SOURCE"
                            for g in audit["gaps"]), audit["gaps"])

    def test_omitted_registration_still_has_its_runtime_audited(self):
        self.run_baseline()
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("nostrc_finalize_test_inventory()", '''set_property(TEST omitted APPEND PROPERTY NOSTRC_SELECTION_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/missing-omitted")
nostrc_finalize_test_inventory()'''))
        realized = self.configure()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        self.assertNotIn("omitted", resolved["tests"])
        audit = self.audit(realized)
        self.assertFalse(audit["passed"])
        self.assertTrue(any(g["kind"] == "missing_input" and g["test"] == "omitted" for g in audit["gaps"]), audit["gaps"])

    def test_missing_or_forged_resolved_runtime_closure_refuses(self):
        _, resolved, audit = self.run_baseline()
        for operation in ("missing_field", "missing_file", "forged_file"):
            with self.subTest(operation=operation):
                forged = copy.deepcopy(resolved)
                if operation == "missing_field":
                    forged.pop("runtime_inputs")
                elif operation == "missing_file":
                    forged["runtime_inputs"].remove(str(self.runtime))
                else:
                    forged["runtime_inputs"].append(str(self.root / "forged-input"))
                forged["resolved_hash"] = r._hash({k: v for k, v in forged.items() if k != "resolved_hash"})
                with self.assertRaisesRegex(r.RealizationError, "resolved descriptor runtime closure"):
                    r.run_resolved(forged, self.build, self.evidence / operation, input_audit=audit)
                self.assertFalse(list((self.evidence / operation).rglob("ctest.log")))

    def test_missing_or_forged_full_audit_binding_refuses(self):
        _, resolved, audit = self.run_baseline()
        for operation in ("missing_closure", "forged_closure", "missing_hash", "forged_hash"):
            with self.subTest(operation=operation):
                forged = copy.deepcopy(audit)
                if operation == "missing_closure":
                    forged.pop("descriptor_inputs")
                elif operation == "forged_closure":
                    forged["descriptor_inputs"]["a-model"].remove(str(self.runtime))
                elif operation == "missing_hash":
                    forged["input_hashes"].pop(str(self.runtime))
                else:
                    forged["input_hashes"][str(self.runtime)] = "0" * 64
                forged["audit_hash"] = r._hash({k: v for k, v in forged.items() if k != "audit_hash"})
                with self.assertRaisesRegex(r.RealizationError, "descriptor runtime|input changed since audit"):
                    r.run_resolved(resolved, self.build, self.evidence / operation, input_audit=forged)
                self.assertFalse(list((self.evidence / operation).rglob("ctest.log")))

    def test_directory_inputs_and_internal_symlinks_preserve_closure(self):
        directory = self.root / "runtime-directory"
        directory.mkdir()
        (directory / "data").write_text("directory runtime bytes\n")
        (directory / "internal-link").symlink_to(self.runtime)
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("INPUTS vector.txt SPEC_SOURCE)",
                                                 "INPUTS vector.txt SPEC_SOURCE runtime-directory)"))
        self.inventory["path_rules"].append({"prefix": "runtime-directory", "nodes": ["vectors"]})
        self.inventory["checks"]["a-model"]["runtime_inputs"].append("runtime-directory/data")
        _, resolved, audit = self.run_baseline()
        self.assertNotIn(str(directory / "data"), resolved["runtime_inputs"])
        self.assertIn(str(directory), resolved["runtime_inputs"])
        for name in ("data", "internal-link"):
            self.assertIn(str(directory / name), audit["input_hashes"])

    def test_mixed_command_and_ctest_runtime_union_is_exact(self):
        self.inventory["checks"]["command"] = {
            "kind": "command", "profiles": ["synthetic"], "nodes": ["testing"],
            "command": ["cmake", "-P", "{source}/noop.cmake"],
            "working_directory": "{source}", "environment": {},
            "runtime_inputs": ["{source}/noop.cmake"]}
        self.plan["selected_checks"].append("command")
        _, resolved, audit = self.ready()
        expected = {p for n in resolved["tests"] for p in audit["descriptor_inputs"][n]}
        expected.update(resolved["command_executions"]["command"]["inputs"])
        self.assertEqual(resolved["runtime_inputs"], sorted(expected))
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit)
        self.assertEqual(receipt["outcome"], "passed", receipt)
        self.assertEqual(receipt["observed_executions"]["command"], "passed")
        self.assertEqual(receipt["observed_executions"]["a-model"], "passed")

    def test_duplicate_descriptor_override_authority_still_refuses(self):
        realized, _, _ = self.run_baseline()
        self.inventory["test_overrides"] = [{"name": "a-model", "inputs": [str(self.runtime)]}]
        with self.assertRaisesRegex(r.RealizationError, "duplicate descriptor/override authority"):
            r.check_inputs(self.inventory, realized)


class RuntimeAuthorityTests(Project):
    def ready(self):
        self.refresh_identity()
        self.plan["inventory_hash"] = r._hash(self.inventory)
        realized = r.configure_profile(self.root, self.build, "synthetic", self.profile,
                                       self.identity, self.plan["inventory_hash"], inventory=self.inventory)
        original = copy.deepcopy(realized)
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        self.assertEqual(realized, original)  # overrides must not rewrite exported descriptors
        pre = r.check_inputs(self.inventory, realized)
        self.assertTrue(pre["passed"], pre["gaps"])
        self.build_all()
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        self.assertEqual(realized, original)
        receipt = r.run_resolved(resolved, self.build, self.evidence / "baseline", input_audit=audit)
        self.assertEqual(receipt["outcome"], "passed", receipt)
        return resolved, audit

    def test_empty_repository_descriptor_rejects_jointly_forged_closures(self):
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("INPUTS vector.txt)", 'INPUTS "")'))
        resolved, audit = self.ready()
        self.assertEqual(audit["descriptor_inputs"]["a-model"], [])
        added = str(self.root / "main.c")  # already hashed as a compiled input
        forged_audit, forged_resolved = copy.deepcopy(audit), copy.deepcopy(resolved)
        forged_audit["descriptor_inputs"]["a-model"] = [added]
        forged_resolved["runtime_inputs"] = sorted(set(forged_resolved["runtime_inputs"]) | {added})
        forged_audit["audit_hash"] = r._hash({k: v for k, v in forged_audit.items() if k != "audit_hash"})
        forged_resolved["resolved_hash"] = r._hash({k: v for k, v in forged_resolved.items() if k != "resolved_hash"})
        with self.assertRaisesRegex(r.RealizationError, "descriptor runtime closure differs"):
            r.run_resolved(forged_resolved, self.build, self.evidence / "joint-forgery", input_audit=forged_audit)
        self.assertFalse(list((self.evidence / "joint-forgery").rglob("ctest.log")))

    def test_legacy_override_requires_content_bound_inventory_provenance(self):
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("nostrc_finalize_test_inventory()", '''add_test(NAME legacy COMMAND ${CMAKE_COMMAND} -Dexe=$<TARGET_FILE:shared> -P ${CMAKE_CURRENT_SOURCE_DIR}/noop.cmake)
nostrc_finalize_test_inventory()'''))
        (self.root / "noop.cmake").write_text('''if(DEFINED exe)
  execute_process(COMMAND "${exe}" RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "legacy executable failed")
  endif()
endif()
''')
        self.inventory["test_overrides"] = [{"name": "legacy", "owner": ["application"], "targets": ["shared"],
                                             "inputs": [str(self.root / "noop.cmake")]}]
        self.inventory["checks"]["legacy"] = {"kind": "ctest", "profiles": ["synthetic"],
                                               "ctest_tests": {"synthetic": ["legacy"]}, "nodes": ["application", "testing"]}
        self.plan["selected_checks"] = ["legacy"]
        self.inventory["profiles"] = {"synthetic": self.profile}
        resolved, audit = self.ready()
        self.assertEqual(audit["override_inventory_hash"], resolved["inventory_hash"])
        planfile = self.root.parent / "legacy-plan.json"
        output = self.root.parent / "legacy-cli-receipt.json"
        r._atomic(planfile, self.plan)
        args = SimpleNamespace(profile="synthetic", repo=str(self.root), plan=str(planfile),
                               configure=True, command="run", phase="all", build_dir=str(self.build.parent / "cli-build"),
                               resolved=None, output=str(output), attempt=1)
        code = r.cli(args, self.inventory)
        self.assertEqual(code, 0, r._read(output))
        self.assertEqual(r._read(output)["outcome"], "passed")
        for mutation in ("missing", "forged", "jointly_stripped"):
            with self.subTest(mutation=mutation):
                forged = copy.deepcopy(audit)
                supplied = copy.deepcopy(resolved)
                if mutation == "forged":
                    forged["override_inventory_hash"] = "0" * 64
                else:
                    forged.pop("override_inventory_hash")
                if mutation == "jointly_stripped":
                    forged["descriptor_inputs"]["legacy"] = []
                    supplied["runtime_inputs"] = []
                    supplied["resolved_hash"] = r._hash({k: v for k, v in supplied.items() if k != "resolved_hash"})
                forged["audit_hash"] = r._hash({k: v for k, v in forged.items() if k != "audit_hash"})
                with self.assertRaisesRegex(r.RealizationError, "override inventory content identity"):
                    r.run_resolved(supplied, self.build, self.evidence / mutation, input_audit=forged)
                self.assertFalse(list((self.evidence / mutation).rglob("ctest.log")))
        record = self.build / "nostrc-test-realization.json"
        original = r._read(record)
        for mutation in ("missing_configured_policy", "forged_configured_policy"):
            with self.subTest(mutation=mutation):
                forged = copy.deepcopy(original)
                if mutation == "missing_configured_policy":
                    forged.pop("override_inventory")
                else:
                    forged["override_inventory"]["test_overrides"][0]["inputs"] = []
                try:
                    r._atomic(record, forged)
                    with self.assertRaisesRegex(r.RealizationError, "cached profile fingerprint mismatch"):
                        r.run_resolved(resolved, self.build, self.evidence / mutation, input_audit=audit)
                finally:
                    r._atomic(record, original)
