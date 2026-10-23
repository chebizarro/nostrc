"""Finite ordinary full-membership regressions, not native/hosted acceptance.

Miniature CMake tests adapt native admission and the pinned graph fixture ONLY;
production full_marker, unique membership and live validation stay unchanged.
The separate physical Go33 receipt uses the actual repository and Linux GCC.
"""
import copy
import importlib.util
import hashlib
import json
import platform
import unittest
from unittest import mock

from realization_support import Project, ROOT
from selector_support import s, change
import test_selection_realization as r
import test_selection_subset as subset


class FullDeclarationTests(unittest.TestCase):
    def setUp(self):
        self.policy = json.loads((ROOT / "tests/selection/inventory.json").read_bytes())

    def test_exact_membership_and_marker_not_diagnostic_authority(self):
        self.assertEqual(subset.full_profiles(self.policy), set(subset.PROFILES))
        self.assertEqual(subset.active_contracts(self.policy), {})
        self.assertEqual(set(self.policy["diagnostic_nonmember_subsets"]), set(subset.PROFILES))
        self.assertFalse(subset.nested(subset.full_marker()))

    def test_missing_forged_partial_duplicate_or_aliased_declaration_refuses(self):
        mutations = [
            lambda p: p["checks"]["supported.libgo"].pop(subset.FULL_FIELD),
            lambda p: p["checks"]["supported.libgo"][subset.FULL_FIELD].update(fixture_sha256="0" * 64),
            lambda p: p["checks"]["supported.libgo"][subset.FULL_FIELD].update(schema_version=True),
            lambda p: p["checks"]["supported.libgo"]["ctest_tests"]["groundhog.linux"].pop(),
            lambda p: p["checks"]["supported.libgo"]["ctest_tests"]["groundhog.linux"].append("GoChannelTest"),
            lambda p: p["profiles"]["groundhog.linux"].update(architecture="arm64"),
            lambda p: p["profiles"]["grotto.linux"].update(instrumentation="asan_ubsan"),
            lambda p: p["checks"]["supported.libgo"].update(runtime_inputs=["invented-vector"]),
            lambda p: p["checks"].update(forged={"kind": "ctest", "profiles": ["groundhog.linux"],
                "ctest_tests": {"groundhog.linux": ["GoChannelTest"]}}),
        ]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                policy = copy.deepcopy(self.policy)
                mutate(policy)
                with self.assertRaises(ValueError):
                    subset.active_contracts(policy)

    def test_full_marker_removed_with_memberships_only_returns_incomplete_old_route(self):
        check = self.policy["checks"]["supported.libgo"]
        check.pop(subset.FULL_FIELD)
        for pid in subset.PROFILES:
            check["profiles"].remove(pid)
            del check["ctest_tests"][pid]
        self.assertEqual(set(subset.active_contracts(self.policy)), set(subset.PROFILES))
        self.assertTrue(subset.nested({"diagnostic_nonmember_subset_profiles": list(subset.PROFILES)}))

    def test_finite_reverse_projection_preserves_all_other_sections(self):
        before = subset.before_full_ownership_policy(self.policy)
        self.assertEqual(subset.full_profiles(before), set())
        self.assertEqual(subset.before_full_ownership_policy(before), before)
        restored = copy.deepcopy(before)
        restored["checks"]["supported.libgo"] = self.policy["checks"]["supported.libgo"]
        self.assertEqual(restored, self.policy)
        self.assertEqual(set(subset.contracts(before)), set(subset.PROFILES))
        bad = copy.deepcopy(self.policy)
        bad["checks"]["supported.libgo"][subset.FULL_FIELD]["authority"] = "forged"
        with self.assertRaises(ValueError):
            subset.before_full_ownership_policy(bad)

    def test_pinned_fixture_w4_semantics_sources_and_old_witness_unchanged(self):
        fixture = subset._full_fixture(ROOT)
        self.assertEqual(sorted(fixture["registrations"]), list(subset.REGISTRATIONS))
        w4 = {n for n, rec in fixture["registrations"].items()
              if rec["properties"].get("ENVIRONMENT") == ["GOF_NWORKERS=4"]}
        self.assertEqual(w4, {n for n in subset.REGISTRATIONS if n.endswith("W4")})
        self.assertEqual(len(w4), 6)
        for name, expected in fixture["source_inputs"].items():
            self.assertEqual(hashlib.sha256((ROOT / name).read_bytes()).hexdigest(), expected, name)
        for name in subset.REGISTRATIONS:
            rec = fixture["registrations"][name]
            self.assertEqual(rec["runtime_inputs"], [])
            self.assertEqual(rec["descriptor"]["declared_depends"], [])
            self.assertEqual(rec["descriptor"]["owner"], ["libgo"])
            if name.endswith("W4"):
                self.assertEqual(rec["command"], fixture["registrations"][name[:-2]]["command"])
                self.assertGreater(rec["properties"]["TIMEOUT"], 0)

    def test_full_contract_does_not_rebind_historical_current_profile_hashes(self):
        self.policy["profiles"]["groundhog.linux"]["capabilities"].append("future-tool")
        with self.assertRaisesRegex(ValueError, "stale profile"):
            subset.contracts(self.policy)
        self.assertEqual(subset.active_contracts(self.policy), {})

    def test_missing_or_nested_old_receipt_cannot_forge_full_promotion(self):
        for evidence in ({}, {"complete": False}, {"authority": subset.AUTHORITY},
                         {"nested": {"id": subset.IDENT}},
                         {subset.FULL_FIELD: subset.full_marker()}):
            with self.subTest(evidence=evidence), self.assertRaises(ValueError):
                subset.validate_full_receipt(evidence, evidence, {}, {"profile": "groundhog.linux"}, {}, checks={})


class FullGraphTests(Project):
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
                            instrumentation="none", feature_variants=[], setup="synthetic-full-only",
                            local_availability="host", parallelism=1, build_parallelism=1, test_parallelism=1,
                            timeout_seconds=60, capabilities=[])
        self.profile["environment"]["TEST_DISPLAY"] = "1"
        self.profile["configure_args"] += ["-DCMAKE_BUILD_TYPE=Debug"]
        actual = subset._full_fixture(ROOT)
        executables = sorted({t for rec in actual["registrations"].values() for t in rec["descriptor"]["targets"]})
        libgo = self.root / "libgo"
        libgo.mkdir()
        (libgo / "runner.c").write_text('#include "runner.h"\n#include <stdlib.h>\nint main(void) { const char *w = getenv("GOF_NWORKERS"); return w && w[0] != \'4\' ? 1 : VALUE; }\n')
        (libgo / "runner.h").write_text("#define VALUE 0\n")
        lines = [f"add_executable({name} runner.c)" for name in executables]
        for name, rec in actual["registrations"].items():
            target = rec["descriptor"]["targets"][0]
            lines += [f"add_test(NAME {name} COMMAND {target})",
                      f"nostrc_describe_test({name} OWNER libgo TARGETS {target})"]
            if name.endswith("W4"):
                lines += [f'set_tests_properties({name} PROPERTIES ENVIRONMENT "GOF_NWORKERS=4" TIMEOUT 10)']
        (libgo / "CMakeLists.txt").write_text("\n".join(lines) + "\n")
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("nostrc_finalize_test_inventory()",
                         "add_subdirectory(libgo)\nnostrc_finalize_test_inventory()"))
        self.inventory["nodes"]["libgo"] = {}
        self.inventory["path_rules"].append({"prefix": "libgo/", "nodes": ["libgo"], "effect": "impact", "reason": "fixture"})
        self.inventory["profiles"] = {pid: copy.deepcopy(self.profile) for pid in subset.PROFILES}
        for check in self.inventory["checks"].values():
            name = next(iter(check["ctest_tests"].values()))
            check.update(profiles=list(subset.PROFILES), ctest_tests={pid: name for pid in subset.PROFILES},
                         requirement="blocking", expected_outcome="pass", cost="ordinary")
        self.inventory['checks'] = {'fixture.' + name.replace('-', '_'): check
                                    for name, check in self.inventory['checks'].items()}
        self.inventory["checks"]["supported.libgo"] = {
            "kind": "ctest", "profiles": sorted(subset.PROFILES), "nodes": ["libgo"], "cost": "ordinary",
            "requirement": "blocking", "expected_outcome": "pass", "runtime_inputs": [],
            "build_targets": executables, "ctest_tests": {pid: list(subset.REGISTRATIONS) for pid in subset.PROFILES},
            subset.FULL_FIELD: subset.full_marker()}
        self.inventory["checks"]["ordinary.default"] = {
            "kind": "build_contract", "profiles": sorted(subset.PROFILES), "nodes": ["application", "libgo", "testing"],
            "requirement": "blocking", "expected_outcome": "pass", "cost": "ordinary", "build_scope": "default",
            "build_targets": [], "command": ["cmake", "--build", "{build}", "--config", "Debug"],
            "working_directory": "{source}", "environment": {}, "runtime_inputs": []}
        for name, node in self.inventory['nodes'].items():
            node.update(owner_paths=[name], rationale='synthetic dependency')
        for rule in self.inventory['path_rules']:
            rule.setdefault('effect', 'impact')
            rule.setdefault('reason', 'synthetic ownership')
        self.inventory.update(schema_version=1, release_components={})
        self.refresh_identity()
        self.pid = "groundhog.linux"
        self.plan.update(selected_checks=list(self.inventory["checks"]), inventory_hash=s.digest(self.inventory))
        self.real = r.configure_profile(self.root, self.build, self.pid, self.profile,
                                        self.identity, s.digest(self.inventory), inventory=self.inventory)
        self.tests, self.descriptors = r._metadata(self.inventory, self.real)
        candidate = subset.candidate_contract(self.real, self.tests, self.descriptors)
        fixture = {"registrations": {}, "source_inputs": {"libgo/runner.h": hashlib.sha256((libgo / "runner.h").read_bytes()).hexdigest()}}
        for name, record in candidate.items():
            fixture["registrations"][name] = {k: record[k] for k in ("command", "properties", "descriptor", "runtime_inputs")}
            fixture["registrations"][name].update(input_names=sorted(record["input_hashes"]),
                                                   target_names=sorted(t["name"] for t in record["targets"]))
        fixture["target_graph"] = subset._full_target_structure(candidate)
        self.graph_fixture = fixture
        mock.patch.object(subset, "_full_fixture", return_value=fixture).start()

    def test_live_graph_and_pre_post_full_audits_without_any_exclusion(self):
        full = subset.full_evidence(self.inventory, self.real, self.tests, self.descriptors)
        self.assertEqual(full["executions"], list(subset.REGISTRATIONS))
        self.assertFalse(subset.nested(full))
        self.assertEqual(subset.admit(self.inventory, self.real, self.tests, self.descriptors), [])
        before = r.check_inputs(self.inventory, self.real)
        self.assertTrue(before["passed"], before["gaps"])
        resolved = r.resolve_checks(self.inventory, self.plan, self.real, self.pid)
        r.build_resolved(resolved, self.build)
        after = r.check_inputs(self.inventory, self.real, after_build=True)
        self.assertTrue(after["passed"], after["gaps"])
        # This existing flag means full audited selector readiness, not an
        # audit restricted to selected tests. The historical subset stays false.
        self.assertTrue(after["selective_enforcement"])
        self.assertEqual(after["baseline_exclusions"], [])

    def test_real_full_default_build_ctest_receipt_and_adversarial_promotion(self):
        full = subset.full_evidence(self.inventory, self.real, self.tests, self.descriptors)
        lane = dict(id='fixture-full-go', profile=self.pid, profile_hash=s.digest(self.profile),
                    checks=list(self.inventory['checks']))
        self.plan.update(plan_id='fixture-full-go', lanes=[lane])
        resolved = r.resolve_checks(self.inventory, self.plan, self.real, self.pid)
        before = r.check_inputs(self.inventory, self.real)
        self.assertTrue(before["passed"], before["gaps"])
        self.assertEqual(resolved[subset.FULL_FIELD], full)
        self.assertEqual(before[subset.FULL_FIELD], full)
        r.build_resolved(resolved, self.build)
        after = r.check_inputs(self.inventory, self.real, after_build=True)
        self.assertTrue(after["passed"], after["gaps"])
        self.assertEqual(after[subset.FULL_FIELD], full)
        raw = r.run_resolved(resolved, self.build, self.evidence, input_audit=after,
                             input_audit_before_build=before, build=False)
        self.assertEqual(raw["outcome"], "passed", raw)
        self.assertEqual(raw[subset.FULL_FIELD], full)
        self.assertEqual(raw["input_audit_before_build"], before)
        spec = importlib.util.spec_from_file_location('full_go_adapter', ROOT / 'scripts/test-plan.py')
        adapter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(adapter)
        normalized = adapter.normalize_receipt(self.inventory, self.plan, resolved, raw)
        self.assertEqual(normalized[subset.FULL_FIELD], full)
        for key in s.COVERAGE_STATE_FIELDS:
            self.assertEqual(normalized[key], resolved[key])
            self.assertEqual(raw[key], before[key])
            self.assertEqual(raw[key], after[key])
        subset.validate_full_receipt(raw, raw, self.plan, lane, self.profile, checks=self.inventory["checks"])
        mutations = [
            lambda value: value.pop(subset.FULL_FIELD),
            lambda value: value.pop("input_audit_before_build"),
            lambda value: value["input_audit"][subset.FULL_FIELD].update(graph_sha256="0" * 64),
            lambda value: value["input_audit"].update(header_index_hash=None),
            lambda value: value.update(target_ids=[]),
            lambda value: value.update(build_targets=[]),
            lambda value: value["observed_executions"].update(GoFiberBasicTestW4="skipped"),
            lambda value: value.update(ctest_exit_code=8),
            lambda value: value.update(baseline_exclusions=[{"authority": subset.AUTHORITY}]),
            lambda value: value.update(nested={"authority": subset.AUTHORITY, "complete": False}),
            lambda value: value.update(nested={"candidate_contract_hash": "a" * 64}),
            lambda value: value.update(nested={"diagnostic_nonmember_subset_profiles": []}),
        ]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                value = copy.deepcopy(raw)
                mutate(value)
                with self.assertRaises((ValueError, KeyError, TypeError)):
                    subset.validate_full_receipt(value, value, self.plan, lane, self.profile,
                                                 checks=self.inventory["checks"])
        outer, backend = copy.deepcopy(raw), copy.deepcopy(raw)
        backend["hidden"] = {"authority": subset.AUTHORITY, "complete": False}
        with self.assertRaisesRegex(ValueError, "historical partial authority"):
            subset.validate_full_receipt(outer, backend, self.plan, lane, self.profile,
                                         checks=self.inventory["checks"])

    def test_unselected_go_keeps_full_graph_audits_without_broadening_execution(self):
        selected = [ident for ident in self.inventory['checks'] if ident != 'supported.libgo']
        lane = dict(id='fixture-no-go', profile=self.pid, profile_hash=s.digest(self.profile), checks=selected)
        self.plan.update(plan_id='fixture-no-go', selected_checks=selected, lanes=[lane])
        resolved = r.resolve_checks(self.inventory, self.plan, self.real, self.pid)
        self.assertNotIn('supported.libgo', resolved['checks'])
        self.assertFalse(set(subset.REGISTRATIONS) & set(resolved['tests']))
        pre = r.check_inputs(self.inventory, self.real)
        r.build_resolved(resolved, self.build)
        post = r.check_inputs(self.inventory, self.real, after_build=True)
        raw = r.run_resolved(resolved, self.build, self.evidence, input_audit=post,
                             input_audit_before_build=pre, build=False)
        self.assertEqual(raw['outcome'], 'passed', raw)
        self.assertNotIn('supported.libgo', raw['checks'])
        self.assertFalse(set(subset.REGISTRATIONS) & set(raw['observed_executions']))
        spec = importlib.util.spec_from_file_location('no_go_adapter', ROOT / 'scripts/test-plan.py')
        adapter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(adapter)
        normalized = adapter.normalize_receipt(self.inventory, self.plan, resolved, raw)
        self.assertEqual(normalized[subset.FULL_FIELD], resolved[subset.FULL_FIELD])
        self.assertEqual(set(normalized['checks']), set(selected))

    def test_test_only_cli_cannot_synthesize_original_prebuild_audit(self):
        plan = s.make_plan(self.inventory, [change('libgo/runner.c')], head=self.identity['sha'],
                           base='b' * 40, source_identity=self.identity, mode='broad')
        path = self.build.parent / 'plan.json'
        path.write_text(json.dumps(plan))
        spec = importlib.util.spec_from_file_location('test_only_adapter', ROOT / 'scripts/test-plan.py')
        adapter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(adapter)
        args = adapter.parser().parse_args(['run', '--repo', str(self.root), '--plan', str(path),
            '--build-dir', str(self.build), '--profile', self.pid, '--phase', 'test', '--evidence-dir', str(self.evidence)])
        with mock.patch.object(r, 'check_inputs', wraps=r.check_inputs) as audit:
            with self.assertRaisesRegex(s.CheckFailure, 'full ownership test-only phase is unsupported'):
                adapter.realization_command(args, self.inventory, r)
            audit.assert_not_called()

    def test_missing_or_forged_retained_full_audit_refuses_before_target_execution(self):
        resolved = r.resolve_checks(self.inventory, self.plan, self.real, self.pid)
        before = r.check_inputs(self.inventory, self.real)
        r.build_resolved(resolved, self.build)
        after = r.check_inputs(self.inventory, self.real, after_build=True)
        cases = []
        cases.append((copy.deepcopy(resolved), None, copy.deepcopy(after)))
        for key in s.COVERAGE_STATE_FIELDS:
            stale = copy.deepcopy(before)
            stale.pop(key)
            stale['audit_hash'] = s.digest({k: v for k, v in stale.items() if k != 'audit_hash'})
            cases.append((copy.deepcopy(resolved), stale, copy.deepcopy(after)))
        stale = copy.deepcopy(after)
        stale.pop(subset.FULL_FIELD)
        stale['audit_hash'] = s.digest({k: v for k, v in stale.items() if k != 'audit_hash'})
        cases.append((copy.deepcopy(resolved), copy.deepcopy(before), stale))
        changed = copy.deepcopy(resolved)
        changed[subset.FULL_FIELD]['graph_sha256'] = '0' * 64
        cases.append((changed, copy.deepcopy(before), copy.deepcopy(after)))
        for index, (selected, pre, post) in enumerate(cases):
            with self.subTest(index=index), mock.patch.object(r, '_execute', wraps=r._execute) as execute:
                with self.assertRaises((ValueError, KeyError, r.RealizationError)):
                    r.run_resolved(selected, self.build, self.evidence / str(index), input_audit=post,
                                   input_audit_before_build=pre, build=False)
                # Real read-only graph/tool queries are allowed; no target CTest
                # or prerequisite build may execute with invalid retained audits.
                self.assertFalse(any('--output-junit' in call.args[0] or '--build' in call.args[0]
                                     for call in execute.call_args_list))

    def test_inherited_worker_override_refuses_even_with_unchanged_ctest_graph(self):
        env = r.phase_environment(self.profile, self.root, "test")
        env["GOF_NWORKERS"] = "4"
        with mock.patch.object(r, "phase_environment", return_value=env):
            with self.assertRaisesRegex((ValueError, r.RealizationError),
                                        "default worker environment|effective phase environment"):
                subset.full_evidence(self.inventory, self.real, self.tests, self.descriptors)

    def test_forged_descriptor_or_test_map_refuses(self):
        descriptors = copy.deepcopy(self.descriptors)
        descriptors["GoChannelTest"]["owner"] = ["other"]
        with self.assertRaisesRegex(ValueError, "metadata authority"):
            subset.full_evidence(self.inventory, self.real, self.tests, descriptors)
        tests = copy.deepcopy(self.tests)
        tests["GoFiberBasicTestW4"]["command"] = ["/bin/true"]
        with self.assertRaisesRegex(ValueError, "metadata authority"):
            subset.full_evidence(self.inventory, self.real, tests, self.descriptors)

    def test_changed_source_fixture_env_timeout_and_required_closure_refuse(self):
        # Simulate a newly configured, internally self-consistent graph that still
        # differs from the independently frozen full contract.
        mutations = [
            lambda real: next(t for t in real["tests"] if t["name"] == "GoFiberBasicTestW4")["properties"].append({"name": "ENVIRONMENT", "value": ["GOF_NWORKERS=1"]}),
            lambda real: next(t for t in real["tests"] if t["name"] == "GoFiberBasicTestW4")["properties"].append({"name": "TIMEOUT", "value": 999}),
            lambda real: next(t for t in real["tests"] if t["name"] == "GoChannelTest")["properties"].append({"name": "FIXTURES_REQUIRED", "value": ["private"]}),
            lambda real: next(d for d in real["descriptors"] if d["name"] == "GoChannelTest")["targets"].clear(),
            lambda real: next(t for t in real["targets"].values() if t["name"] == "go_channel_test")["sources"].append(
                {"path": str(self.build / "injected-generated.c"), "generated": True, "compiled": True}),
            lambda real: next(t for t in real["targets"].values() if t["name"] == "go_channel_test").update(type="STATIC_LIBRARY"),
            lambda real: next(t for t in real["targets"].values() if t["name"] == "go_channel_test")["dependencies"].append(
                next(i for i, t in real["targets"].items() if t["name"] == "go_channel_test")),

        ]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                real = copy.deepcopy(self.real)
                mutate(real)
                tests = {t["name"]: t for t in real["tests"]}
                desc = {d["name"]: d for d in real["descriptors"]}
                with mock.patch.object(r, "load_realization", return_value=real), mock.patch.object(r, "_command", return_value=json.dumps({"tests": real["tests"]})):
                    with self.assertRaises((ValueError, r.RealizationError)):
                        subset.full_evidence(self.inventory, real, tests, desc)
        (self.root / "libgo/runner.h").write_text("#define VALUE 1\n")
        with self.assertRaises((ValueError, r.RealizationError)):
            subset.full_evidence(self.inventory, self.real, self.tests, self.descriptors)
