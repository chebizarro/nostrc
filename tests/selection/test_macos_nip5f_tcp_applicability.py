"""Exact legacy macOS unconfigured NIP-5F TCP applicability (nostrc-0cfsg.36.9).

Validator-level cases; test_macos_nip5f_tcp_engine.py drives the real engine.
The authored record below is pinned independently of the module and must equal
the enrolled inventory record. Every drift/configured-endpoint negative refuses
instead of becoming a waiver.
"""
import copy
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

from realization_support import ROOT, r

sys.path.insert(0, str(ROOT / "scripts"))
import test_selection_macos_nip5f_tcp as a  # noqa: E402

AUTHORED_RECORD = {
    "id": "core_macos_asan_nip5f_tcp_unconfigured",
    "profile": "core.macos.asan",
    "check": "supported.nips.nip5f.signer_endpoint",
    "registrations": ["test_nip5f_tcp"],
    "reason": ("Legacy nostrc-ci macOS ASan/UBSan (BUILD_APPS=OFF) provisions no NIP-5F TCP endpoint or token; "
               "test_nip5f_tcp then exits 77 (SKIP_RETURN_CODE 77), which never validated a live signer. "
               "Accounted not_executed/nonvalidation: never passed, skipped-as-pass or signing coverage."),
    "tracking": "nostrc-0cfsg.36.9",
    "binding": {
        "source_sha256": "0686a0c8b1e424f146b08f1a5d5a0588156098067571bd4bd3e545198515242e",
        "registration_sha256": "d85dfaa2b4a907b02a2f16bdef81e051dbf00ffb8684ce3bbdf2c92d03e6228e",
        "selection_metadata_sha256": "8a54cc3454613a5450717c1c193f99dbb98ba0b6ce10154d74c160805236b0af",
        "legacy_job_source_sha256": "35ecf5ab6debc5277f69556b6e106e7b9191638ebbb08bddf1e6ff45590e158c",
        "legacy_workflow_sha256": "10a675396b64c928f3d5bb79025ab2f6be0acbacaa3410155090e186ecdcdcbc",
    },
}
FILES = (a.SOURCE, a.REGISTRATION_FILE, a.SELECTION_FILE, a.LEGACY_WORKFLOW)


def inventory():
    return json.loads((ROOT / "tests/selection/inventory.json").read_text())


class PolicyApplicabilityTests(unittest.TestCase):
    def setUp(self):
        self.inventory = inventory()
        self.record = copy.deepcopy(AUTHORED_RECORD)
        tmp = tempfile.TemporaryDirectory(prefix="nostrc-nip5f-applicability-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        for path in FILES:
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, self.root / path)

    def refuses(self, inventory=None, record=None, root=None, pattern=None):
        with self.assertRaises(a.ApplicabilityError) as caught:
            a.validate_policy(inventory or self.inventory, record or self.record, root or self.root)
        if pattern:
            self.assertRegex(str(caught.exception), pattern)

    def test_exact_legacy_record_validates_against_committed_sources(self):
        self.assertEqual(a.authored_record(), AUTHORED_RECORD)  # independent literal pin
        a.validate_policy(self.inventory, self.record, ROOT)
        a.validate_policy(self.inventory, self.record, self.root)
        self.assertEqual(a.records(self.inventory), [AUTHORED_RECORD])  # enrolled exactly

    def test_record_shape_and_identity_cannot_be_widened(self):
        for change in ({"waiver": True}, {"profile": "core.linux.asan"}, {"profile": "libgo.macos.asan"},
                       {"check": "root.asan.nip5f"}, {"registrations": ["*"]},
                       {"registrations": ["test_nip5f_tcp", "test_nip5f_loopback"]},
                       {"registrations": ["test_nip5f_loopback"]}, {"tracking": "nostrc-0cfsg.36"},
                       {"reason": " "}, {"id": "missing_resource"}):
            with self.subTest(change=change):
                self.refuses(record=dict(copy.deepcopy(self.record), **change))
        for change in ({"extra": "x"}, {"source_sha256": None}):
            with self.subTest(binding=change):
                record = copy.deepcopy(self.record)
                record["binding"].update(change)
                if change.get("source_sha256", 1) is None:
                    del record["binding"]["source_sha256"]
                self.refuses(record=record)

    def test_configured_endpoint_or_token_in_profile_refuses(self):
        for mutate in (lambda p: p["environment"].update(NOSTR_SIGNER_ENDPOINT="tcp:127.0.0.1:1"),
                       lambda p: p["environment"].update(NOSTR_SIGNER_TOKEN="t"),
                       lambda p: p["environment"].update(EXTRA="NOSTR_SIGNER_TOKEN=t"),
                       lambda p: p["capabilities"].append("signer_endpoint")):
            with self.subTest():
                inv = inventory()
                mutate(inv["profiles"][a.PROFILE])
                self.refuses(inventory=inv, pattern="signer")

    def test_profile_variant_drift_refuses(self):
        for key, value in (("platform", "linux"), ("instrumentation", "none"), ("setup", "x.yml:job"),
                           ("architecture", "x86_64"), ("source_dir", "libgo")):
            with self.subTest(key=key):
                inv = inventory()
                inv["profiles"][a.PROFILE][key] = value
                self.refuses(inventory=inv)
        inv = inventory()
        args = inv["profiles"][a.PROFILE]["configure_args"]
        args[args.index("-DBUILD_APPS=OFF")] = "-DBUILD_APPS=ON"
        self.refuses(inventory=inv, pattern="BUILD_APPS")

    def test_cache_definitions_cannot_be_overridden_in_any_cmake_form(self):
        for extra, pattern in ((["-D", "BUILD_APPS=ON"], "duplicate configure option BUILD_APPS"),
                               (["-DBUILD_APPS:BOOL=ON"], "duplicate configure option BUILD_APPS"),
                               (["-D", "GO_ENABLE_ASAN=OFF"], "duplicate configure option GO_ENABLE_ASAN"),
                               (["-UBUILD_APPS"], "cache-altering"), (["-U", "BUILD_APPS"], "cache-altering"),
                               (["-C", "preload.cmake"], "cache-altering"), (["-Cpreload.cmake"], "cache-altering"),
                               (["-D"], "dangling"), (["-DNOVALUE"], "malformed")):
            with self.subTest(extra=extra):
                inv = inventory()
                inv["profiles"][a.PROFILE]["configure_args"] += extra
                self.refuses(inventory=inv, pattern=pattern)

    def test_workflow_provisioning_or_drift_refuses(self):
        workflow = self.root / a.LEGACY_WORKFLOW
        workflow.write_text(workflow.read_text().replace(
            "    env:\n", "    env:\n      NOSTR_SIGNER_ENDPOINT: tcp:127.0.0.1:1\n", 1))
        self.refuses(pattern="workflow")
        inv = inventory()
        inv["workflow_jobs"][a.LEGACY_JOB]["source_sha256"] = "0" * 64
        self.refuses(inventory=inv, pattern="drifted")

    def test_source_registration_and_metadata_drift_refuses(self):
        cases = ((a.SOURCE, "return 77;  /* Signal SKIP to CTest */", "return 0;  /* Signal SKIP to CTest */"),
                 (a.SOURCE, "int main(void) {", "int main(void)  {"),
                 (a.REGISTRATION_FILE, "  set_tests_properties(test_nip5f_tcp PROPERTIES SKIP_RETURN_CODE 77)\n", ""),
                 (a.SELECTION_FILE, "NOSTRC_SELECTION_REQUIRES signer_endpoint", "NOSTRC_SELECTION_REQUIRES none"))
        for path, old, new in cases:
            with self.subTest(path=path, old=old):
                tmp = tempfile.TemporaryDirectory()
                self.addCleanup(tmp.cleanup)
                root = Path(tmp.name)
                for item in FILES:
                    (root / item).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(self.root / item, root / item)
                text = (root / path).read_text()
                self.assertIn(old, text)
                (root / path).write_text(text.replace(old, new, 1))
                self.refuses(root=root, pattern="drifted")

    def test_semantic_anchors_reject_changed_unconfigured_branch(self):
        text = (ROOT / a.SOURCE).read_text()
        self.assertRegex(text, a.UNCONFIGURED)
        self.assertNotRegex(text.replace("return 77;", "return 0;", 1), a.UNCONFIGURED)
        scheduled = text.index('if (getenv("CTEST_RESOURCE_GROUP_COUNT")) {')
        failure = text.index("return 1;", scheduled)
        # A resource-scheduled run without its allocation must fail, never skip.
        self.assertNotRegex(text[:failure] + "return 77;" + text[failure + len("return 1;"):], a.UNCONFIGURED)
        self.assertNotRegex(text.replace('if (getenv("CTEST_RESOURCE_GROUP_COUNT")) {', "{", 1), a.UNCONFIGURED)
        self.assertNotRegex(text.replace('strncmp(ep, "tcp:", 4) != 0 || ', "", 1), a.UNCONFIGURED)
        self.assertIn(a.LEGACY_REGISTRATION, (ROOT / a.REGISTRATION_FILE).read_text())

    def test_membership_mandatory_coverage_and_enforcement_stay_intact(self):
        def readd_profile(inv):  # double authority: check and record
            check = inv["checks"][a.CHECK]
            check["profiles"].append(a.PROFILE)
            check["ctest_tests"][a.PROFILE] = ["test_nip5f_tcp"]

        def drop_signer_coverage(inv):  # the check must stay real for signer profiles
            check = inv["checks"][a.CHECK]
            for pid in list(check["ctest_tests"]):
                check["ctest_tests"][pid] = []

        def other_check_claims(inv):
            inv["checks"][a.MANDATORY_CHECK]["ctest_tests"][a.PROFILE].append("test_nip5f_tcp")

        def drop_negative(inv):
            inv["checks"][a.MANDATORY_CHECK]["ctest_tests"][a.PROFILE].remove("test_nip5f_negative_env")

        for mutate in (readd_profile, drop_signer_coverage, other_check_claims, drop_negative,
                       lambda inv: inv["checks"][a.MANDATORY_CHECK].update(requirement="advisory"),
                       lambda inv: inv["checks"][a.MANDATORY_CHECK].update(requires=["signer_endpoint"]),
                       lambda inv: inv["checks"][a.CHECK].update(requires=["signer_endpoint", "docker"]),
                       lambda inv: inv.update(enforcement_ready=True)):
            with self.subTest():
                inv = inventory()
                mutate(inv)
                self.refuses(inventory=inv)

    def test_single_authority_for_the_registration(self):
        inv = inventory()
        inv["coverage_gaps"] = inv["coverage_gaps"] + [copy.deepcopy(self.record), copy.deepcopy(self.record)]
        with self.assertRaisesRegex(a.ApplicabilityError, "duplicate"):
            a.records(inv)
        inv = inventory()
        inv["coverage_gaps"] = inv["coverage_gaps"] + [dict(id="other", profile="core.linux.asan",
                                                            registrations=["test_nip5f_tcp"])]
        with self.assertRaisesRegex(a.ApplicabilityError, "another authority"):
            a.records(inv)


def realization_fixture(test_case):
    tmp = tempfile.TemporaryDirectory(prefix="nostrc-nip5f-realization-")
    test_case.addCleanup(tmp.cleanup)
    root = Path(tmp.name).resolve()
    source, build = root / "source", root / "build"
    source.mkdir()
    build.mkdir()
    (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Debug\nBUILD_APPS:BOOL=OFF\n"
                                          "BUILD_APPS-ADVANCED:INTERNAL=1\n")
    profile = copy.deepcopy(inventory()["profiles"][a.PROFILE])
    tests = {name: {"name": name, "command": [str(build / "nips/nip5f" / name)], "properties": []}
             for name in ("test_nip5f_tcp",) + a.MANDATORY}
    tests["test_nip5f_tcp"]["properties"] = [{"name": "SKIP_RETURN_CODE", "value": "77"}, ROUTE]
    descriptors = {"test_nip5f_tcp": {"name": "test_nip5f_tcp", "owner": ["nip5f", "nip5f.test_inputs", "nip5f.fixture_inputs"],
                                      "requires": ["signer_endpoint"], "targets": [], "inputs": [], "expected": []}}
    targets = {f"{name}-id": {"id": f"{name}-id", "name": name, "type": "EXECUTABLE",
                              "artifacts": tests[name]["command"], "dependencies": [],
                              "compile_fragments": ["-fsanitize=address -fno-omit-frame-pointer -fsanitize=undefined"],
                              "sources": [{"path": str(source / f"nips/nip5f/tests/{name}.c"), "generated": False}]}
               for name in tests}
    realization = {"profile_id": a.PROFILE, "profile": profile, "repo_root": str(source), "build_dir": str(build),
                   "targets": targets}
    return realization, tests, descriptors


ROUTE = {"name": "RESOURCE_GROUPS", "value": [{"requirements": [{".type": "nip5f_signer", "slots": 1}]}]}


class RealizationApplicabilityTests(unittest.TestCase):
    def run_validate(self, realization, tests, descriptors):
        return a.validate_realization(inventory(), copy.deepcopy(AUTHORED_RECORD), realization, tests, descriptors,
                                      r._mapped_targets, r._relative)

    def test_exact_registration_is_not_executed_nonvalidation_and_still_audited(self):
        result = self.run_validate(*realization_fixture(self))
        self.assertEqual(result["status"], "not_executed")
        self.assertEqual(result["authority"], "legacy_unconfigured_nonvalidation")
        self.assertEqual(result["validation"], "nonvalidation")
        self.assertEqual(result["audit_build_targets"], ["test_nip5f_tcp"])
        self.assertEqual(result["target_ids"], ["test_nip5f_tcp-id"])
        for claim in ("outcome", "passed", "skipped", "coverage"):
            self.assertNotIn(claim, result)

    def test_other_profiles_are_never_applied(self):
        for profile in ("core.linux.asan", "core.linux", "libgo.macos.asan", "grotto.linux"):
            with self.subTest(profile=profile):
                realization, tests, descriptors = realization_fixture(self)
                realization["profile_id"] = profile
                self.assertIsNone(self.run_validate(realization, tests, descriptors))

    def test_configured_or_changed_graph_refuses(self):
        def props(tests, value):
            tests["test_nip5f_tcp"]["properties"] = value

        mutations = {
            "no skip code": lambda rz, t, d: props(t, []),
            "skip code changed": lambda rz, t, d: props(t, [{"name": "SKIP_RETURN_CODE", "value": "0"}]),
            "disabled": lambda rz, t, d: props(t, [{"name": "SKIP_RETURN_CODE", "value": "77"},
                                                   {"name": "DISABLED", "value": "1"}]),
            "endpoint in test env": lambda rz, t, d: props(t, [{"name": "SKIP_RETURN_CODE", "value": "77"},
                                                               {"name": "ENVIRONMENT", "value": ["NOSTR_SIGNER_ENDPOINT=tcp:1:2"]}]),
            "endpoint via env modification": lambda rz, t, d: props(t, [
                {"name": "SKIP_RETURN_CODE", "value": "77"},
                {"name": "ENVIRONMENT_MODIFICATION", "value": ["NOSTR_SIGNER_ENDPOINT=set:tcp:127.0.0.1:9000",
                                                              "NOSTR_SIGNER_TOKEN=set:t"]}]),
            "resource route removed": lambda rz, t, d: props(t, [{"name": "SKIP_RETURN_CODE", "value": "77"}]),
            "resource route slots changed": lambda rz, t, d: props(t, [
                {"name": "SKIP_RETURN_CODE", "value": "77"},
                {"name": "RESOURCE_GROUPS", "value": [{"requirements": [{".type": "nip5f_signer", "slots": 2}]}]}]),
            "realized platform differs": lambda rz, t, d: rz["profile"].update(platform="linux"),
            "realized configure args differ": lambda rz, t, d: rz["profile"]["configure_args"].extend(["-D", "BUILD_APPS=ON"]),
            "realized env has signer": lambda rz, t, d: rz["profile"]["environment"].update(NOSTR_SIGNER_TOKEN="t"),
            "approver/fixture targets present": lambda rz, t, d: d["test_nip5f_tcp"].update(targets=["test_nip5f_tcp", "test_nip5f_approver"]),
            "fixture inputs present": lambda rz, t, d: d["test_nip5f_tcp"].update(inputs=["x"]),
            "requires changed": lambda rz, t, d: d["test_nip5f_tcp"].update(requires=[]),
            "owner changed": lambda rz, t, d: d["test_nip5f_tcp"].update(owner=["nip5f", "nip5f.test_inputs"]),
            "directory owner missing": lambda rz, t, d: d["test_nip5f_tcp"].update(owner=["nip5f.test_inputs", "nip5f.fixture_inputs"]),
            "expected changed": lambda rz, t, d: d["test_nip5f_tcp"].update(expected=["fixture"]),
            "mandatory loopback missing": lambda rz, t, d: t.pop("test_nip5f_loopback"),
            "mandatory negative missing": lambda rz, t, d: t.pop("test_nip5f_negative_env"),
            "registration missing": lambda rz, t, d: t.pop("test_nip5f_tcp"),
            "source mapping changed": lambda rz, t, d: rz["targets"]["test_nip5f_tcp-id"]["sources"][0].update(
                path=rz["repo_root"] + "/nips/nip5f/tests/other.c"),
            "tsan instead": lambda rz, t, d: rz["targets"]["test_nip5f_tcp-id"].update(compile_fragments=["-fsanitize=thread"]),
            "ubsan dropped": lambda rz, t, d: rz["targets"]["test_nip5f_tcp-id"].update(compile_fragments=["-fsanitize=address"]),
            "realized signer capability": lambda rz, t, d: rz["profile"]["capabilities"].append("signer_endpoint"),
            "effective cache BUILD_APPS=ON": lambda rz, t, d: (Path(rz["build_dir"]) / "CMakeCache.txt").write_text(
                "BUILD_APPS:BOOL=ON\n"),
            "cache entry duplicated": lambda rz, t, d: (Path(rz["build_dir"]) / "CMakeCache.txt").write_text(
                "BUILD_APPS:BOOL=OFF\nBUILD_APPS:BOOL=OFF\n"),
            "cache entry missing": lambda rz, t, d: (Path(rz["build_dir"]) / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Debug\n"),
            "cache absent": lambda rz, t, d: (Path(rz["build_dir"]) / "CMakeCache.txt").unlink(),
            "cache symlinked": lambda rz, t, d: ((Path(rz["build_dir"]) / "real-cache").write_text("BUILD_APPS:BOOL=OFF\n"),
                                                  (Path(rz["build_dir"]) / "CMakeCache.txt").unlink(),
                                                  (Path(rz["build_dir"]) / "CMakeCache.txt").symlink_to("real-cache")),
            "daemon producer configured": lambda rz, t, d: rz["targets"].update({"daemon-id": dict(
                rz["targets"]["test_nip5f_loopback-id"], id="daemon-id", name="grotto-daemon")}),
            "apps/ source configured": lambda rz, t, d: rz["targets"].update({"app-id": dict(
                rz["targets"]["test_nip5f_loopback-id"], id="app-id", name="anything",
                sources=[{"path": rz["repo_root"] + "/apps/grotto/main.c", "generated": False}])}),
        }
        for label, mutate in mutations.items():
            with self.subTest(mutation=label):
                realization, tests, descriptors = realization_fixture(self)
                mutate(realization, tests, descriptors)
                with self.assertRaises(a.ApplicabilityError):
                    self.run_validate(realization, tests, descriptors)


class ExecutionEnvironmentTests(unittest.TestCase):
    def test_any_endpoint_or_token_at_execution_refuses(self):
        a.require_unconfigured_environment({"PATH": "/usr/bin", "ASAN_OPTIONS": "detect_leaks=0"})
        for env in ({"NOSTR_SIGNER_ENDPOINT": "tcp:127.0.0.1:9"}, {"NOSTR_SIGNER_TOKEN": "t"},
                    {"NOSTR_SIGNER_ENDPOINT": ""}, {"NOSTR_SIGNER_TOKEN": ""}):
            with self.subTest(env=env), self.assertRaisesRegex(a.ApplicabilityError, "genuine provisioned signer"):
                a.require_unconfigured_environment(env)


if __name__ == "__main__":
    unittest.main()
