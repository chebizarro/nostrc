"""Real miniature CMake audit/execution; not production sanitizer acceptance."""
import copy
import importlib.util
import json
import platform
from unittest import mock

from selector_support import s, change
from realization_support import Project, ROOT, r
import test_selection as nm

spec = importlib.util.spec_from_file_location("nonmember_adapter", ROOT / "scripts/test-plan.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class DiagnosticNonmemberTests(Project):
    def setUp(self):
        super().setUp()
        self.profile.update(platform={"Darwin": "macos", "Linux": "linux"}[platform.system()],
                            architecture={"aarch64": "arm64"}.get(platform.machine(), platform.machine()),
                            compiler="clang" if platform.system() == "Darwin" else "gcc",
                            build_type="Debug", instrumentation="none", setup="miniature",
                            local_availability="host", parallelism=1)
        self.profile["configure_args"] += ["-DCMAKE_BUILD_TYPE=Debug"]
        self.profile["capabilities"] = []
        (self.root / "omitted.c").write_text('#include "omitted.h"\nint main(void) { return OMITTED; }\n')
        (self.root / "omitted.h").write_text('#define OMITTED 1\n')
        (self.root / "omitted-vector.txt").write_bytes(b"nonmember runtime\xff\n")
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace('add_executable(shared main.c)',
            'add_executable(shared main.c)\nadd_executable(omitted-bin omitted.c)')
            .replace('add_test(NAME omitted COMMAND shared)', 'add_test(NAME omitted COMMAND omitted-bin)')
            .replace('foreach(t a-model gui omitted)', 'foreach(t a-model gui)')
            .replace('nostrc_finalize_test_inventory()', '''set_tests_properties(omitted PROPERTIES FIXTURES_REQUIRED private)
nostrc_describe_test(omitted OWNER application TARGETS omitted-bin INPUTS omitted-vector.txt)
nostrc_finalize_test_inventory()'''))
        self.inventory["checks"] = {"model.unit": {"kind": "ctest", "profiles": [nm.NONMEMBER_PROFILE],
            "ctest_tests": {nm.NONMEMBER_PROFILE: ["a-model"]}, "nodes": ["application", "testing"],
            "requirement": "blocking", "expected_outcome": "pass", "cost": "ordinary"}}
        for name, node in self.inventory["nodes"].items():
            node.update(owner_paths=[name], rationale="miniature dependency")
        for rule in self.inventory["path_rules"]:
            rule.setdefault("effect", "impact")
            rule["reason"] = "miniature ownership"
        for path in ("omitted.c", "omitted.h", "omitted-vector.txt"):
            self.inventory["path_rules"].append({"exact": path, "nodes": ["application"],
                                                 "effect": "impact", "reason": "nonmember inputs"})
        self.contract = {"schema_version": 1, "profile_hash": s.digest(self.profile),
                         "registrations": ["gui", "omitted"],
                         "required_closure": ["a-model", "cleanup", "reset", "second-setup", "z-first"],
                         "review": {"source_sha": "a" * 40, "post_audit_sha256": "b" * 64}}
        self.inventory.update(schema_version=1, profiles={nm.NONMEMBER_PROFILE: self.profile}, release_components={},
                              diagnostic_nonmembers={nm.NONMEMBER_PROFILE: self.contract})

    def configure(self):
        self.refresh_identity()
        self.plan = s.make_plan(self.inventory, [change("main.c")], head=self.identity["sha"],
                                base="b" * 40, source_identity=self.identity, mode="broad")
        return r.configure_profile(self.root, self.build, nm.NONMEMBER_PROFILE, self.profile,
                                   self.identity, s.digest(self.inventory))

    def execute(self):
        realized = self.configure()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, nm.NONMEMBER_PROFILE)
        self.assertTrue(r.check_inputs(self.inventory, realized)["passed"])
        r.build_resolved(resolved, self.build)
        audit = r.check_inputs(self.inventory, realized, after_build=True)
        self.assertTrue(audit["passed"], audit["gaps"])
        result = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(result["outcome"], "passed", result)
        return realized, resolved, audit, adapter.normalize_receipt(self.inventory, self.plan, resolved, result)

    def test_real_full_audit_build_and_selected_execution_remain_incomplete(self):
        _, resolved, audit, result = self.execute()
        self.assertIn("omitted-bin", resolved["audit_build_targets"])
        self.assertTrue((self.build / "omitted-bin").exists())
        self.assertIn(str(self.root / "omitted.h"), audit["input_hashes"])
        self.assertIn(str(self.root / "omitted-vector.txt"), audit["input_hashes"])
        self.assertNotIn("omitted", result["backend_receipt"]["observed_executions"])
        self.assertNotIn("gui", result["backend_receipt"]["observed_executions"])
        self.assertEqual(result["baseline_exclusions"][0]["status"], "not_executed")
        self.assertFalse(result["backend_receipt"]["complete"])
        self.assertFalse(audit["selective_enforcement"])
        partial = s.aggregate(self.inventory, self.plan, [result], complete=False)
        self.assertTrue(partial["passed"], partial)
        self.assertFalse(s.aggregate(self.inventory, self.plan, [result])["passed"])

    def test_missing_extra_duplicate_and_stale_contracts_refuse(self):
        for mutate in (lambda c: c["registrations"].pop(),
                       lambda c: c["registrations"].append("unknown"),
                       lambda c: c["registrations"].append("omitted"),
                       lambda c: c.update(profile_hash="0" * 64),
                       lambda c: c["required_closure"].remove("cleanup"),
                       lambda c: c["registrations"].insert(0, "a-model")):
            with self.subTest(mutate=mutate):
                original = copy.deepcopy(self.contract)
                mutate(self.contract)
                try:
                    with self.assertRaises((r.RealizationError, s.ContractError)):
                        realized = self.configure()
                        r.resolve_checks(self.inventory, self.plan, realized, nm.NONMEMBER_PROFILE)
                finally:
                    self.contract.clear()
                    self.contract.update(original)

    def test_real_new_missing_or_required_overlap_registration_refuses(self):
        cmake = self.root / "CMakeLists.txt"
        original = cmake.read_text()
        for edit in (lambda t: t.replace('nostrc_finalize_test_inventory()', 'add_test(NAME extra COMMAND omitted-bin)\nnostrc_describe_test(extra OWNER application TARGETS omitted-bin)\nnostrc_finalize_test_inventory()'),
                     lambda t: t.replace('add_test(NAME gui COMMAND shared --gui)', ''),
                     lambda t: t.replace('DEPENDS z-first', 'DEPENDS "z-first;omitted"')):
            with self.subTest(edit=edit):
                cmake.write_text(edit(original))
                with self.assertRaises(r.RealizationError):
                    realized = self.configure()
                    r.resolve_checks(self.inventory, self.plan, realized, nm.NONMEMBER_PROFILE)
        cmake.write_text(original)

    def test_duplicate_export_and_stale_generated_metadata_refuse(self):
        realized = self.configure()
        duplicate = copy.deepcopy(realized)
        duplicate["tests"].append(duplicate["tests"][0])
        with self.assertRaises(r.RealizationError):
            r.resolve_checks(self.inventory, self.plan, duplicate, nm.NONMEMBER_PROFILE)
        path = self.build / "CTestTestfile.cmake"
        path.write_text(path.read_text() + '\nadd_test(extra "/bin/false")\n')
        with self.assertRaises(r.RealizationError):
            r.resolve_checks(self.inventory, self.plan, realized, nm.NONMEMBER_PROFILE)

    def test_omitted_header_runtime_and_impact_gaps_are_not_waived(self):
        realized = self.configure()
        # Only selected target built: omitted header discovery remains mandatory.
        r._command(["cmake", "--build", str(self.build), "--target", "shared"])
        audit = r.check_inputs(self.inventory, realized, after_build=True)
        self.assertTrue(any(g["kind"] == "missing_discovered_header_index" for g in audit["gaps"]))
        self.assertFalse(audit["passed"])
        r._command(["cmake", "--build", str(self.build)])
        for rule in self.inventory["path_rules"]:
            if rule.get("exact") == "omitted.h":
                rule["nodes"] = ["other"]
        realized = self.configure()
        r._command(["cmake", "--build", str(self.build)])
        audit = r.check_inputs(self.inventory, realized, after_build=True)
        self.assertTrue(any(g["kind"] == "missing_impact_edge" and g["test"] == "omitted" for g in audit["gaps"]))
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace('INPUTS omitted-vector.txt', 'INPUTS missing-vector.txt'))
        realized = self.configure()
        self.assertTrue(any(g["kind"] == "missing_input" and g["test"] == "omitted"
                            for g in r.check_inputs(self.inventory, realized)["gaps"]))

    def test_run_rejects_identity_and_audit_binding_mutations(self):
        _, resolved, audit, _ = self.execute()
        for key in ("source_identity", "inventory_hash", "profile_fingerprint", "phase_context_hash",
                    "registration_hash", "descriptors_hash", "generation_hash", "toolchain_hash"):
            with self.subTest(key=key):
                bad = copy.deepcopy(audit)
                bad["baseline_exclusions"][0]["binding"][key] = "0" * 64
                bad["audit_hash"] = r._hash({k: v for k, v in bad.items() if k != "audit_hash"})
                with self.assertRaises(r.RealizationError):
                    r.run_resolved(resolved, self.build, self.evidence / key, input_audit=bad, build=False)

    def test_aggregate_rejects_stripped_stale_duplicate_and_nested_promotions(self):
        _, _, _, result = self.execute()
        for change_result in (lambda v: v.pop("baseline_exclusions"),
                              lambda v: v["baseline_exclusions"].append(v["baseline_exclusions"][0]),
                              lambda v: v["baseline_exclusions"][0].update(status="passed"),
                              lambda v: v.update(complete=True, complete_gate_eligible=True),
                              lambda v: v["backend_receipt"]["input_audit"].update(gaps=[{"kind": "missing_input"}])):
            with self.subTest(change=change_result):
                bad = copy.deepcopy(result)
                change_result(bad)
                self.assertFalse(s.aggregate(self.inventory, self.plan, [bad], complete=False)["passed"])
        partial = s.aggregate(self.inventory, self.plan, [result], complete=False)
        release = {k: self.plan[k] for k in ("source_identity", "tested_sha", "inventory_hash")}
        release.update(tag=None, required_package_jobs=[], publish=False)
        for nested in (result, {"backend_receipt": result}, {"worker": {"validation": partial}}):
            optimistic = dict(partial, passed=True, complete=True, complete_gate_eligible=True,
                              incomplete_lanes=[], mode="broad", partial_profiles=[], tag=None, nested=nested)
            self.assertFalse(s.aggregate_release(release, optimistic, {})["passed"])

    def test_membership_uses_all_authored_checks_not_only_affected_selection(self):
        self.inventory["checks"]["gui.unit"] = dict(self.inventory["checks"]["model.unit"],
                                                  ctest_tests={nm.NONMEMBER_PROFILE: ["gui"]})
        realized = self.configure()
        self.plan["selected_checks"] = ["model.unit"]
        with self.assertRaisesRegex(r.RealizationError, "closure"):
            r.resolve_checks(self.inventory, self.plan, realized, nm.NONMEMBER_PROFILE)

    def test_final_receipt_binding_mutations_refuse_partial(self):
        _, _, _, result = self.execute()
        for key in sorted(nm.NONMEMBER_BINDING_KEYS):
            with self.subTest(key=key):
                bad = copy.deepcopy(result)
                bad["baseline_exclusions"][0]["binding"][key] = "0" * 64
                audit = bad["backend_receipt"]["input_audit"]
                audit["audit_hash"] = s.digest({k: v for k, v in audit.items() if k != "audit_hash"})
                self.assertFalse(s.aggregate(self.inventory, self.plan, [bad], complete=False)["passed"])

    def test_cache_and_ninja_changes_after_audit_refuse(self):
        _, resolved, audit, _ = self.execute()
        for name in ("CMakeCache.txt", "build.ninja", "CMakeFiles/rules.ninja"):
            path = self.build / name
            original = path.read_bytes()
            path.write_bytes(original + b"\n# changed after full-root audit\n")
            try:
                with self.assertRaisesRegex(r.RealizationError, "binding changed"):
                    r.run_resolved(resolved, self.build, self.evidence / path.name, input_audit=audit, build=False)
            finally:
                path.write_bytes(original)

    def test_other_profiles_keep_unclaimed_findings_without_fallback(self):
        self.inventory["profiles"]["other.profile"] = copy.deepcopy(self.profile)
        self.inventory["checks"]["model.unit"].update(profiles=["other.profile"], ctest_tests={"other.profile": ["a-model"]})
        self.refresh_identity()
        realized = r.configure_profile(self.root, self.build, "other.profile", self.profile,
                                       self.identity, s.digest(self.inventory))
        audit = r.check_inputs(self.inventory, realized)
        self.assertEqual(audit["baseline_exclusions"], [])
        self.assertEqual({g["test"] for g in audit["gaps"] if g["kind"] == "unclaimed_registration"}, {"gui", "omitted"})
        self.assertFalse(audit["passed"])

    def test_only_exact_declared_profile_and_real_host_identity_admitted(self):
        for profile in ("groundhog.linux.arm64.prepared011", "groundhog.linux", "unknown"):
            bad = copy.deepcopy(self.inventory)
            bad["diagnostic_nonmembers"][profile] = bad["diagnostic_nonmembers"].pop(nm.NONMEMBER_PROFILE)
            with self.assertRaises(ValueError):
                nm.diagnostic_nonmember_contracts(bad)
        checked = json.loads((ROOT / "tests/selection/inventory.json").read_text())
        self.assertEqual(set(checked["diagnostic_nonmembers"]), {nm.NONMEMBER_PROFILE})
        declared = checked["diagnostic_nonmembers"][nm.NONMEMBER_PROFILE]
        self.assertEqual(len(declared["registrations"]), 347)
        frozen = json.loads((ROOT / "tests/selection/fixtures/nonmember_audit_edges.json").read_text())
        self.assertEqual(declared["registrations"], frozen["unclaimed_registrations"])
        self.assertEqual(declared["review"], {"source_sha": "8d052ca8440eba453f951b62f0c799853980c266",
                                              "post_audit_sha256": "4e65b90e07c0fd26fae54fa6815e156f71643c723f386ebeb3abe36c73aa8436"})
        compat = checked["profiles"][nm.NONMEMBER_PROFILE]["sanitizer_compatibility"]
        self.assertEqual(declared["required_closure"], sorted(compat["GH_SAN_TESTS"] +
            ["groundhog-test-homes", "nip55l-test-homes", "groundhog-test-gst-registry"]))
        self.assertEqual(checked["profiles"][nm.NONMEMBER_PROFILE]["architecture"], "x86_64")
        self.assertEqual(checked["profiles"][nm.NONMEMBER_PROFILE]["parallelism"], 2)
        with mock.patch.object(r.platform, "system", return_value="Linux"), mock.patch.object(r.platform, "machine", return_value="aarch64"):
            with self.assertRaisesRegex(r.RealizationError, "architecture"):
                r._validate_native_profile(checked["profiles"][nm.NONMEMBER_PROFILE])


if __name__ == "__main__":
    import unittest
    unittest.main()
