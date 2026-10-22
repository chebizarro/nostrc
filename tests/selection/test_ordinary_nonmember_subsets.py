"""Synthetic exact-33 graph/build tests, NOT authored Linux/x86 acceptance.

Native miniature builds validate real CMake/Ninja/header/runtime behavior. Only
host admission is adapted for this test fixture; production admission is tested
separately and is never relaxed by these tests or by policy activation.
"""
import copy
import hashlib
import importlib.util
import json
import platform
from unittest import mock

from realization_support import Project, ROOT
from selector_support import s, change
import test_selection_realization as r
import test_selection_subset as subset

spec = importlib.util.spec_from_file_location("ordinary_subset_adapter", ROOT / "scripts/test-plan.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class OrdinarySubsetTests(Project):
    def setUp(self):
        super().setUp()
        original = r._validate_native_profile
        def native_fixture(profile, *args, **kwargs):
            native = dict(profile, platform={"Darwin": "macos", "Linux": "linux"}[platform.system()],
                          architecture={"aarch64": "arm64"}.get(platform.machine(), platform.machine()),
                          compiler="clang" if platform.system() == "Darwin" else "gcc")
            return original(native, *args, **kwargs)
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(r, "_validate_native_profile", side_effect=native_fixture).start()
        self.profile.update(platform="linux", architecture="x86_64", compiler="gcc", build_type="Debug",
                            instrumentation="none", feature_variants=[], setup="synthetic",
                            local_availability="host", parallelism=1, timeout_seconds=60,
                            capabilities=[])
        self.profile["configure_args"] += ["-DCMAKE_BUILD_TYPE=Debug"]
        libgo = self.root / "libgo"
        libgo.mkdir()
        (libgo / "omitted.c").write_text('#include "omitted.h"\nextern int support(void);\nint main(void) { return OMITTED + support(); }\n')
        (libgo / "support.c").write_text('int support(void) { return 0; }\n')
        (libgo / "omitted.h").write_text('#define OMITTED 1\n')
        (libgo / "vector.bin").write_bytes(b"runtime\xff\n")
        (libgo / "CMakeLists.txt").write_text('''add_library(subset-lib STATIC support.c)
add_executable(subset-bin omitted.c)
target_link_libraries(subset-bin PRIVATE subset-lib)
''' + '\n'.join(f'''add_test(NAME {name} COMMAND subset-bin)
nostrc_describe_test({name} OWNER libgo TARGETS subset-bin INPUTS vector.bin)''' for name in subset.REGISTRATIONS) + '\n')
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace('nostrc_finalize_test_inventory()',
                                                 'add_subdirectory(libgo)\nnostrc_finalize_test_inventory()'))
        for pid, (path, include, exclude) in subset.PROFILES.items():
            p = self.root / path
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(f"# Synthetic workflow witness {pid}\n# {include}\n# {exclude}\n")
        self.inventory["nodes"]["libgo"] = {}
        self.inventory["path_rules"].append({"prefix": "libgo/", "nodes": ["libgo"], "effect": "impact", "reason": "precise fixture owner"})
        self.inventory["path_rules"].append({"prefix": ".github/", "nodes": [], "effect": "global", "reason": "workflow fixture"})
        for name, node in self.inventory["nodes"].items():
            node.update(owner_paths=[name], rationale="synthetic dependency")
        for rule in self.inventory["path_rules"]:
            rule.setdefault("effect", "impact")
            rule.setdefault("reason", "synthetic ownership")
        self.inventory.update(schema_version=1, release_components={})
        self.pid = "groundhog.linux"

    def configure(self, pid=None, *, admit=True):
        self.pid = pid or self.pid
        if self.pid == "grotto.linux":
            self.profile.update(build_parallelism=2, test_parallelism=1)
        self.inventory["profiles"] = {self.pid: self.profile}
        self.inventory["checks"] = {"model.unit": {"kind": "ctest", "profiles": [self.pid],
            "ctest_tests": {self.pid: ["a-model"]}, "nodes": ["application", "testing"],
            "requirement": "blocking", "expected_outcome": "pass", "cost": "ordinary"}}
        self.refresh_identity()
        real = r.configure_profile(self.root, self.build, self.pid, self.profile, self.identity, s.digest(self.inventory))
        if admit and not self.inventory.get("diagnostic_nonmember_subsets"):
            self.contract = self.review_contract(real)
            self.inventory["diagnostic_nonmember_subsets"] = {self.pid: self.contract}
            real = r.configure_profile(self.root, self.build, self.pid, self.profile, self.identity, s.digest(self.inventory))
        self.plan = s.make_plan(self.inventory, [change("main.c")], head=self.identity["sha"], base="b" * 40,
                                source_identity=self.identity, mode="broad")
        return real

    def review_contract(self, real):
        tests = {t["name"]: t for t in real["tests"]}
        descriptors = {d["name"]: d for d in real["descriptors"]}
        path, include, exclude = subset.PROFILES[self.pid]
        inputs = {name: hashlib.sha256((self.root / name).read_bytes()).hexdigest() for name in
                  (path, "CMakeLists.txt", "libgo/CMakeLists.txt", "cmake/NostrcTestInventory.cmake")}
        return {"schema_version": 1, "profile_hash": s.digest(self.profile), "registrations": list(subset.REGISTRATIONS),
            "workflow": dict(path=path, include=include, exclude=exclude, sha256=inputs[path]),
            "registration_universe": sorted(tests), "universe_hash": s.digest(sorted(tests)),
            "primary_registrations": ["a-model"], "required_closure": r._fixture_closure(["a-model"], tests, descriptors),
            "candidate_contract_hash": s.digest(subset.candidate_contract(real, tests, descriptors)),
            "toolchain_contract_hash": s.digest(subset.toolchain_contract(real)),
            "review": {"reviewer": "synthetic-fixture-only", "approval_document": "docs/plans/ordinary-profile-nonmember-policy-proposal-2026-10-19.md",
                "tracking": ["nostrc-0cfsg.2", "nostrc-0cfsg.9"], "source_sha": self.identity["sha"],
                "source_inputs": inputs, "pre_audit_sha256": "a" * 64, "post_audit_sha256": "b" * 64,
                "realization_sha256": "c" * 64, "profile_fingerprint": real["profile_fingerprint"],
                **{k: self.profile[k] for k in ("platform", "architecture", "compiler", "feature_variants", "instrumentation")}}}

    def execute(self, pid=None, *, parallel=None):
        real = self.configure(pid)
        resolved = r.resolve_checks(self.inventory, self.plan, real, self.pid)
        # Existing unrelated remainder is not approved: keep its findings visible.
        before = r.check_inputs(self.inventory, real)
        self.assertEqual({g["test"] for g in before["gaps"] if g["kind"] == "unclaimed_registration"}, {"gui", "omitted"})
        # Enroll only these unrelated fixture checks for the positive end-to-end
        # diagnostic, never alter the production candidate scope or audit guards.
        self.inventory["checks"]["model.unit"]["ctest_tests"][self.pid] += ["gui", "omitted"]
        self.contract["primary_registrations"] = ["a-model", "gui", "omitted"]
        self.contract["required_closure"] = sorted(set(self.contract["required_closure"]) | {"gui", "omitted"})
        # GUI fixture command is not a production display contract.
        self.profile["environment"]["TEST_DISPLAY"] = "1"
        self.contract["profile_hash"] = s.digest(self.profile)
        self.refresh_identity()
        real = r.configure_profile(self.root, self.build, self.pid, self.profile, self.identity, s.digest(self.inventory))
        self.plan = s.make_plan(self.inventory, [change("main.c")], head=self.identity["sha"], base="b" * 40,
                                source_identity=self.identity, mode="broad")
        resolved = r.resolve_checks(self.inventory, self.plan, real, self.pid)
        self.assertTrue(r.check_inputs(self.inventory, real)["passed"])
        r.build_resolved(resolved, self.build, parallel=parallel)
        audit = r.check_inputs(self.inventory, real, after_build=True)
        self.assertTrue(audit["passed"], audit["gaps"])
        raw = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=True, parallel=parallel)
        result = adapter.normalize_receipt(self.inventory, self.plan, resolved, raw)
        return real, resolved, audit, result

    def test_exact_two_profiles_positive_and_incomplete_end_to_end(self):
        for pid in subset.PROFILES:
            with self.subTest(pid=pid):
                if pid != self.pid:
                    self.inventory.pop("diagnostic_nonmember_subsets", None)
                    self.build = self.build.parent / "grotto-build"
                _, resolved, audit, result = self.execute(pid)
                self.assertEqual(result["baseline_exclusions"][0]["registrations"], list(subset.REGISTRATIONS))
                self.assertEqual(result["baseline_exclusions"][0]["status"], "not_executed")
                self.assertFalse(result["complete"])
                self.assertEqual(resolved["audit_build_targets"], ["subset-bin", "subset-lib"])
                self.assertIn(str(self.root / "libgo/omitted.h"), audit["input_hashes"])
                self.assertIn(str(self.root / "libgo/vector.bin"), audit["input_hashes"])
                self.assertFalse(set(subset.REGISTRATIONS) & set(result["backend_receipt"]["observed_executions"]))
                partial = s.aggregate(self.inventory, self.plan, [result], complete=False)
                self.assertTrue(partial["passed"], partial)
                complete = s.aggregate(self.inventory, self.plan, [result])
                self.assertFalse(complete["passed"])
                self.assertFalse(complete["complete"])
                plan_file = self.build / "adapter-plan.json"
                s.write_json(plan_file, self.plan)
                args = adapter.parser().parse_args(["run", "--repo", str(self.root), "--profile", pid,
                    "--plan", str(plan_file), "--build-dir", str(self.build),
                    "--evidence-dir", str(self.evidence / "adapter"), "--phase", "all"])
                cli_result, code = adapter.realization_command(args, self.inventory, r)
                self.assertEqual(code, 0, cli_result)
                self.assertTrue(s.aggregate(self.inventory, self.plan, [cli_result], complete=False)["passed"])
                self.assertFalse(cli_result["complete"])

    def test_authored_shared_parallelism_matches_worker_and_receipt_without_changing_defaults(self):
        self.profile["parallelism"] = 2
        real, resolved, audit, result = self.execute(parallel=2)
        options = dict(build_parallel=2, test_parallel=2, timeout_seconds=60)
        self.assertEqual(result["backend_receipt"]["execution_options"], options)
        self.assertTrue(s.aggregate(self.inventory, self.plan, [result], complete=False)["passed"])
        self.assertFalse(result["complete"])
        self.assertFalse(result["complete_gate_eligible"])
        for command in result["backend_receipt"]["commands"]:
            if command and command[0] in ("cmake", "ctest"):
                self.assertEqual(command.count("--parallel"), 1)
                self.assertEqual(command[command.index("--parallel") + 1], "2")
        # Legacy API defaults stay at one; they cannot authorize this profile.
        self.assertEqual(r.effective_phase_parallelism(self.profile), dict(build=1, test=1))
        for parallel in (None, 1, 3, True):
            with self.subTest(parallel=parallel), mock.patch.object(r, "_execute", wraps=r._execute) as execute:
                with self.assertRaises((ValueError, r.RealizationError)):
                    r.build_resolved(resolved, self.build, parallel=parallel)
                self.assertFalse(any(call.args[0][:2] == ["cmake", "--build"] for call in execute.call_args_list))
                with self.assertRaises((ValueError, r.RealizationError)):
                    r.run_resolved(resolved, self.build, self.evidence / "wrong-parallel",
                                   input_audit=audit, build=True, parallel=parallel)
                self.assertFalse(any("--output-junit" in call.args[0] for call in execute.call_args_list))
        for phase in ("build", "test"):
            with self.subTest(phase=phase):
                self.assertEqual(r._diagnostic_execution_options(resolved, real, 2, phase=phase), options)
                with self.assertRaises(r.RealizationError):
                    r._diagnostic_execution_options(resolved, real, 2, 10, phase=phase)
        # Matching forged argv/options at legacy one still fail authored checks.
        bad = copy.deepcopy(result["backend_receipt"])
        bad["execution_options"].update(build_parallel=1, test_parallel=1)
        for command in bad["commands"]:
            if command and command[0] in ("cmake", "ctest"):
                command[command.index("--parallel") + 1] = "1"
        with self.assertRaisesRegex(ValueError, "execution options differ"):
            subset.validate_receipt(result, bad, resolved["baseline_exclusions"], self.contract,
                                    self.plan, self.plan["lanes"][0], self.profile)
        plan_file = self.build / "shared-parallel-plan.json"
        s.write_json(plan_file, self.plan)
        args = adapter.parser().parse_args(["run", "--repo", str(self.root), "--profile", self.pid,
            "--plan", str(plan_file), "--build-dir", str(self.build), "--parallel", "2",
            "--evidence-dir", str(self.evidence / "worker"), "--phase", "all"])
        cli_result, code = adapter.realization_command(args, self.inventory, r)
        self.assertEqual(code, 0, cli_result)
        self.assertEqual(cli_result["backend_receipt"]["execution_options"], options)
        self.assertTrue(s.aggregate(self.inventory, self.plan, [cli_result], complete=False)["passed"])
        self.assertFalse(cli_result["complete_gate_eligible"])

    def test_only_exact_33_unclaimed_findings_removed_full_remainder_unchanged(self):
        real = self.configure(admit=False)
        before = r.check_inputs(self.inventory, real)
        r._command(["cmake", "--build", str(self.build)])
        before_post = r.check_inputs(self.inventory, real, after_build=True)
        self.contract = self.review_contract(real)
        self.inventory["diagnostic_nonmember_subsets"] = {self.pid: self.contract}
        real = self.configure()
        after = r.check_inputs(self.inventory, real)
        removed = {s.canonical(g) for g in before["gaps"]} - {s.canonical(g) for g in after["gaps"]}
        self.assertEqual(removed, {s.canonical({"kind": "unclaimed_registration", "test": n}) for n in subset.REGISTRATIONS})
        self.assertFalse({s.canonical(g) for g in after["gaps"]} - {s.canonical(g) for g in before["gaps"]})
        self.assertFalse(after["passed"])
        self.assertFalse(after["complete_gate_eligible"])
        after_post = r.check_inputs(self.inventory, real, after_build=True)
        self.assertEqual({s.canonical(g) for g in before_post["gaps"]} - {s.canonical(g) for g in after_post["gaps"]}, removed)
        self.assertFalse({s.canonical(g) for g in after_post["gaps"]} - {s.canonical(g) for g in before_post["gaps"]})
        self.assertEqual(before_post["input_hashes"], after_post["input_hashes"])
        self.assertEqual(before_post["header_entries"], after_post["header_entries"])

    def test_production_records_bind_exact_x86_witness_not_current_acceptance(self):
        policy = s.load_inventory(ROOT / "tests/selection/inventory.json")
        path = "tests/selection/fixtures/ordinary_subset_x86_exports.json"
        fixture = json.loads((ROOT / path).read_text())
        self.assertIn("{source}/" + path, policy["checks"]["selection.contract"]["runtime_inputs"])
        self.assertEqual(fixture["machine"]["architecture"], "x86_64")
        self.assertEqual(fixture["machine"]["compiler_target"], "x86_64-linux-gnu")
        self.assertEqual(fixture["machine"]["uid"], 1001)
        for key in ("complete", "complete_gate_eligible", "outer_arbitrary_tree_custody", "hosted_acceptance", "w11_credit", "current_attestation"):
            self.assertIs(fixture["limits"][key], False)
        packed = fixture["candidate_contract"]
        reconstructed = {}
        for name, record in packed["registrations"].items():
            reconstructed[name] = {k: v for k, v in record.items() if k not in ("target_names", "input_names")}
            reconstructed[name]["targets"] = [packed["targets"][n] for n in record["target_names"]]
            reconstructed[name]["input_hashes"] = {n: packed["source_hashes"][n] for n in record["input_names"]}
        self.assertEqual(sorted(reconstructed), list(subset.REGISTRATIONS))
        self.assertEqual(s.digest(reconstructed), fixture["candidate_contract_hash"])
        renewal_path = "tests/selection/fixtures/ordinary_subset_source_renewal_20261022.json"
        renewal = json.loads((ROOT / renewal_path).read_text())
        self.assertIn("{source}/" + renewal_path, policy["checks"]["selection.contract"]["runtime_inputs"])
        self.assertEqual(renewal["source_review_scope"], ["apps/grotto/CMakeLists.txt", "cmake/NostrcConfigureProvenance.cmake"])
        self.assertFalse(any(renewal["limits"].values()))
        records = subset.contracts(policy)
        self.assertEqual(set(records), set(subset.PROFILES))
        for pid, contract in records.items():
            witness = fixture["profiles"][pid]
            current = renewal["profiles"][pid]
            if pid == "groundhog.linux":
                from headless_identity_projection import before_identity_family
                contract = before_identity_family(contract)
                # The later selected-family renewal adds coverage, not omissions.
                # Project only those exact arrays to inspect this older receipt.
                path = "tests/selection/fixtures/headless_model_subset_renewal_20261022.json"
                membership = json.loads((ROOT / path).read_text())
                self.assertIn("{source}/" + path, policy["checks"]["selection.contract"]["runtime_inputs"])
                contract = copy.deepcopy(contract)
                for key in ("primary_registrations", "required_closure"):
                    self.assertEqual(contract[key], sorted(membership["previous"][key] + membership["added_primary"]))
                    contract[key] = membership["previous"][key]
                self.assertEqual(s.digest(contract), membership["previous_record_hash"])
            if pid == "grotto.linux":
                from grotto_nip49_projection import before_grotto_nip49_family
                contract = before_grotto_nip49_family(contract)
            # Retain the original witness byte-for-byte; renew source provenance,
            # not its exact family/profile/candidate/fixture/toolchain authority.
            previous = dict(contract, review=current["previous_review"])
            self.assertEqual(s.digest(previous), witness["record_hash"])
            self.assertEqual(s.digest(contract), current["record_hash"])
            self.assertEqual(s.digest({k: v for k, v in contract.items() if k != "review"}), current["unchanged_contract_hash"])
            self.assertEqual(contract["candidate_contract_hash"], s.digest(reconstructed))
            self.assertEqual(contract["toolchain_contract_hash"], s.digest(fixture["toolchain_contracts"][pid]))
            self.assertEqual(previous["review"]["source_sha"], fixture["review_source_identity"]["sha"])
            self.assertEqual(previous["review"]["post_audit_sha256"], witness["postaudit"]["sha256"])
            self.assertEqual(previous["review"]["pre_audit_sha256"], witness["preaudit"]["sha256"])
            self.assertEqual(previous["review"]["realization_sha256"], witness["realization_file_sha256"])
            self.assertEqual(contract["review"]["source_sha"], renewal["source_identity"]["sha"])
            self.assertEqual(contract["review"]["pre_audit_sha256"], current["raw_preaudit"]["sha256"])
            self.assertEqual(contract["review"]["post_audit_sha256"], current["candidate_preaudit"]["sha256"])
            self.assertEqual(contract["review"]["realization_sha256"], current["realization_file_sha256"])
            self.assertEqual(contract["review"]["profile_fingerprint"], current["profile_fingerprint"])
            renewed = {"source_inputs", "source_sha", "reviewer", "pre_audit_sha256", "post_audit_sha256", "realization_sha256", "profile_fingerprint"}
            self.assertEqual({k: v for k, v in contract["review"].items() if k not in renewed},
                             {k: v for k, v in previous["review"].items() if k not in renewed})
            changed = current["source_input_changes"]
            self.assertEqual(set(changed), set(renewal["source_review_scope"]))
            self.assertIsNone(changed["cmake/NostrcConfigureProvenance.cmake"]["before"])
            expected_inputs = dict(previous["review"]["source_inputs"])
            for name, values in changed.items():
                self.assertEqual(expected_inputs.get(name), values["before"])
                expected_inputs[name] = values["after"]
            self.assertEqual(contract["review"]["source_inputs"], expected_inputs)
            self.assertIn("reviewed source input changed: apps/grotto/CMakeLists.txt", current["old_source_guard_refusal"])
            self.assertEqual(current["removed_findings"], [{"kind": "unclaimed_registration", "test": n} for n in subset.REGISTRATIONS])
            self.assertEqual(current["added_findings"], [])
            self.assertFalse(current["candidate_preaudit"]["passed"])
            self.assertFalse(current["complete"])
            self.assertFalse(current["complete_gate_eligible"])
            self.assertEqual(len(contract["registration_universe"]), {"groundhog.linux": 422, "grotto.linux": 461}[pid])
            self.assertEqual(witness["build_exit"], 0)
            self.assertEqual(len(witness["audit_build_targets"]), 29)
            self.assertFalse(witness["preaudit"]["passed"])
            self.assertFalse(witness["postaudit"]["passed"])
            self.assertEqual(witness["candidate_findings"], [{"kind": "unclaimed_registration", "test": n} for n in subset.REGISTRATIONS])
            for name, expected in records[pid]["review"]["source_inputs"].items():
                self.assertEqual(hashlib.sha256((ROOT / name).read_bytes()).hexdigest(), expected, name)

    def test_production_selection_savings_do_not_hide_legitimate_libgo_coverage(self):
        policy = s.load_inventory(ROOT / "tests/selection/inventory.json")
        for path in ("gnome/groundhog/src/store/gh-store-key.c", "apps/grotto/src/main.c"):
            plan = s.make_plan(policy, [change(path)], head="a" * 40)
            self.assertFalse(any(pid.startswith("libgo.") for pid in plan["expected_lane_ids"]))
            for check_id in plan["selected_checks"]:
                check = policy["checks"][check_id]
                for pid in subset.PROFILES:
                    self.assertFalse(set(subset.REGISTRATIONS) & set(check.get("ctest_tests", {}).get(pid, [])))
        plan = s.make_plan(policy, [change("libgo/src/channel.c")], head="a" * 40)
        self.assertIn("libgo.channel", plan["selected_checks"])
        self.assertIn("libgo.native_standalone", plan["selected_checks"])
        self.assertTrue(any(pid.startswith("libgo.") for pid in plan["expected_lane_ids"]))

    def test_schema_name_count_duplicates_unknown_profile_and_evidence_drift(self):
        self.configure()
        for mutation in (
            lambda c: c["registrations"].pop(), lambda c: c["registrations"].append("GoFutureTest"),
            lambda c: c["registrations"].append(c["registrations"][-1]), lambda c: c.update(schema_version=True),
            lambda c: c.update(extra="unknown"), lambda c: c.update(profile_hash="0" * 64),
            lambda c: c["review"].update(architecture="arm64"), lambda c: c["review"].pop("post_audit_sha256"),
            lambda c: c["workflow"].update(include=".*"), lambda c: c["required_closure"].append("GoChannelTest")):
            policy = copy.deepcopy(self.inventory)
            mutation(policy["diagnostic_nonmember_subsets"][self.pid])
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                subset.contracts(policy)
        for pid in ("core.linux", "groundhog.linux.sanitizers", "groundhog.linux.arm64.prepared011", "libgo.macos.asan", "unknown"):
            policy = copy.deepcopy(self.inventory)
            policy["diagnostic_nonmember_subsets"] = {pid: self.contract}
            with self.subTest(pid=pid), self.assertRaises(ValueError):
                subset.contracts(policy)

    def test_new_registration_semantic_owner_fixture_target_and_workflow_drift_refuse(self):
        real = self.configure()
        tests = {t["name"]: t for t in real["tests"]}
        descriptors = {d["name"]: d for d in real["descriptors"]}
        name = subset.REGISTRATIONS[0]
        for mutation in (
            lambda t, d, x: t.update(extra=t[name]), lambda t, d, x: t.pop(name),
            lambda t, d, x: t[name]["command"].append("--changed"),
            lambda t, d, x: t[name]["properties"].append({"name": "TIMEOUT", "value": 999}),
            lambda t, d, x: t[name]["properties"].append({"name": "FIXTURES_REQUIRED", "value": ["private"]}),
            lambda t, d, x: d["a-model"]["declared_depends"].append(name),
            lambda t, d, x: d[name].update(owner=["other"]), lambda t, d, x: d[name].update(targets=["missing"]),
            lambda t, d, x: d[name].update(source_dir=str(self.root)),
            lambda t, d, x: x["runtime_file_manifest"].update({name: [str(self.root / "vector.txt")]}),
            lambda t, d, x: x["targets"].clear(),
            lambda t, d, x: x["tool_versions"].update(ninja="unreviewed-version"),
            lambda t, d, x: x["toolchains"].clear()):
            t, d, x = copy.deepcopy(tests), copy.deepcopy(descriptors), copy.deepcopy(real)
            mutation(t, d, x)
            with self.subTest(mutation=mutation), self.assertRaises((ValueError, r.RealizationError)):
                subset.validate_graph(self.contract, x, t, d, ["a-model"])
        path = self.root / subset.PROFILES[self.pid][0]
        path.write_text(path.read_text() + "# drift\n")
        with self.assertRaisesRegex(ValueError, "source input changed"):
            subset.validate_graph(self.contract, real, tests, descriptors, ["a-model"])

    def test_target_custody_binding_and_phase_options_refuse_before_build(self):
        real = self.configure("grotto.linux")
        resolved = r.resolve_checks(self.inventory, self.plan, real, self.pid)
        for mutation in (
            lambda x: x["build_targets"].remove("subset-bin"), lambda x: x["target_ids"].clear(),
            lambda x: x["tests"].append(subset.REGISTRATIONS[0]),
            lambda x: x["baseline_exclusions"][0]["binding"].update(generation_hash="0" * 64),
            lambda x: x.pop("complete"), lambda x: x.pop("complete_gate_eligible"),
            lambda x: x["phase_context"].update(configuration="Release")):
            bad = copy.deepcopy(resolved)
            mutation(bad)
            with self.subTest(mutation=mutation), mock.patch.object(r, "_command") as command:
                # Real loader needs its own commands; direct closure validation
                # proves the edited target set refuses without invoking a build.
                with self.assertRaises((ValueError, r.RealizationError)):
                    r._validate_resolved_phase(bad, real)
                command.assert_not_called()
        with self.assertRaises(r.RealizationError):
            r._diagnostic_execution_options(resolved, real, 1, phase="build")
        with self.assertRaises(r.RealizationError):
            r._diagnostic_execution_options(resolved, real, 2, phase="test")
        with self.assertRaises(r.RealizationError):
            r._diagnostic_execution_options(resolved, real, 1, 10, phase="test")

    def test_standalone_build_accounting_and_stripped_incomplete_flags(self):
        real = self.configure("grotto.linux")
        resolved = r.resolve_checks(self.inventory, self.plan, real, self.pid)
        built = r.build_resolved(resolved, self.build)
        self.assertEqual(built["baseline_exclusions"], resolved["baseline_exclusions"])
        self.assertIs(built["complete"], False)
        self.assertIs(built["complete_gate_eligible"], False)
        self.assertEqual(built["baseline_exclusions"][0]["status"], "not_executed")
        self.assertTrue((self.build / "libgo/subset-bin").is_file())
        for field in ("complete", "complete_gate_eligible"):
            bad = copy.deepcopy(resolved)
            bad.pop(field)
            with self.subTest(field=field), mock.patch.object(r, "_execute", wraps=r._execute) as execute:
                with self.assertRaisesRegex(r.RealizationError, "incomplete flags"):
                    r.build_resolved(bad, self.build)
                self.assertFalse(any(call.args[0][:2] == ["cmake", "--build"] for call in execute.call_args_list))

    def test_cli_test_only_refuses_before_ctest_without_changing_other_phases(self):
        _, _, _, _ = self.execute("grotto.linux")
        plan_file = self.build / "phase-plan.json"
        s.write_json(plan_file, self.plan)
        def args(phase):
            return adapter.parser().parse_args(["run", "--repo", str(self.root), "--profile", self.pid,
                "--plan", str(plan_file), "--build-dir", str(self.build),
                "--evidence-dir", str(self.evidence / ("phase-" + phase)), "--phase", phase])
        with mock.patch.object(r, "run_resolved", wraps=r.run_resolved) as run, mock.patch.object(r, "_execute", wraps=r._execute) as execute:
            failure, code = adapter.realization_command(args("test"), self.inventory, r, retain_lane_failures=True)
            self.assertEqual(code, 1)
            self.assertEqual(failure["receipt_kind"], "lane_failure_v1")
            self.assertEqual(failure["failure"]["stage"], "capability")
            self.assertEqual(failure["failure"]["input_audits"], [])
            self.assertIn("test-only phase is unsupported", " ".join(e["message"] for e in failure["failure"]["errors"]))
            self.assertEqual(failure["failure"]["work_status"]["test"], "not_executed")
            run.assert_not_called()
            self.assertFalse(any("--output-junit" in call.args[0] for call in execute.call_args_list))
        for phase in ("configure", "build"):
            result, code = adapter.realization_command(args(phase), self.inventory, r)
            self.assertEqual(code, 0, result)
            self.assertIs(result["complete"], False)
            self.assertIs(result["complete_gate_eligible"], False)
            self.assertEqual(result["resolved"]["baseline_exclusions"][0]["status"], "not_executed")

    def test_active_production_policy_cannot_promote_ordinary_evidence(self):
        policy = s.load_inventory(ROOT / "tests/selection/inventory.json")
        release = s.release_plan(ROOT, policy, head="HEAD", component="libgo", dry_run=True, run_id="ordinary-production-negative")
        self.assertEqual(release["diagnostic_nonmember_subset_profiles"], sorted(subset.PROFILES))
        validation = dict(passed=True, complete=True, mode="broad", partial_profiles=[], tag=release["tag"],
                          **{k: release[k] for k in ("source_identity", "tested_sha", "inventory_hash")})
        jobs = {job: "success" for job in release["required_package_jobs"]}
        for pid in subset.PROFILES:
            # A separately complete summary cannot launder a nested ordinary
            # receipt even when its omission records and flags are stripped.
            bad = dict(validation, evidence=[{"profile": pid, "complete": True, "passed": True}])
            result = s.aggregate_release(release, bad, jobs)
            self.assertFalse(result["passed"])
            self.assertIn("ordinary subset receipt cannot authorize complete release validation", result["errors"])

    def test_receipt_nested_stripped_malformed_cross_profile_and_release_promotion_refuse(self):
        _, resolved, _, result = self.execute("grotto.linux")
        for mutation in (
            lambda x: x.pop("baseline_exclusions"), lambda x: x.update(complete=True),
            lambda x: x["backend_receipt"]["baseline_exclusions"][0].update(status="passed"),
            lambda x: x["backend_receipt"]["baseline_exclusions"][0].update(authority="reviewed_diagnostic_nonmember_subset_v2"),
            lambda x: x["baseline_exclusions"][0].update(profile="groundhog.linux"),
            lambda x: x["backend_receipt"]["observed_executions"].update(GoChannelTest="passed"),
            lambda x: x["backend_receipt"]["build_targets"].remove("subset-bin"),
            lambda x: x["backend_receipt"]["commands"][-1].extend(["--parallel", "99"])):
            bad = copy.deepcopy(result)
            mutation(bad)
            with self.subTest(mutation=mutation):
                self.assertFalse(s.aggregate(self.inventory, self.plan, [bad], complete=False)["passed"])
        raw = copy.deepcopy(result["backend_receipt"])
        raw["baseline_exclusions"] = []
        with self.assertRaises(ValueError):
            adapter.normalize_receipt(self.inventory, self.plan, resolved, raw)
        release = {"source_identity": self.plan["source_identity"], "tested_sha": self.plan["tested_sha"],
                   "inventory_hash": self.plan["inventory_hash"], "required_package_jobs": [], "publish": True}
        omission = result["baseline_exclusions"][0]
        for payload in (result, omission, {k: v for k, v in omission.items() if k not in ("id", "authority", "status")},
                        {"authority": "reviewed_diagnostic_nonmember_subset_v999"}):
            validation = dict(passed=True, complete=True, mode="broad", partial_profiles=[], tag=None,
                              **{k: release[k] for k in ("source_identity", "tested_sha", "inventory_hash")},
                              attempts=[{"unrelated": payload}])
            self.assertFalse(s.aggregate_release(release, validation, {})["passed"])
        # Independent broad validation keeps its existing release route; it
        # cannot be pasted over an embedded ordinary receipt missing omissions.
        validation.pop("attempts")
        guarded_release = dict(release, diagnostic_nonmember_subset_profiles=[self.pid])
        self.assertTrue(s.aggregate_release(guarded_release, validation, {})["passed"])
        for wrapper in ({"profile": self.pid}, {"profile_id": self.pid},
                        {"attempts": [{"profile": self.pid, "complete": True, "passed": True}]}):
            bad = dict(validation, embedded=wrapper)
            self.assertFalse(s.aggregate_release(guarded_release, bad, {})["passed"])

    def test_retained_source_ninja_header_runtime_and_audit_bytes_refuse_execution(self):
        real, resolved, audit, result = self.execute()
        for filename in (self.root / "libgo/omitted.c", self.root / "libgo/omitted.h",
                         self.root / "libgo/vector.bin", self.build / "build.ninja"):
            original = filename.read_bytes()
            filename.write_bytes(original + b"\n")
            try:
                with self.subTest(filename=filename), mock.patch.object(r, "_execute") as execute:
                    with self.assertRaises(r.RealizationError):
                        r.run_resolved(resolved, self.build, self.evidence / filename.name, input_audit=audit, build=False)
                    execute.assert_not_called()
            finally:
                filename.write_bytes(original)
        # Restoring bytes advances mtimes; establish a new real build/audit
        # before the independent retained-header-index tampering case.
        r.build_resolved(resolved, self.build)
        audit = r.check_inputs(self.inventory, real, after_build=True)
        self.assertTrue(audit["passed"], audit["gaps"])
        bad_audit = copy.deepcopy(audit)
        bad_audit["header_index_hash"] = "0" * 64
        bad_audit["audit_hash"] = s.digest({k: v for k, v in bad_audit.items() if k != "audit_hash"})
        with mock.patch.object(r, "_execute", wraps=r._execute) as execute:
            with self.assertRaisesRegex(r.RealizationError, "header index changed"):
                r.run_resolved(resolved, self.build, self.evidence / "audit-drift", input_audit=bad_audit, build=False)
            self.assertFalse(any("--output-junit" in call.args[0] for call in execute.call_args_list))

    def test_omitted_header_runtime_and_semantic_audits_still_block(self):
        real = self.configure()
        r._command(["cmake", "--build", str(self.build), "--target", "shared"])
        audit = r.check_inputs(self.inventory, real, after_build=True)
        self.assertTrue(any(g["kind"] == "missing_discovered_header_index" for g in audit["gaps"]))
        r._command(["cmake", "--build", str(self.build)])
        self.inventory["path_rules"].append({"exact": "libgo/omitted.h", "nodes": ["other"], "effect": "impact", "reason": "adversarial ownership"})
        real = self.configure()
        audit = r.check_inputs(self.inventory, real, after_build=True)
        self.assertTrue(any(g["kind"] == "missing_impact_edge" and g.get("test") in subset.REGISTRATIONS for g in audit["gaps"]))
        self.assertFalse(audit["passed"])
