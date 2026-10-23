"""Legacy macOS NIP-5F TCP applicability through the real selection engine.

A miniature CMake project carries the real bound sources byte-for-byte
(test_nip5f_tcp.c, nips/nip5f CMake/TestInventory, legacy nostrc-ci workflow)
and is configured with the real core.macos.asan profile, so configure, resolve,
full audits, build, CTest execution, receipt normalization and aggregation run
unmodified. Native configure requires an actual macOS/arm64/Clang host; other
hosts run the engine-level cases below. Not production profile proof.
"""
import copy
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import unittest
from unittest import mock

from selector_support import s, change
from realization_support import Project, ROOT, r

sys.path.insert(0, str(ROOT / "scripts"))
import test_selection_macos_nip5f_tcp as m  # noqa: E402

spec = importlib.util.spec_from_file_location("nip5f_applicability_adapter", ROOT / "scripts/test-plan.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)

NATIVE = platform.system() == "Darwin" and platform.machine() == "arm64"
BOUND = (m.SOURCE, m.REGISTRATION_FILE, m.SELECTION_FILE, m.LEGACY_WORKFLOW)
SIGNER = {"NOSTR_SIGNER_ENDPOINT": "tcp:127.0.0.1:9", "NOSTR_SIGNER_TOKEN": "t"}


def production():
    return json.loads((ROOT / "tests/selection/inventory.json").read_text())


@unittest.skipUnless(NATIVE, "real core.macos.asan configure requires a native macOS/arm64 Clang host")
class LegacyMacosEngineTests(Project):
    def setUp(self):
        super().setUp()
        for path in BOUND:
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, self.root / path)
        (self.root / "fixture").mkdir()
        (self.root / "fixture/json.h").write_text(
            "int nostr_json_get_string(const char *json, const char *key, char **out);\n")
        (self.root / "fixture/stubs.c").write_text(
            "#include <stddef.h>\n"
            "int nip5f_read_frame(int fd, char **j, size_t *n) { (void)fd; (void)j; (void)n; return -1; }\n"
            "int nip5f_write_frame(int fd, const char *j, size_t n) { (void)fd; (void)j; (void)n; return -1; }\n"
            "int nostr_json_get_string(const char *j, const char *k, char **o) { (void)j; (void)k; (void)o; return -1; }\n")
        (self.root / "fixture/ok.c").write_text("int main(void) { return 0; }\n")
        # Registrations live under nips/nip5f/ so CMake assigns the same directory
        # owner as production; the real nips/nip5f/CMakeLists.txt is only bound.
        (self.root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.22)
project(nip5f_applicability C)
enable_testing()
include(cmake/NostrcTestInventory.cmake)
if(GO_ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
if(GO_ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()
add_subdirectory(nips/nip5f/mini)
nostrc_finalize_test_inventory()
''')
        (self.root / "nips/nip5f/mini").mkdir()
        (self.root / "nips/nip5f/mini/CMakeLists.txt").write_text('''add_executable(test_nip5f_tcp ../tests/test_nip5f_tcp.c ${CMAKE_SOURCE_DIR}/fixture/stubs.c)
target_include_directories(test_nip5f_tcp PRIVATE ${CMAKE_SOURCE_DIR}/fixture)
add_executable(test_nip5f_loopback ${CMAKE_SOURCE_DIR}/fixture/ok.c)
add_executable(test_nip5f_negative_env ${CMAKE_SOURCE_DIR}/fixture/ok.c)
add_test(NAME test_nip5f_loopback COMMAND test_nip5f_loopback)
add_test(NAME test_nip5f_negative_env COMMAND test_nip5f_negative_env)
add_test(NAME test_nip5f_tcp COMMAND test_nip5f_tcp)
set_tests_properties(test_nip5f_tcp PROPERTIES SKIP_RETURN_CODE 77)
include(${CMAKE_CURRENT_SOURCE_DIR}/../TestInventory.cmake)
''')
        for stale in ("main.c", "shared.h", "vector.txt", "setup.cmake", "noop.cmake"):
            (self.root / stale).unlink()
        real = production()
        self.profile = copy.deepcopy(real["profiles"][m.PROFILE])
        linux = copy.deepcopy(real["profiles"]["core.linux.asan"])
        mandatory = copy.deepcopy(real["checks"][m.MANDATORY_CHECK])
        mandatory.update(nodes=["nip5f", "nip5f.test_inputs"], profiles=[m.PROFILE],
                         ctest_tests={m.PROFILE: list(m.MANDATORY)})
        signer = copy.deepcopy(real["checks"][m.CHECK])
        signer.update(nodes=["nip5f", "nip5f.test_inputs", "nip5f.fixture_inputs"], profiles=["core.linux.asan"],
                      ctest_tests={"core.linux.asan": [m.REGISTRATION]},
                      build_targets=[m.TARGET])
        gaps = [g for g in real["coverage_gaps"] if g["id"] in ("root_asan_execution_contract", m.IDENT)]
        nodes = {"nip5f": {"owner_paths": ["nips/nip5f", "fixture"], "rationale": "miniature"},
                 "nip5f.test_inputs": {"owner_paths": ["nips/nip5f/tests"], "rationale": "miniature"},
                 "nip5f.fixture_inputs": {"owner_paths": ["fixture"], "rationale": "miniature"},
                 "testing": {"owner_paths": ["cmake"], "rationale": "miniature"}}
        rules = [{"prefix": "nips/nip5f/", "nodes": ["nip5f"], "effect": "impact", "reason": "miniature"},
                 {"prefix": "fixture/", "nodes": ["nip5f"], "effect": "impact", "reason": "miniature"},
                 {"prefix": ".github/", "nodes": [], "effect": "global", "reason": "miniature"},
                 {"exact": "CMakeLists.txt", "nodes": [], "effect": "global", "reason": "miniature"},
                 {"prefix": "cmake/", "nodes": [], "effect": "global", "reason": "miniature"}]
        self.inventory = {"schema_version": 1, "nodes": nodes, "path_rules": rules, "release_components": {},
                          "profiles": {m.PROFILE: self.profile, "core.linux.asan": linux},
                          "checks": {m.MANDATORY_CHECK: mandatory, m.CHECK: signer},
                          "workflow_jobs": {m.LEGACY_JOB: copy.deepcopy(real["workflow_jobs"][m.LEGACY_JOB])},
                          "coverage_gaps": gaps, "enforcement_ready": False}

    def configure(self, profile_id=m.PROFILE):
        self.refresh_identity()
        # Only the legacy macOS lane: the signer check's Linux lane is out of scope here.
        self.plan = s.make_plan(self.inventory, [change(m.SOURCE)], head=self.identity["sha"], base="b" * 40,
                                source_identity=self.identity, mode="broad", profiles=[m.PROFILE])
        return r.configure_profile(self.root, self.build, profile_id, self.inventory["profiles"][m.PROFILE],
                                   self.identity, s.digest(self.inventory))

    def resolve(self, realized=None):
        realized = realized or self.configure()
        return realized, r.resolve_checks(self.inventory, self.plan, realized, m.PROFILE)

    def execute(self):
        realized, resolved = self.resolve()
        self.assertTrue(r.check_inputs(self.inventory, realized)["passed"])
        r.build_resolved(resolved, self.build)
        audit = r.check_inputs(self.inventory, realized, after_build=True)
        self.assertTrue(audit["passed"], audit["gaps"])
        result = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        return realized, resolved, audit, result

    def test_exact_registration_is_excluded_built_audited_and_never_passed(self):
        _, resolved, audit, result = self.execute()
        self.assertEqual(result["outcome"], "passed", result)
        [record] = resolved["baseline_exclusions"]
        self.assertEqual({k: record[k] for k in m.RECORD_KEYS}, m.authored_record())
        self.assertEqual((record["status"], record["authority"], record["validation"]),
                         ("not_executed", "legacy_unconfigured_nonvalidation", "nonvalidation"))
        self.assertNotIn(m.REGISTRATION, resolved["tests"])
        self.assertEqual(sorted(resolved["tests"]), sorted(m.MANDATORY))
        self.assertIn(m.TARGET, resolved["audit_build_targets"])
        self.assertIn(m.TARGET, resolved["build_targets"])
        self.assertNotIn("signer_endpoint", resolved["required_capabilities"])
        self.assertFalse(resolved["complete_gate_eligible"])
        self.assertTrue((self.build / "nips/nip5f/mini" / m.TARGET).exists())  # built, never run
        self.assertIn(str(self.root / m.SOURCE), audit["input_hashes"])
        self.assertNotIn(m.REGISTRATION, result["observed_executions"])
        self.assertEqual({n: result["observed_executions"][n] for n in m.MANDATORY},
                         {n: "passed" for n in m.MANDATORY})
        normalized = adapter.normalize_receipt(self.inventory, self.plan, resolved, result)
        self.assertEqual(normalized["baseline_exclusions"], resolved["baseline_exclusions"])
        partial = s.aggregate(self.inventory, self.plan, [normalized], complete=False)
        self.assertTrue(partial["passed"], partial)
        self.assertEqual(partial["incomplete_lanes"], [m.PROFILE])
        self.assertFalse(partial["complete_gate_eligible"])
        self.assertFalse(s.aggregate(self.inventory, self.plan, [normalized])["passed"])
        for label, tamper in (("status", lambda e: e.update(status="passed")),
                              ("validation", lambda e: e.pop("validation", None)),
                              ("authority", lambda e: e.update(authority="existing_workflow_baseline")),
                              ("extra", lambda e: e.update(coverage="signing")),
                              ("registrations", lambda e: e.update(registrations=["test_nip5f_loopback"])),
                              ("binding", lambda e: e["binding"].update(source_sha256="0" * 64))):
            with self.subTest(tamper=label):
                forged = copy.deepcopy(normalized)
                # The two receipts may share one exclusion object; tamper each once.
                targets = {id(e): e for e in (forged["baseline_exclusions"][0],
                                              forged["backend_receipt"]["baseline_exclusions"][0])}
                for exclusion in targets.values():
                    tamper(exclusion)
                verdict = s.aggregate(self.inventory, self.plan, [forged], complete=False)
                self.assertFalse(verdict["passed"], verdict)

    def test_missing_forged_double_and_alternate_authorities_refuse(self):
        _, baseline = self.resolve()  # the unmutated fixture resolves: negatives are not vacuous
        self.assertEqual(len(baseline["baseline_exclusions"]), 1)
        real_record = copy.deepcopy(self.inventory["coverage_gaps"][-1])
        self.assertEqual(real_record, m.authored_record())
        mutations = {
            "missing record": (lambda inv: inv["coverage_gaps"].pop(), "no exact applicability authority"),
            "forged binding": (lambda inv: inv["coverage_gaps"][-1]["binding"].update(source_sha256="0" * 64),
                               "differs from the authored"),
            "forged reason": (lambda inv: inv["coverage_gaps"][-1].update(reason="waived"), "differs from the authored"),
            "alternate profile": (lambda inv: inv["coverage_gaps"][-1].update(profile="core.linux.asan"),
                                  "differs from the authored"),
            "double authority": (lambda inv: (inv["checks"][m.CHECK]["profiles"].append(m.PROFILE),
                                              inv["checks"][m.CHECK]["ctest_tests"].update({m.PROFILE: [m.REGISTRATION]})),
                                 "exclude only legacy macOS"),
            "duplicate record": (lambda inv: inv["coverage_gaps"].append(copy.deepcopy(real_record)), "duplicate"),
        }
        for label, (mutate, message) in mutations.items():
            with self.subTest(mutation=label):
                saved = copy.deepcopy(self.inventory)
                try:
                    mutate(self.inventory)
                    with self.assertRaisesRegex((r.RealizationError, s.ContractError), message):
                        self.resolve()
                finally:
                    self.inventory.clear()
                    self.inventory.update(saved)
        self.assertEqual(self.inventory["coverage_gaps"][-1], real_record)

    def test_alternate_profile_id_never_receives_the_authority(self):
        alternate = copy.deepcopy(self.profile)
        self.inventory["profiles"]["core.macos.asan.alternate"] = alternate
        self.refresh_identity()
        self.plan = s.make_plan(self.inventory, [change(m.SOURCE)], head=self.identity["sha"], base="b" * 40,
                                source_identity=self.identity, mode="broad")
        realized = r.configure_profile(self.root, self.build, "core.macos.asan.alternate", alternate,
                                       self.identity, s.digest(self.inventory))
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "core.macos.asan.alternate")
        self.assertEqual(resolved["baseline_exclusions"], [])
        self.assertNotIn(m.REGISTRATION, resolved["tests"])

    def test_registration_drift_refuses(self):
        self.resolve()  # baseline resolves
        cmake = self.root / "nips/nip5f/mini/CMakeLists.txt"
        original = cmake.read_text()
        edits = {
            "skip code removed": (lambda t: t.replace(
                "set_tests_properties(test_nip5f_tcp PROPERTIES SKIP_RETURN_CODE 77)\n", ""), "exit 77"),
            "endpoint in registration env": (lambda t: t.replace(
                "SKIP_RETURN_CODE 77)", "SKIP_RETURN_CODE 77 ENVIRONMENT \"NOSTR_SIGNER_ENDPOINT=tcp:1:2\")"),
                "configures a signer"),
            "mandatory negative removed": (lambda t: t.replace(
                "add_test(NAME test_nip5f_negative_env COMMAND test_nip5f_negative_env)\n", ""), "negative_env"),
        }
        for label, (edit, message) in edits.items():
            with self.subTest(edit=label):
                cmake.write_text(edit(original))
                with self.assertRaisesRegex(r.RealizationError, message):
                    self.resolve()
        cmake.write_text(original)

    def test_inherited_endpoint_or_token_refuses_at_every_phase(self):
        # Configure is where an inherited value first appears: the signer guard
        # names it. Later phases are also refused by the engine's existing
        # phase-environment binding; either refusal must stop before CTest.
        later = "genuine provisioned signer|effective phase environment changed"
        for key, value in dict(SIGNER, NOSTR_SIGNER_TOKEN_EMPTY="").items():
            key = "NOSTR_SIGNER_TOKEN" if key.endswith("_EMPTY") else key
            with self.subTest(phase="configure", key=key, value=value), mock.patch.dict(os.environ, {key: value}):
                with self.assertRaisesRegex(r.RealizationError, "genuine provisioned signer"):
                    self.configure()
        realized = self.configure()
        with mock.patch.dict(os.environ, {"NOSTR_SIGNER_TOKEN": ""}):
            with self.assertRaisesRegex(r.RealizationError, later):
                self.resolve(realized)
        realized, resolved = self.resolve()
        with mock.patch.dict(os.environ, {"NOSTR_SIGNER_ENDPOINT": SIGNER["NOSTR_SIGNER_ENDPOINT"]}):
            with self.assertRaisesRegex(r.RealizationError, later):
                r.build_resolved(resolved, self.build)
        r.build_resolved(resolved, self.build)
        audit = r.check_inputs(self.inventory, realized, after_build=True)
        self.assertTrue(audit["passed"], audit["gaps"])
        with mock.patch.dict(os.environ, {"NOSTR_SIGNER_ENDPOINT": SIGNER["NOSTR_SIGNER_ENDPOINT"]}):
            with self.assertRaisesRegex(r.RealizationError, later):
                r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertFalse(list(self.evidence.rglob("ctest.log")), "CTest ran despite a configured endpoint")

    def test_signer_guard_itself_covers_every_phase_and_the_ctest_environment(self):
        """The guard the engine calls before configure/resolve/build/CTest."""
        realized, _ = self.resolve()
        for phase_env in ({"NOSTR_SIGNER_ENDPOINT": "x"}, {"NOSTR_SIGNER_TOKEN": ""}):
            with self.subTest(env=phase_env):
                profile = copy.deepcopy(realized["profile"])
                profile["environment"].update(phase_env)
                with self.assertRaisesRegex(r.RealizationError, "genuine provisioned signer"):
                    r._legacy_macos_environment_guard(m.PROFILE, profile, realized["repo_root"])
                with self.assertRaisesRegex(r.RealizationError, "genuine provisioned signer"):
                    r._legacy_macos_environment_guard(m.PROFILE, realized["profile"], realized["repo_root"],
                                                      dict(os.environ, **phase_env))
        r._legacy_macos_environment_guard("core.linux.asan", realized["profile"], realized["repo_root"],
                                          {"NOSTR_SIGNER_ENDPOINT": "x"})  # signer profiles are not this guard

    def test_source_drift_after_resolution_refuses_before_ctest(self):
        realized, resolved = self.resolve()
        r.build_resolved(resolved, self.build)
        audit = r.check_inputs(self.inventory, realized, after_build=True)
        self.assertTrue(audit["passed"], audit["gaps"])
        workflow = self.root / m.LEGACY_WORKFLOW
        workflow.write_text(workflow.read_text() + "\n# drift\n")
        with self.assertRaises(r.RealizationError):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertFalse(list(self.evidence.rglob("ctest.log")))


class EngineAuthorityTests(unittest.TestCase):
    """Host-independent: the real engine entry points refuse forged authority."""

    def test_unknown_registration_gap_still_refuses_and_libgo_authority_is_unchanged(self):
        inv = production()
        inv["coverage_gaps"].append({"id": "other", "profile": m.PROFILE, "registrations": ["x"],
                                     "reason": "r", "tracking": "t"})
        realization = {"profile_id": "core.linux.asan", "profile": inv["profiles"]["core.linux.asan"]}
        with self.assertRaisesRegex(r.RealizationError, "unsupported registration exclusion coverage gap"):
            r._baseline_exclusions(inv, realization, {}, {})
        libgo = [g for g in production()["coverage_gaps"] if g["id"] == "libgo_macos_asan_stress_exclusion"]
        self.assertEqual(libgo, [{"id": "libgo_macos_asan_stress_exclusion", "profile": "libgo.macos.asan",
                                  "registrations": ["GoChannelStressTest"], "reason": libgo[0]["reason"],
                                  "tracking": libgo[0]["tracking"]}])

    def test_enrolled_inventory_is_exact_and_selection_valid(self):
        inv = production()
        self.assertEqual(m.records(inv), [m.authored_record()])
        m.validate_policy(inv, m.records(inv)[0], ROOT)
        s.validate_inventory(inv)
        state = s.coverage_state(inv)
        self.assertIn(m.authored_record(), state["physical_coverage_gaps"])  # keeps complete gates closed

    def test_aggregate_branch_is_exact_for_the_lane_profile(self):
        inv = production()
        record = dict(m.authored_record(), status="not_executed", authority=m.AUTHORITY, validation="nonvalidation",
                      target_ids=["tcp-id"], audit_build_targets=[m.TARGET])
        m.validate_receipt_record(record, inv, m.PROFILE)
        for profile in ("core.linux.asan", "libgo.macos.asan"):
            with self.subTest(profile=profile), self.assertRaises(m.ApplicabilityError):
                m.validate_receipt_record(record, inv, profile)
        missing = copy.deepcopy(inv)
        missing["coverage_gaps"] = [g for g in missing["coverage_gaps"] if g["id"] != m.IDENT]
        with self.assertRaises(m.ApplicabilityError):
            m.validate_receipt_record(record, missing, m.PROFILE)


if __name__ == "__main__":
    unittest.main()
