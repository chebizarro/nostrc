"""Authored phase jobs bind real native/CTest argv without changing legacy calls."""
import copy
import hashlib
import importlib.util
from pathlib import Path
from unittest import mock

import test_failure_receipts as failure
from realization_support import Project, ROOT, r
from selector_support import s


class PhaseParallelismTests(Project):
    cli = failure.FailureReceiptTests.cli
    assert_negative = failure.FailureReceiptTests.assert_negative

    def prepare(self, *, profile=None, contract=False, default_all=False, **kwargs):
        failure.FailureReceiptTests.prepare(self, **kwargs)
        self.profile.update({"parallelism": 1, "build_parallelism": 2, "test_parallelism": 1} if profile is None else profile)
        if contract or default_all:
            cmake = self.root / "CMakeLists.txt"
            cmake.write_text(cmake.read_text() + "\nadd_custom_target(signer-tests DEPENDS shared)\n")
            self.inventory["checks"]["build.contract"] = {
                "kind": "build_contract", "nodes": ["application", "testing"], "profiles": ["synthetic"],
                "build_targets": ["signer-tests"], "command": ["cmake", "--build", "{build}"] + ([] if default_all else ["--target", "signer-tests"]),
                "working_directory": "{source}", "environment": {}, "cost": "ordinary", "requirement": "blocking", "expected_outcome": "pass"}
        s.validate_inventory(self.inventory)
        s.write_json(self.root / "inventory.json", self.inventory)
        self.git("add", "-A")
        self.git("-c", "core.hooksPath=/dev/null", "commit", "-qm", "phase parallelism fixture")
        head = self.git("rev-parse", "HEAD").decode().strip()
        self.plan = s.make_plan(self.inventory, [], head=head, context="local", mode="broad")
        s.write_json(self.planfile, self.plan)

    def adapter(self):
        spec = importlib.util.spec_from_file_location("phase_parallelism_cli", ROOT / "scripts/test-plan.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def assert_jobs(self, receipt, expected):
        raw = receipt["backend_receipt"]
        realized = r.load_realization(self.build, self.plan["source_identity"])
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        for value in (receipt, raw, resolved, realized["phase_context"]):
            self.assertEqual(value["execution_parallelism"], expected)
        self.assertEqual(raw["resolved_hash"], resolved["resolved_hash"])
        self.assertEqual(resolved["phase_context_hash"], s.digest(resolved["phase_context"]))
        self.assertEqual(resolved["phase_context"]["execution_parallelism"], expected)
        build = next(c for c in raw["commands"] if Path(c[0]).name == "cmake" and c[1] == "--build")
        test = next(c for c in raw["commands"] if Path(c[0]).name == "ctest")
        self.assertEqual(build[build.index("--parallel") + 1], str(expected["build"]))
        self.assertEqual(test[test.index("--parallel") + 1], str(expected["test"]))
        build_status = s.read_json(raw["build_evidence"][0] + ".process.json")
        test_status = s.read_json(Path(raw["evidence_dir"]) / "ctest.log.process.json")
        self.assertEqual(build_status["argv"], build)
        self.assertEqual(test_status["argv"], test)
        for status in (build_status, test_status):
            self.assertEqual(status["exit_code"], 0)
            self.assertEqual(status["outcome"], "passed")
        self.assertEqual(raw["support_tests"], ["cleanup", "reset", "second-setup", "z-first"])
        self.assertTrue(raw["input_audit"]["passed"])
        self.assertTrue(raw["input_audit"]["after_build"])
        self.assertTrue(s.aggregate(self.inventory, self.plan, [receipt])["passed"])
        return resolved, raw

    def test_real_cli_build2_test1_and_explicit_build_only_aggregate(self):
        self.prepare(contract=True)
        receipt = self.cli(expected=0)
        resolved, raw = self.assert_jobs(receipt, {"build": 2, "test": 1})
        contract = resolved["command_executions"]["build.contract"]
        self.assertEqual(contract["phase"], "build")
        self.assertEqual(contract["argv"][-2:], ["--parallel", "2"])
        self.assertEqual(contract["build_targets"], ["signer-tests"])
        self.assertNotIn("signer-tests", raw["observed_executions"])
        self.assertEqual(raw["checks"]["build.contract"]["observed_executions"], ["build.contract"])
        self.assertEqual(raw["checks"]["build.contract"]["argv"], contract["argv"])

    def test_explicit_phase_cli_values_agree_independently(self):
        self.prepare()
        self.assert_jobs(self.cli("--parallel", "1", "--build-parallel", "2", "--test-parallel", "1", expected=0), {"build": 2, "test": 1})

    def test_missing_phase_uses_legacy_parallelism_fallback(self):
        self.prepare(profile={"parallelism": 3, "build_parallelism": 2})
        self.assert_jobs(self.cli(expected=0), {"build": 2, "test": 3})

    def test_phase_without_legacy_fallback_defaults_to_one(self):
        self.prepare(profile={"build_parallelism": 2})
        self.assert_jobs(self.cli(expected=0), {"build": 2, "test": 1})

    def test_legacy_cli_shared_override_keeps_argv_and_identity_shape(self):
        self.prepare(profile={"parallelism": 3})
        receipt = self.cli("--parallel", "3", expected=0)
        raw = receipt["backend_receipt"]
        self.assertNotIn("execution_parallelism", receipt)
        self.assertNotIn("execution_parallelism", raw)
        self.assertNotIn("execution_parallelism", raw["phase_context"])
        for command in raw["commands"]:
            if Path(command[0]).name in ("cmake", "ctest"):
                self.assertEqual(command[command.index("--parallel") + 1], "3")
        self.assertTrue(s.aggregate(self.inventory, self.plan, [receipt])["passed"])

    def test_legacy_cli_no_override_still_defaults_to_one(self):
        self.prepare(profile={"parallelism": 3})
        raw = self.cli(expected=0)["backend_receipt"]
        for command in raw["commands"]:
            if Path(command[0]).name in ("cmake", "ctest"):
                self.assertEqual(command[command.index("--parallel") + 1], "1")

    def test_type_and_positive_authorship_validation(self):
        self.prepare()
        for key in ("build_parallelism", "test_parallelism", "parallelism"):
            for invalid in (0, -1, True, False, "2", 1.5, None):
                with self.subTest(key=key, invalid=invalid):
                    policy = copy.deepcopy(self.inventory)
                    policy["profiles"]["synthetic"][key] = invalid
                    with self.assertRaises(s.ContractError):
                        s.validate_inventory(policy)
                    with self.assertRaises(r.RealizationError):
                        r.validate_phase_profile(policy["profiles"]["synthetic"])
        self.profile["build_parallelism"] = True
        s.write_json(self.root / "inventory.json", self.inventory)
        self.assertIsNone(self.cli(expected=2))
        self.assertIn(b"authored positive integers", self.last_cli_process.stderr)
        self.assertFalse(self.build.exists())
        self.assertFalse(self.evidence.exists())

    def test_wrong_build_override_refuses_before_configure(self):
        self.prepare()
        receipt = self.cli("--build-parallel", "1")
        self.assert_negative(receipt, "execution_options")
        self.assertIn("build parallelism override", receipt["failure"]["errors"][0]["message"])
        self.assertFalse(self.build.exists())

    def test_wrong_test_override_refuses_before_configure(self):
        self.prepare()
        receipt = self.cli("--test-parallel", "2")
        self.assert_negative(receipt, "execution_options")
        self.assertIn("test parallelism override", receipt["failure"]["errors"][0]["message"])
        self.assertFalse(self.build.exists())

    def test_shared_override_cannot_silently_flatten_split_authorship(self):
        self.prepare()
        receipt = self.cli("--parallel", "1")
        self.assert_negative(receipt, "execution_options")
        self.assertIn("build parallelism override", receipt["failure"]["errors"][0]["message"])

    def test_nonpositive_effective_override_is_not_launched(self):
        self.prepare()
        receipt = self.cli("--build-parallel", "0", expected=2)
        self.assert_negative(receipt, "execution_options")
        self.assertFalse(self.build.exists())

    def test_profile_counts_and_actual_argv_cannot_be_laundered(self):
        self.prepare()
        receipt = self.cli(expected=0)
        module = self.adapter()
        resolved, raw = self.assert_jobs(receipt, {"build": 2, "test": 1})
        missing_profile = copy.deepcopy(self.inventory)
        del missing_profile["profiles"]
        with self.assertRaisesRegex(s.ContractError, "requires an authored profile"):
            module.normalize_receipt(missing_profile, self.plan, resolved, raw)
        mutations = []
        bad = copy.deepcopy(receipt)
        bad["execution_parallelism"]["build"] = 1
        mutations.append(bad)
        bad = copy.deepcopy(receipt)
        bad["backend_receipt"]["execution_parallelism"]["test"] = 2
        mutations.append(bad)
        bad = copy.deepcopy(receipt)
        bad["backend_receipt"]["execution_parallelism"]["test"] = True
        mutations.append(bad)
        for tool, incorrect in (("cmake", "1"), ("ctest", "2")):
            bad = copy.deepcopy(receipt)
            command = next(c for c in bad["backend_receipt"]["commands"] if Path(c[0]).name == tool)
            command[command.index("--parallel") + 1] = incorrect
            mutations.append(bad)
        for extra in (["--parallel", "9"], ["-j9"], ["--", "-j9"]):
            bad = copy.deepcopy(receipt)
            command = next(c for c in bad["backend_receipt"]["commands"] if Path(c[0]).name == "ctest")
            command += extra
            mutations.append(bad)
        bad = copy.deepcopy(receipt)
        bad["backend_receipt"]["phase_context"]["execution_parallelism"]["build"] = 1
        bad["backend_receipt"]["phase_context_hash"] = s.digest(bad["backend_receipt"]["phase_context"])
        mutations.append(bad)
        for bad in mutations:
            with self.subTest(mutation=bad):
                for complete in (False, True):
                    result = s.aggregate(self.inventory, self.plan, [bad], complete=complete)
                    self.assertFalse(result["passed"])
                    self.assertTrue(any("phase parallelism" in e for e in result["errors"]), result)
                if s.digest(bad["backend_receipt"]) != s.digest(raw):
                    with self.assertRaises(s.ContractError):
                        module.normalize_receipt(self.inventory, self.plan, resolved, bad["backend_receipt"])
        changed = copy.deepcopy(resolved)
        changed["execution_parallelism"]["build"] = 1
        changed["resolved_hash"] = s.digest({k: v for k, v in changed.items() if k != "resolved_hash"})
        with self.assertRaisesRegex(r.RealizationError, "phase parallelism identity"):
            r.build_resolved(changed, self.build)
        changed = copy.deepcopy(resolved)
        changed["execution_parallelism"]["test"] = True
        changed["resolved_hash"] = s.digest({k: v for k, v in changed.items() if k != "resolved_hash"})
        with self.assertRaisesRegex(r.RealizationError, "phase parallelism identity"):
            r.build_resolved(changed, self.build)

    def test_separate_phase_library_calls_enforce_only_the_executed_phase(self):
        self.prepare()
        built = self.cli("--phase", "build", expected=0)
        self.assertFalse(built["execution_complete"])
        self.assertEqual(built["commands"][0][built["commands"][0].index("--parallel") + 1], "2")
        resolved = built["resolved"]
        with mock.patch.object(r, "_execute") as execute:
            with self.assertRaisesRegex(r.RealizationError, "build parallelism override"):
                r.build_resolved(resolved, self.build, parallel=1)
            execute.assert_not_called()
        direct_build = r.build_resolved(resolved, self.build, parallel=2)
        self.assertEqual(direct_build["commands"][0][direct_build["commands"][0].index("--parallel") + 1], "2")
        raw = r.run_resolved(resolved, self.build, self.evidence / "library", input_audit=built["input_audit"], build=False, parallel=1)
        self.assertEqual(raw["execution_parallelism"], {"build": 2, "test": 1})
        test = next(c for c in raw["commands"] if c[0] == "ctest")
        self.assertEqual(test[test.index("--parallel") + 1], "1")
        receipt = self.cli("--phase", "test", configure=False, expected=0)
        self.assertEqual(receipt["execution_parallelism"], {"build": 2, "test": 1})
        self.assertTrue(s.aggregate(self.inventory, self.plan, [receipt])["passed"])

    def test_phase_override_without_authorship_is_not_silently_accepted(self):
        self.prepare(profile={"parallelism": 3})
        receipt = self.cli("--build-parallel", "2")
        self.assert_negative(receipt, "execution_options")
        self.assertIn("require authored", receipt["failure"]["errors"][0]["message"])
        self.assertFalse(self.build.exists())

    def test_explicit_unused_phase_override_still_has_to_agree(self):
        self.prepare()
        built = self.cli("--phase", "build", expected=0)
        with self.assertRaisesRegex(r.RealizationError, "build parallelism override"):
            r.run_resolved(built["resolved"], self.build, self.evidence / "wrong-unused",
                input_audit=built["input_audit"], build=False, parallel=1, build_parallel=1)

    def test_default_all_remains_unsupported_not_a_fabricated_target(self):
        self.prepare(default_all=True)
        receipt = self.cli()
        self.assert_negative(receipt, "resolve")
        self.assertIn("requires exact cmake --build/--target", receipt["failure"]["errors"][0]["message"])
        self.assertFalse(list(self.evidence.rglob("build-attempt-*.log")))
        self.assertTrue(all(c["outcome"] == "missing" for c in receipt["checks"].values()))

    def test_split_profile_does_not_waive_missing_command_capabilities(self):
        self.prepare(command="raise RuntimeError('must not launch')\n", missing_capability=True)
        receipt = self.cli()
        self.assert_negative(receipt, "resolve")
        self.assertIn("unavailable command capabilities", receipt["failure"]["errors"][0]["message"])
        self.assertEqual(receipt["failure"]["work_status"]["test"], "not_executed")
        self.assertFalse(list(self.evidence.rglob("command-*.log")))

    def test_split_profile_does_not_waive_source_dirty_guard_or_copy_contents(self):
        self.prepare()
        secret = b"private source must not enter receipt"
        (self.root / "vector.txt").write_bytes(secret)
        receipt = self.cli()
        self.assert_negative(receipt, "source_validation")
        self.assertFalse(self.build.exists())
        self.assertNotIn(secret, s.canonical(receipt))
        changed = next(c for c in receipt["failure"]["source_status"]["changes"] if c["path"] == "vector.txt")
        self.assertEqual(changed["sha256"], hashlib.sha256(secret).hexdigest())

    def test_no_new_diagnostic_nonmember_execution_option_admission(self):
        realization = {"profile": {"parallelism": 1, "build_parallelism": 2, "test_parallelism": 1}}
        resolved = {"baseline_exclusions": [{"authority": "reviewed_diagnostic_nonmembers_v1"}]}
        with self.assertRaisesRegex(r.RealizationError, "split parallelism is not admitted"):
            r._diagnostic_execution_options(resolved, realization, 1)

    def test_cancellation_retains_split_identity_actual_build_and_raw_status(self):
        body = (f"import os,signal; os.write(1,{failure.PAYLOAD!r}); "
                "os.kill(os.getppid(),signal.SIGTERM); signal.pause()\n")
        self.prepare(command=body)
        receipt = self.cli()
        self.assert_negative(receipt, "test")
        self.assertEqual(receipt["job_conclusion"], "cancelled")
        raw = receipt["failure"]["backend_receipt"]
        self.assertEqual(raw["execution_parallelism"], {"build": 2, "test": 1})
        self.assertEqual(raw["phase_context"]["execution_parallelism"], {"build": 2, "test": 1})
        self.assertEqual(raw["process_error"]["outcome"], "cancelled")
        self.assertTrue(raw["process_error"]["cleanup_complete"])
        log = next(e for e in receipt["failure"]["evidence"] if e["path"].endswith("command-command.actual.log"))
        self.assertEqual(Path(log["path"]).read_bytes(), failure.PAYLOAD)
        self.assertEqual(raw["process_error"]["output_sha256"], hashlib.sha256(failure.PAYLOAD).hexdigest())
        build_status = s.read_json(next(self.evidence.rglob("build-attempt-1.log.process.json")))
        self.assertEqual(build_status["argv"][build_status["argv"].index("--parallel") + 1], "2")
