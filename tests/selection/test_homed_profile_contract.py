"""Normal homed unit ownership and host applicability; no runtime/hosted promotion."""
import copy
import json
from pathlib import Path
import shutil
import hashlib
import unittest
from unittest import mock

from selector_support import ROOT, s, change
from realization_support import Project, r
from profile_contract_support import validate_job_accounting

PROFILE = "homed.linux.units"
NAMES = ["homed_cache", "homed_cache_config", "homed_groups", "homed_manifest", "homed_uid_map"]


class HomedProfileContract(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / "tests/selection/inventory.json")
        self.profile = self.policy["profiles"][PROFILE]

    def test_shared_canonical_declarations_and_five_owned_units(self):
        homed = ROOT / "gnome/nostr-homed"
        self.assertIn("include(${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/ManifestCacheTests.cmake)",
                      (homed / "CMakeLists.txt").read_text())
        standalone = (homed / "tests/unit/CMakeLists.txt").read_text()
        self.assertIn("include(${CMAKE_CURRENT_SOURCE_DIR}/ManifestCacheTests.cmake)", standalone)
        self.assertIn("nostrc_finalize_test_inventory()", standalone)
        self.assertEqual(self.profile["source_dir"], "gnome/nostr-homed/tests/unit")
        self.assertEqual(self.profile["configure_args"], ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"])
        check = self.policy["checks"]["homed.units"]
        self.assertEqual(check["ctest_tests"], {PROFILE: NAMES})
        claims = [name for c in self.policy["checks"].values() for name in c.get("ctest_tests", {}).get(PROFILE, [])]
        self.assertEqual(claims, NAMES)
        self.assertEqual(check["build_targets"], ["test_cache", "test_cache_config", "test_groups", "test_manifest", "test_uid_map"])
        self.assertEqual(check["runtime_inputs"], [])
        self.assertFalse(self.profile["capabilities"])
        self.assertEqual(self.profile["host_system"], {"ID": "ubuntu", "VERSION_ID": "24.04"})
        self.assertEqual(self.profile["setup"], "standalone/nostr-homed-units-v1")
        self.assertNotIn(PROFILE, self.policy["workflow_jobs"][self.profile["legacy_workflow"]]["profiles"])
        validate_job_accounting(self.policy, ROOT)

    def test_default_build_and_independent_phases_not_imported_passes(self):
        check = self.policy["checks"]["homed.default_build"]
        self.assertEqual(check["build_scope"], "default")
        self.assertEqual(check["build_targets"], [])
        self.assertEqual(check["command"], ["cmake", "--build", "{build}", "--config", "RelWithDebInfo"])
        self.assertEqual(r.authored_phase_parallelism(self.profile), {"build": 2, "test": 1})
        for phase in r.PHASES:
            self.assertEqual(r.phase_environment(self.profile, ROOT, phase, {}), {"CC": "gcc", "CXX": "g++"})
        self.assertFalse(self.policy["enforcement_ready"])
        self.assertIn("homed_unit_profile_contract", {g["id"] for g in self.policy["coverage_gaps"]})
        self.assertEqual(self.profile["scope"], "standalone_manifest_cache_units")

    def test_production_and_unit_inputs_select_units_without_live_peer_alias(self):
        for path in ["gnome/nostr-homed/src/common/nostr_cache.c", "gnome/nostr-homed/include/nostr_manifest.h",
                     "gnome/nostr-homed/tests/unit/test_manifest.c", ".github/workflows/nostr-homed-ci.yml"]:
            plan = s.make_plan(self.policy, [change(path)], base="a" * 40, head="b" * 40)
            self.assertIn("homed.units", plan["selected_checks"], path)
            lanes = [lane for lane in plan["lanes"] if lane["profile"] == PROFILE]
            self.assertEqual(len(lanes), 1)
            self.assertEqual(set(lanes[0]["checks"]), {"homed.units", "homed.default_build"})
        self.assertNotIn(PROFILE, self.policy.get("diagnostic_nonmembers", {}))
        self.assertNotIn(PROFILE, self.policy.get("diagnostic_nonmember_subsets", {}))

    def test_historical_policy_and_sources_reverse_exactly_without_authority_renewal(self):
        from homed_profile_support import before_homed_policy, before_homed_source
        data = json.loads((ROOT / "tests/selection/fixtures/homed_profile_enrollment_20261023.json").read_text())
        historical = before_homed_policy(self.policy)
        self.assertEqual(s.digest(historical), data["before_policy_hash"])
        self.assertEqual(self.policy["diagnostic_nonmember_subsets"], historical["diagnostic_nonmember_subsets"])
        self.assertEqual(self.policy["diagnostic_nonmembers"], historical["diagnostic_nonmembers"])
        for contract in self.policy["diagnostic_nonmember_subsets"].values():
            for path in ("gnome/nostr-homed/CMakeLists.txt", "gnome/nostr-homed/TestInventory.cmake"):
                self.assertEqual(contract["review"]["source_inputs"][path], data["sources"][path]["before_sha256"])
                self.assertNotEqual(contract["review"]["source_inputs"][path], data["sources"][path]["after_sha256"])
        for path, record in data["sources"].items():
            raw = (ROOT / path).read_bytes()
            self.assertEqual(hashlib.sha256(before_homed_source(path, raw)).hexdigest(), record["before_sha256"])
            with self.assertRaises(s.ContractError):
                before_homed_source(path, raw + b"\n")
        changed = copy.deepcopy(self.policy)
        changed["checks"]["homed.units"]["nodes"] = ["libgo"]
        with self.assertRaisesRegex(s.ContractError, "homed enrollment changed"):
            before_homed_policy(changed)
        changed = copy.deepcopy(self.policy)
        changed["nodes"]["unknown_probe"] = {"owner_paths": ["unrelated"], "depends_on": []}
        self.assertIn("unknown_probe", before_homed_policy(changed)["nodes"])

    def test_distribution_is_not_inferred_from_linux_or_aliases(self):
        with mock.patch.object(r.platform, "system", return_value="Linux"):
            for raw in [b'ID=ubuntu\nVERSION_ID="24.04"\n', b'ID="ubuntu"\nVERSION_ID=24.04\nNAME="Ubuntu Linux"\n']:
                with mock.patch.object(Path, "read_bytes", return_value=raw):
                    evidence = r._host_system_evidence(self.profile)
                    self.assertEqual(evidence["identity"], self.profile["host_system"])
                    self.assertEqual(evidence["path"], "/etc/os-release")
            for raw in [b'ID=debian\nVERSION_ID=12\n', b'ID=ubuntu\nVERSION_ID=22.04\n',
                        b'ID=ubuntu\n', b'ID=ubuntu\nID=debian\nVERSION_ID=24.04\n',
                        b'ID=ubuntu\nVERSION_ID="24.04 24.10"\n', b'ID=ubuntu\nVERSION_ID="24.04\n']:
                with self.subTest(raw=raw), mock.patch.object(Path, "read_bytes", return_value=raw):
                    with self.assertRaises(r.RealizationError):
                        r._host_system_evidence(self.profile)
        with mock.patch.object(r.platform, "system", return_value="Darwin"):
            with self.assertRaisesRegex(r.RealizationError, "actual Linux"):
                r._host_system_evidence(self.profile)

    def test_malformed_host_contract_refuses_before_execution(self):
        for value in [None, {}, {"ID_LIKE": "ubuntu"}, {"ID": "Ubuntu"}, {"ID": "ubuntu", "VERSION_ID": 24.04},
                      {"ID": "ubuntu", "image": "ubuntu:24.04"}, "ubuntu"]:
            profile = dict(self.profile, host_system=value)
            with self.subTest(value=value), self.assertRaises(r.RealizationError):
                r.validate_phase_profile(profile)


class HostSystemBinding(Project):
    def test_full_os_release_bytes_are_part_of_phase_and_fingerprint_binding(self):
        self.profile.update(platform="linux", host_system={"ID": "ubuntu", "VERSION_ID": "24.04"})
        first = {"path": "/etc/os-release", "identity": self.profile["host_system"], "sha256": "a" * 64}
        with mock.patch.object(r.platform, "system", return_value="Linux"), \
                mock.patch.object(r, "_host_system_evidence", return_value=first):
            realized = self.configure()
            self.assertEqual(realized["phase_context"]["host_system"], first)
            r._validate_phase_context(realized)
        with mock.patch.object(r, "_host_system_evidence", return_value=dict(first, sha256="b" * 64)):
            with self.assertRaisesRegex(r.RealizationError, "phase environment changed"):
                r._validate_phase_context(realized)


class ActualHomedUnitGraph(Project):
    def test_real_units_full_default_and_source_header_runtime_audits(self):
        homed = "gnome/nostr-homed"
        paths = [homed + "/TestInventory.cmake", homed + "/tests/nh_test.h", homed + "/tests/unit/CMakeLists.txt",
                 homed + "/tests/unit/ManifestCacheTests.cmake",
                 homed + "/src/common/nostr_cache.c", homed + "/src/common/nostr_manifest.c"]
        paths += [homed + "/tests/unit/test_" + name + ".c" for name in
                  ("manifest", "cache", "cache_config", "uid_map", "groups")]
        paths += [p.relative_to(ROOT).as_posix() for p in (ROOT / homed / "include").glob("*.h")]
        for path in paths:
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, target)
        policy = s.load_inventory(ROOT / "tests/selection/inventory.json")
        self.profile = dict(source_dir=homed + "/tests/unit", build_type="RelWithDebInfo",
                            configure_args=["-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"],
                            environment={}, capabilities=[], build_parallelism=2, test_parallelism=1)
        checks = {k: copy.deepcopy(policy["checks"][k]) for k in ("homed.units", "homed.default_build")}
        for check in checks.values():
            check["profiles"] = ["synthetic"]
            if check["kind"] == "ctest":
                check["ctest_tests"] = {"synthetic": NAMES}
        self.inventory = dict(policy, profiles={"synthetic": self.profile}, checks=checks, aliases={},
                              diagnostic_nonmembers={}, diagnostic_nonmember_subsets={}, workflow_jobs={}, coverage_gaps=[])
        self.refresh_identity()
        realized = r.configure_profile(self.root, self.build, "synthetic", self.profile, self.identity,
                                       s.digest(self.inventory), inventory=self.inventory)
        self.assertEqual(sorted(t["name"] for t in realized["tests"]), NAMES)
        pre = r.check_inputs(self.inventory, realized)
        self.assertTrue(pre["passed"], pre["gaps"])
        plan = dict(selected_checks=sorted(checks), source_identity=self.identity, inventory_hash=s.digest(self.inventory))
        resolved = r.resolve_checks(self.inventory, plan, realized, "synthetic")
        built = r.build_resolved(resolved, self.build, build_parallel=2)
        self.assertTrue(any("--target" not in cmd for cmd in built["commands"]))
        post = r.check_inputs(self.inventory, realized, after_build=True)
        self.assertTrue(post["passed"], post["gaps"])
        self.assertGreater(post["header_entries"], 0)
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=post, test_parallel=1)
        self.assertEqual(receipt["outcome"], "passed", receipt)
        self.assertEqual(receipt["ctest_exit_code"], 0)
        self.assertEqual(receipt["observed_executions"], {name: "passed" for name in NAMES + ["homed.default_build"]})
        unowned = copy.deepcopy(self.inventory)
        unowned["checks"]["homed.units"]["ctest_tests"]["synthetic"] = NAMES[:-1]
        audit = r.check_inputs(unowned, realized, after_build=True)
        self.assertIn({"kind": "unclaimed_registration", "test": NAMES[-1]}, audit["gaps"])


if __name__ == "__main__":
    unittest.main()
