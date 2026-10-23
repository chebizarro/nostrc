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

    def test_full_marker_removed_refuses_current_profile_but_retains_exact_old_route(self):
        check = self.policy["checks"]["supported.libgo"]
        check.pop(subset.FULL_FIELD)
        for pid in subset.PROFILES:
            check["profiles"].remove(pid)
            del check["ctest_tests"][pid]
        with self.assertRaisesRegex(ValueError, 'stale profile hash'):
            subset.active_contracts(self.policy)
        from runtime_integration_support import before_runtime_policy
        historical = before_runtime_policy(self.policy)
        self.assertEqual(set(subset.active_contracts(historical)), set(subset.PROFILES))
        self.assertTrue(subset.nested({"diagnostic_nonmember_subset_profiles": list(subset.PROFILES)}))

    def test_finite_reverse_projection_preserves_all_other_sections(self):
        before = subset.before_full_ownership_policy(self.policy)
        self.assertEqual(subset.full_profiles(before), set())
        self.assertEqual(subset.before_full_ownership_policy(before), before)
        restored = copy.deepcopy(before)
        restored["checks"]["supported.libgo"] = self.policy["checks"]["supported.libgo"]
        self.assertEqual(restored, self.policy)
        from runtime_integration_support import before_runtime_policy
        self.assertEqual(set(subset.contracts(before_runtime_policy(before))), set(subset.PROFILES))
        bad = copy.deepcopy(self.policy)
        bad["checks"]["supported.libgo"][subset.FULL_FIELD]["authority"] = "forged"
        with self.assertRaises(ValueError):
            subset.before_full_ownership_policy(bad)

    def test_pinned_structural_fixture_w4_semantics_and_source_provenance(self):
        fixture = subset._full_fixture(ROOT)
        self.assertEqual(sorted(fixture["registrations"]), list(subset.REGISTRATIONS))
        w4 = {n for n, rec in fixture["registrations"].items()
              if rec["properties"].get("ENVIRONMENT") == ["GOF_NWORKERS=4"]}
        self.assertEqual(w4, {n for n in subset.REGISTRATIONS if n.endswith("W4")})
        self.assertEqual(len(w4), 6)
        self.assertTrue(subset._sha(fixture["base"], 40))
        for name, historical_hash in fixture["source_inputs"].items():
            self.assertEqual(s.normalized_path(name), name)
            self.assertTrue(subset._sha(historical_hash))
            self.assertTrue((ROOT / name).is_file(), name)
        # _full_fixture verifies the immutable fixture itself, including review
        # hashes. Normal product edits need no historical hash/policy refresh.
        for name in subset.REGISTRATIONS:
            rec = fixture["registrations"][name]
            self.assertEqual(rec["runtime_inputs"], [])
            self.assertEqual(rec["descriptor"]["declared_depends"], [])
            self.assertEqual(rec["descriptor"]["owner"], ["libgo"])
            if name.endswith("W4"):
                self.assertEqual(rec["command"], fixture["registrations"][name[:-2]]["command"])
                self.assertGreater(rec["properties"]["TIMEOUT"], 0)

    def test_product_source_changes_select_full_go_and_original_consumers(self):
        before = s.digest(self.policy)
        for name in ("libgo/src/channel.c", "libgo/include/channel.h"):
            with self.subTest(name=name):
                self.assertTrue((ROOT / name).is_file())
                plan = s.make_plan(self.policy, [change(name)], head="a" * 40, base="b" * 40)
                self.assertEqual(plan["effective_mode"], "affected")
                self.assertEqual(plan["source_identity"], {"kind": "committed", "sha": "a" * 40})
                for pid, default in (("groundhog.linux", "groundhog.default_build"),
                                     ("grotto.linux", "grotto.default_build")):
                    lane = next(lane for lane in plan["lanes"] if lane["profile"] == pid)
                    self.assertTrue({"supported.libgo", "supported.libnostr", "supported.nips.nip04", default}
                                    <= set(lane["checks"]))
                    self.assertEqual(self.policy["checks"]["supported.libgo"]["ctest_tests"][pid],
                                     list(subset.REGISTRATIONS))
                self.assertFalse(subset.nested(plan))
        self.assertEqual(s.digest(self.policy), before)

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
        (libgo / "runner.c").write_text('#include "runner.h"\n#include <stdlib.h>\nint main(void) { const char *w = getenv("GOF_NWORKERS"); return w && w[0] != \'4\' ? 1 : go_value(); }\n')
        (libgo / "runner.h").write_text("#define VALUE 0\nint go_value(void);\n")
        (libgo / "value.c").write_text('#include "runner.h"\nint go_value(void) { return VALUE; }\n')
        lines = ["add_library(go_fixture STATIC value.c)"]
        for name in executables:
            lines += [f"add_executable({name} runner.c)", f"target_link_libraries({name} PRIVATE go_fixture)"]
        for name, rec in actual["registrations"].items():
            target = rec["descriptor"]["targets"][0]
            lines += [f"add_test(NAME {name} COMMAND {target})",
                      f"nostrc_describe_test({name} OWNER libgo TARGETS {target})"]
            if name.endswith("W4"):
                lines += [f'set_tests_properties({name} PROPERTIES ENVIRONMENT "GOF_NWORKERS=4" TIMEOUT 10)']
        (libgo / "CMakeLists.txt").write_text("\n".join(lines) + "\n")
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("nostrc_finalize_test_inventory()",
                         "add_subdirectory(libgo)\ntarget_link_libraries(shared PRIVATE go_fixture)\nnostrc_finalize_test_inventory()"))
        consumer = self.root / "main.c"
        consumer.write_text(consumer.read_text().replace('#include "shared.h"',
                            '#include "shared.h"\n#include "libgo/runner.h"').replace(
                            'int main(int argc, char **argv) {',
                            'int main(int argc, char **argv) {\n  if (go_value() != 0) return 1;'))
        self.inventory["nodes"]["libgo"] = {}
        self.inventory["nodes"]["application"]["depends_on"].append("libgo")
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
        fixture = {"registrations": {}, "source_inputs": {
            name: hashlib.sha256((self.root / name).read_bytes()).hexdigest()
            for name in ("libgo/runner.h", "libgo/value.c", "libgo/CMakeLists.txt")}}
        for name, record in candidate.items():
            fixture["registrations"][name] = {k: record[k] for k in ("command", "properties", "descriptor", "runtime_inputs")}
            fixture["registrations"][name].update(input_names=sorted(record["input_hashes"]),
                                                   target_names=sorted(t["name"] for t in record["targets"]))
        fixture["target_graph"] = subset._full_target_structure(candidate)
        self.graph_fixture = fixture
        mock.patch.object(subset, "_full_fixture", return_value=fixture).start()

    def test_new_committed_product_bytes_run_full_go_and_linked_consumers_without_policy_refresh(self):
        # The only adapted interfaces are native host admission and the small
        # structural graph fixture in setUp; source Git/graphs/audits/build/CTest
        # and normalization execute the production engine without bypasses.
        policy_hash = s.digest(self.inventory)
        fixture_before = copy.deepcopy(self.graph_fixture)
        policy_path = self.root / "tests/selection/inventory.json"
        policy_path.parent.mkdir(parents=True)
        policy_path.write_text(json.dumps(self.inventory))
        self.git("add", "-A")
        self.git("-c", "core.hooksPath=/dev/null", "commit", "-qm", "full normal fixture baseline")
        old_identity = {"kind": "committed", "sha": self.git("rev-parse", "HEAD").decode().strip()}
        old_real = r.configure_profile(self.root, self.build, self.pid, self.profile,
                                      old_identity, policy_hash, inventory=self.inventory)
        old_tests, old_desc = r._metadata(self.inventory, old_real)
        old_full = subset.full_evidence(self.inventory, old_real, old_tests, old_desc)
        product_edits = {
            "libgo/value.c": '#include "runner.h"\nint go_value(void) { return (VALUE + 0); }\n',
            "libgo/runner.h": "#define VALUE (0)\nint go_value(void);\n",
        }
        for name, content in product_edits.items():
            (self.root / name).write_text(content)
            self.assertNotEqual(hashlib.sha256(content.encode()).hexdigest(),
                                fixture_before["source_inputs"][name])
        self.git("add", "-A")
        self.git("-c", "core.hooksPath=/dev/null", "commit", "-qm", "ordinary libgo implementation and header edit")
        identity = {"kind": "committed", "sha": self.git("rev-parse", "HEAD").decode().strip()}
        self.assertNotEqual(identity, old_identity)
        self.assertEqual(set(self.git("diff", "--name-only", old_identity["sha"], identity["sha"]).decode().splitlines()),
                         set(product_edits))
        plan = s.git_plan(self.root, self.inventory, head=identity["sha"], base=old_identity["sha"])
        self.assertEqual(plan["source_identity"], identity)
        self.assertEqual(plan["effective_mode"], "affected")
        self.assertEqual(plan["broadening_reasons"], [])
        self.assertEqual(set(plan["selected_checks"]), set(self.inventory["checks"]))
        self.assertIn("application", plan["witnesses"])
        self.assertEqual(s.digest(self.inventory), policy_hash)
        self.assertEqual(self.graph_fixture, fixture_before)
        spec = importlib.util.spec_from_file_location("fresh_source_adapter", ROOT / "scripts/test-plan.py")
        adapter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(adapter)
        for pid in sorted(subset.PROFILES):
            with self.subTest(profile=pid):
                build = self.build if pid == self.pid else self.build.parent / pid
                real = r.configure_profile(self.root, build, pid, self.inventory["profiles"][pid],
                                           identity, policy_hash, inventory=self.inventory)
                tests, descriptors = r._metadata(self.inventory, real)
                full = subset.full_evidence(self.inventory, real, tests, descriptors)
                self.assertEqual(full["binding"]["source_identity"], identity)
                self.assertNotEqual(full["graph_sha256"], old_full["graph_sha256"])
                self.assertEqual(full["fixture_sha256"], old_full["fixture_sha256"])
                lane = next(lane for lane in plan["lanes"] if lane["profile"] == pid)
                resolved = r.resolve_checks(self.inventory, plan, real, pid)
                pre = r.check_inputs(self.inventory, real)
                self.assertTrue(pre["passed"], pre["gaps"])
                r.build_resolved(resolved, build)
                post = r.check_inputs(self.inventory, real, after_build=True)
                self.assertTrue(post["passed"], post["gaps"])
                self.assertTrue(post["header_index_hash"])
                self.assertGreater(post["header_entries"], 0)
                for name, content in product_edits.items():
                    self.assertEqual(post["input_hashes"][str(self.root / name)],
                                     hashlib.sha256(content.encode()).hexdigest())
                self.assertEqual(pre["input_hashes"][str(self.root / "libgo/value.c")],
                                 post["input_hashes"][str(self.root / "libgo/value.c")])
                raw = r.run_resolved(resolved, build, self.evidence / pid, input_audit=post,
                                     input_audit_before_build=pre, build=False)
                self.assertEqual(raw["outcome"], "passed", raw)
                self.assertEqual(raw["checks"]["supported.libgo"]["observed_executions"], list(subset.REGISTRATIONS))
                # shared is really linked to go_fixture and includes runner.h;
                # all three application registrations are selected and run.
                for name in ("a-model", "gui", "omitted"):
                    self.assertEqual(raw["observed_executions"][name], "passed")
                normalized = adapter.normalize_receipt(self.inventory, plan, resolved, raw)
                self.assertEqual(normalized["source_identity"], identity)
                self.assertEqual(normalized[subset.FULL_FIELD], full)
                subset.validate_full_receipt(raw, raw, plan, lane, self.profile, checks=self.inventory["checks"])
                for stale in (old_full, dict(full, binding=dict(full["binding"], source_identity=old_identity))):
                    wrong_source = copy.deepcopy(raw)
                    wrong_source[subset.FULL_FIELD] = stale
                    # The old groundhog receipt also has the wrong profile in
                    # the grotto lane; either mismatch must refuse promotion.
                    with self.assertRaisesRegex(ValueError, "full source/profile/context evidence differs|missing/forged full ownership evidence"):
                        subset.validate_full_receipt(wrong_source, wrong_source, plan, lane, self.profile,
                                                     checks=self.inventory["checks"])
                partial = copy.deepcopy(raw)
                partial["retained_partial"] = {"authority": subset.AUTHORITY, "complete": False}
                with self.assertRaisesRegex(ValueError, "historical partial authority"):
                    subset.validate_full_receipt(partial, partial, plan, lane, self.profile, checks=self.inventory["checks"])
                for name, content in product_edits.items():
                    with self.subTest(drift=name):
                        path = self.root / name
                        path.write_text(content + "/* post-resolution mutation */\n")
                        try:
                            with mock.patch.object(r, "_execute", wraps=r._execute) as execute:
                                with self.assertRaises(r.RealizationError):
                                    r.run_resolved(resolved, build, self.evidence / pid / "drift",
                                                   input_audit=post, input_audit_before_build=pre, build=False)
                                self.assertFalse(any("--output-junit" in call.args[0] or "--build" in call.args[0]
                                                     for call in execute.call_args_list))
                        finally:
                            path.write_text(content)
        self.assertEqual(s.digest(self.inventory), policy_hash)
        self.assertEqual(self.graph_fixture, fixture_before)

    def test_new_committed_source_graph_change_still_requires_authored_contract(self):
        (self.root / "libgo/new.c").write_text("int new_function(void) { return 0; }\n")
        path = self.root / "libgo/CMakeLists.txt"
        path.write_text(path.read_text() + "target_sources(go_fixture PRIVATE new.c)\n")
        self.git("add", "-A")
        self.git("-c", "core.hooksPath=/dev/null", "commit", "-qm", "unreviewed source graph change")
        identity = {"kind": "committed", "sha": self.git("rev-parse", "HEAD").decode().strip()}
        real = r.configure_profile(self.root, self.build, self.pid, self.profile, identity,
                                   s.digest(self.inventory), inventory=self.inventory)
        tests, descriptors = r._metadata(self.inventory, real)
        with self.assertRaisesRegex(ValueError, "source/prerequisite graph changed"):
            subset.full_evidence(self.inventory, real, tests, descriptors)

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
