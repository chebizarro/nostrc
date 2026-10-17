"""Real CLI negative receipts are evidence, never validation attestations."""
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

from realization_support import Project, ROOT, r
from selector_support import s

PAYLOAD = b"negative:\xff\x00\xfe\r\n"


class FailureReceiptTests(Project):
    def prepare(self, *, preaudit=False, postaudit=False, configure_failure=False,
                build_failure=False, command=None, capability=False, missing_capability=False, configure_cancel=False):
        compiler = shutil.which("clang") or shutil.which("gcc")
        if not compiler:
            self.skipTest("native CLI fixture needs clang or gcc")
        self.profile.update(platform={"Darwin": "macos", "Linux": "linux"}.get(platform.system(), "windows"),
                            architecture="native", compiler="clang" if "clang" in compiler else "gcc",
                            build_type="Debug", instrumentation="none", setup="synthetic", local_availability="host")
        self.profile["configure_args"] += ["-DCMAKE_BUILD_TYPE=Debug", f"-DCMAKE_C_COMPILER={compiler}"]
        self.profile["environment"] = {"TEST_DISPLAY": "1"}
        self.inventory.update(schema_version=1, profiles={"synthetic": self.profile},
                              test_overrides=[], release_components={})
        for ident, node in self.inventory["nodes"].items():
            node.update(id=ident, owner_paths=[ident], rationale="synthetic fixture")
        for rule in self.inventory["path_rules"]:
            rule.update(reason="synthetic fixture")
            rule.setdefault("effect", "impact")
        self.inventory["path_rules"].append({"exact": "inventory.json", "nodes": [], "effect": "global", "reason": "fixture inventory"})
        self.inventory["checks"]["a_model"] = self.inventory["checks"].pop("a-model")
        for check in self.inventory["checks"].values():
            check.update(cost="ordinary", requirement="blocking", expected_outcome="pass")
        cmake = self.root / "CMakeLists.txt"
        if preaudit:
            del self.inventory["checks"]["omitted"]
        if postaudit:
            next(rule for rule in self.inventory["path_rules"] if rule.get("exact") == "shared.h")["nodes"] = ["other"]
        if configure_failure:
            cmake.write_text(cmake.read_text() + '\nmessage(FATAL_ERROR "intentional configure negative")\n')
        if configure_cancel:
            body = (f"import os,signal,subprocess; os.write(1,{PAYLOAD!r}); "
                    "parent=int(subprocess.check_output(['ps','-o','ppid=','-p',str(os.getppid())]).strip()); "
                    "os.kill(parent,signal.SIGTERM); signal.pause()\n")
            (self.root / "configure-negative.py").write_text(body)
            self.inventory["path_rules"].append({"exact": "configure-negative.py", "nodes": [], "effect": "global", "reason": "fixture configure input"})
            cmake.write_text(cmake.read_text() + f'\nexecute_process(COMMAND "{sys.executable}" "${{CMAKE_CURRENT_SOURCE_DIR}}/configure-negative.py")\n')
        if build_failure:
            body = f"import os,sys; os.write(1,{PAYLOAD!r}); sys.exit(23)"
            (self.root / "build-negative.py").write_text(body)
            self.inventory["path_rules"].append({"exact": "build-negative.py", "nodes": [], "effect": "global", "reason": "fixture build input"})
            cmake.write_text(cmake.read_text() + f'\nadd_custom_command(TARGET shared PRE_BUILD COMMAND "{sys.executable}" "${{CMAKE_CURRENT_SOURCE_DIR}}/build-negative.py")\n')
        if command is not None:
            (self.root / "negative.py").write_text(command)
            self.inventory["path_rules"].append({"exact": "negative.py", "nodes": ["application"], "effect": "impact", "reason": "fixture command"})
            self.inventory["checks"]["command.actual"] = {"kind": "command", "nodes": ["application"], "profiles": ["synthetic"],
                "command": ["python3", "{source}/negative.py"], "runtime_inputs": ["{source}/negative.py"],
                "working_directory": "{source}", "environment": {}, "requires": [],
                "cost": "ordinary", "requirement": "blocking", "expected_outcome": "pass"}
        if missing_capability:
            self.profile["capabilities"].remove("display")
            self.inventory["checks"]["command.actual"]["requires"] = ["display"]
        if capability:
            cmake.write_text(cmake.read_text().replace("nostrc_describe_test(${t} OWNER application TARGETS shared INPUTS vector.txt)",
                "nostrc_describe_test(${t} OWNER application TARGETS shared INPUTS vector.txt REQUIRES docker)"))
            self.profile["capabilities"].append("docker")
        s.validate_inventory(self.inventory)
        s.write_json(self.root / "inventory.json", self.inventory)
        self.git("add", "-A")
        self.git("-c", "core.hooksPath=/dev/null", "commit", "-qm", "CLI fixture")
        head = self.git("rev-parse", "HEAD").decode().strip()
        self.plan = s.make_plan(self.inventory, [], head=head, context="local", mode="broad")
        self.planfile, self.output = self.root.parent / "plan.json", self.root.parent / "receipt.json"
        s.write_json(self.planfile, self.plan)

    def cli(self, *options, expected=1, configure=True, attempt=1):
        args = [sys.executable, str(ROOT / "scripts/test-plan.py"), "run", "--repo", str(self.root),
                "--inventory", "inventory.json", "--profile", "synthetic", "--plan", str(self.planfile),
                "--build-dir", str(self.build), "--evidence-dir", str(self.evidence), "--attempt", str(attempt),
                "--output", str(self.output)]
        if configure:
            args.append("--configure")
        result = subprocess.run([*args, *options], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=40)
        self.assertEqual(result.returncode, expected, result.stderr.decode(errors="replace") + result.stdout.decode(errors="replace"))
        return s.read_json(self.output) if self.output.exists() else None

    def assert_negative(self, receipt, stage):
        self.assertEqual(receipt["receipt_kind"], "lane_failure_v1")
        self.assertEqual(receipt["lane_id"], "synthetic")
        self.assertEqual(receipt["failure"]["stage"], stage, receipt)
        self.assertEqual(receipt["source_identity"], self.plan["source_identity"])
        self.assertEqual(receipt["plan_id"], self.plan["plan_id"])
        self.assertEqual(set(receipt["checks"]), set(self.plan["selected_checks"]))
        for key in ("complete", "complete_gate_eligible", "diagnostic_eligible"):
            self.assertIs(receipt[key], False)
        self.assertNotEqual(receipt["job_conclusion"], "success")
        for evidence in receipt["failure"]["evidence"]:
            self.assertEqual(evidence["status"], "retained", evidence)
            self.assertEqual(evidence["sha256"], hashlib.sha256(Path(evidence["path"]).read_bytes()).hexdigest())
        for audit in receipt["failure"]["input_audits"]:
            name = "postaudit.json" if audit.get("after_build") else "preaudit.json"
            saved = next(e for e in receipt["failure"]["evidence"] if e["path"].endswith("/" + name))
            self.assertEqual(s.read_json(saved["path"]), audit)
        for complete in (False, True):
            result = s.aggregate(self.inventory, self.plan, [receipt], complete=complete,
                                 selection_conclusion="success", job_conclusions={"synthetic": "success"})
            self.assertFalse(result["passed"])
            self.assertFalse(result["diagnostic_eligible"])
            self.assertFalse(result["complete_gate_eligible"])
            self.assertTrue(any(f"synthetic: lane failure at {stage}" in e for e in result["errors"]))
            self.assertFalse(any("lane: None" in e or "missing lane" in e or "duplicate/unexpected" in e for e in result["errors"]))
            self.assertEqual(result["non_clean_results"][0]["errors"], receipt["failure"]["errors"])
        # Retained negatives cannot become success by laundering summary flags.
        forged = copy.deepcopy(receipt)
        forged.update(job_conclusion="success", input_audit="passed", registration_audit="passed",
                      diagnostic_eligible=True, complete_gate_eligible=True, profile_fingerprint="fabricated")
        for check in forged["checks"].values():
            check.update(outcome="passed", observed_executions=check["expected_executions"])
        self.assertFalse(s.aggregate(self.inventory, self.plan, [forged], complete=False)["passed"])
        release = {**{k: self.plan[k] for k in ("source_identity", "tested_sha", "inventory_hash")},
                   "required_package_jobs": [], "publish": True, "tag": "fixture-v1.0.0"}
        validation = dict(release, passed=True, complete=True, mode="broad", partial_profiles=False,
                          retained_evidence=[forged])
        promotion = s.aggregate_release(release, validation, {})
        self.assertFalse(promotion["passed"])
        self.assertFalse(promotion["publish"])
        failure_files = list(self.evidence.rglob("failure.json"))
        self.assertEqual(len(failure_files), 1)
        self.assertEqual(s.read_json(failure_files[0]), receipt)

    def test_real_cli_positive_still_uses_strict_success_normalization(self):
        self.prepare()
        receipt = self.cli(expected=0)
        self.assertEqual(receipt["lane_id"], "synthetic")
        self.assertNotIn("receipt_kind", receipt)
        self.assertTrue(s.aggregate(self.inventory, self.plan, [receipt])["passed"])
        self.assertFalse(list(self.evidence.rglob("failure.json")))

    def test_real_cli_phase_composition_keeps_separate_phase_evidence(self):
        self.prepare()
        configure = self.cli("--phase", "configure", expected=0)
        self.assertFalse(configure["execution_complete"])
        self.assertEqual(configure["phase"], "configure")
        built = self.cli("--phase", "build", configure=False, expected=0)
        self.assertFalse(built["execution_complete"])
        self.assertEqual(built["phase"], "build")
        receipt = self.cli("--phase", "test", configure=False, expected=0)
        self.assertTrue(s.aggregate(self.inventory, self.plan, [receipt])["passed"])
        self.assertFalse(list(self.evidence.rglob("failure.json")))

    def test_real_cli_aggregate_preserves_failed_lane_and_original_audit(self):
        self.prepare(preaudit=True)
        receipt = self.cli()
        for partial in (False, True):
            output = self.root.parent / f"aggregate-{partial}.json"
            command = [sys.executable, str(ROOT / "scripts/test-plan.py"), "aggregate", "--repo", str(self.root),
                       "--inventory", "inventory.json", "--plan", str(self.planfile), "--receipt", str(self.output),
                       "--output", str(output)]
            result = subprocess.run(command + (["--partial"] if partial else []), capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 1, result.stderr)
            aggregate = s.read_json(output)
            self.assertFalse(aggregate["passed"])
            self.assertFalse(aggregate["complete_gate_eligible"])
            self.assertTrue(any("synthetic: lane failure at preaudit" in e for e in aggregate["errors"]))
            self.assertFalse(any("None" in e or "missing lane" in e or "duplicate" in e for e in aggregate["errors"]))
            self.assertEqual(aggregate["non_clean_results"][0]["input_audits"], receipt["failure"]["input_audits"])

    def test_failed_preaudit_retains_original_findings_and_all_unexecuted_checks(self):
        self.prepare(preaudit=True)
        receipt = self.cli()
        self.assert_negative(receipt, "preaudit")
        audit = receipt["failure"]["input_audits"][0]
        self.assertFalse(audit["passed"])
        self.assertEqual(receipt["registration_audit"], "failed")
        self.assertTrue(any(g["kind"] == "unclaimed_registration" for g in audit["gaps"]), audit)
        self.assertEqual(receipt["failure"]["work_status"]["test"], "not_executed")
        self.assertTrue(all(v["outcome"] == "missing" for v in receipt["checks"].values()))
        self.assertEqual(receipt["failure"]["resolved"]["support_tests"], ["cleanup", "reset", "second-setup", "z-first"])
        self.assertTrue(all(v == "not_executed" for v in receipt["failure"]["execution_status"].values()))
        self.assertFalse(list(self.evidence.rglob("receipt.json")))

    def test_failed_postaudit_is_not_laundered_into_passed_input_audit(self):
        self.prepare(postaudit=True)
        receipt = self.cli()
        self.assert_negative(receipt, "postaudit")
        pre, post = receipt["failure"]["input_audits"]
        self.assertTrue(pre["passed"])
        self.assertFalse(post["passed"])
        self.assertTrue(post["after_build"])
        self.assertIn("shared.h", json.dumps(post))
        self.assertEqual(receipt["input_audit"], "failed")
        self.assertEqual(receipt["failure"]["work_status"]["build"], "passed")
        self.assertEqual(receipt["failure"]["work_status"]["test"], "not_executed")

    def test_configure_failure_has_no_fabricated_fingerprint_or_executions(self):
        self.prepare(configure_failure=True)
        receipt = self.cli()
        self.assert_negative(receipt, "configure")
        self.assertIsNone(receipt["profile_fingerprint"])
        self.assertIsNone(receipt["failure"]["resolved"])
        log = next(e for e in receipt["failure"]["evidence"] if e["path"].endswith("configure.log"))
        status = s.read_json(log["path"] + ".process.json")
        self.assertNotEqual(status["exit_code"], 0)
        self.assertIn(b"intentional configure negative", Path(log["path"]).read_bytes())
        self.assertFalse((self.build / "nostrc-test-realization.json").exists())

    def test_build_failure_retains_exact_invalid_bytes_and_child_status(self):
        self.prepare(build_failure=True)
        receipt = self.cli()
        self.assert_negative(receipt, "build")
        log = next(e for e in receipt["failure"]["evidence"] if e["path"].endswith("build-attempt-1.log"))
        self.assertIn(PAYLOAD, Path(log["path"]).read_bytes())
        status = s.read_json(log["path"] + ".process.json")
        self.assertNotEqual(status["exit_code"], 0)
        self.assertEqual(status["outcome"], "failed")
        self.assertEqual(receipt["failure"]["work_status"]["test"], "not_executed")

    def test_actual_configure_sigterm_keeps_invalid_bytes_and_reaped_child(self):
        self.prepare(configure_cancel=True)
        receipt = self.cli()
        self.assert_negative(receipt, "configure")
        self.assertEqual(receipt["job_conclusion"], "cancelled")
        log = next(e for e in receipt["failure"]["evidence"] if e["path"].endswith("configure.log"))
        status = s.read_json(log["path"] + ".process.json")
        self.assertEqual(status["outcome"], "cancelled")
        self.assertTrue(status["cleanup_complete"])
        self.assertIsNotNone(status["exit_code"])
        self.assertIn(PAYLOAD, Path(log["path"]).read_bytes())
        self.assertFalse((self.build / "nostrc-test-realization.json").exists())

    def test_missing_command_capability_refuses_before_any_launch(self):
        self.prepare(command="raise RuntimeError('must never launch')\n", missing_capability=True)
        receipt = self.cli()
        self.assert_negative(receipt, "resolve")
        self.assertIn("unavailable command capabilities", json.dumps(receipt["failure"]["errors"]))
        self.assertEqual(receipt["failure"]["work_status"]["test"], "not_executed")
        self.assertFalse(list(self.evidence.rglob("command-*.log")))

    def test_capability_refusal_precedes_build_and_keeps_expected_lane(self):
        self.prepare(capability=True)
        receipt = self.cli()
        self.assert_negative(receipt, "capability")
        self.assertIn("guarded adapter", json.dumps(receipt["failure"]["errors"]))
        self.assertEqual(receipt["failure"]["work_status"]["build"], "not_executed")
        self.assertFalse(list(self.evidence.rglob("build-attempt-*.log")))

    def test_dirty_source_evidence_is_names_hashes_status_not_secret_contents(self):
        self.prepare()
        secret = "DO-NOT-RETAIN-this-source-content"
        (self.root / "shared.h").write_text(secret)
        (self.root / "untracked name\nfile").write_text(secret)
        (self.root / "unsafe-link").symlink_to(self.root.parent / "outside")
        receipt = self.cli()
        self.assert_negative(receipt, "source_validation")
        status = receipt["failure"]["source_status"]
        names = {x["path"]: x for x in status["changes"]}
        self.assertEqual(names["shared.h"]["sha256"], hashlib.sha256(secret.encode()).hexdigest())
        self.assertIn("untracked name\nfile", names)
        self.assertEqual(names["unsafe-link"]["content_status"], "symlink_or_outside_checkout")
        self.assertNotIn(secret, json.dumps(receipt))
        self.assertFalse(self.build.exists())

    def test_actual_child_sigterm_retains_raw_negative_and_process_custody(self):
        body = (f"import os,signal; os.write(1,{PAYLOAD!r}); "
                "os.kill(os.getppid(),signal.SIGTERM); signal.pause()\n")
        self.prepare(command=body)
        receipt = self.cli()
        self.assert_negative(receipt, "test")
        self.assertEqual(receipt["job_conclusion"], "cancelled")
        raw = receipt["failure"]["backend_receipt"]
        raw_file = next(e for e in receipt["failure"]["evidence"] if e["path"].endswith("/receipt.json"))
        self.assertEqual(s.read_json(raw_file["path"]), raw)
        self.assertEqual(raw["outcome"], "cancelled")
        self.assertFalse(raw["complete_gate_eligible"])
        process = raw["process_error"]
        self.assertEqual(process["outcome"], "cancelled")
        self.assertTrue(process["cleanup_complete"])
        self.assertIsNotNone(process["exit_code"])
        log = next(e for e in receipt["failure"]["evidence"] if e["path"].endswith("command-command.actual.log"))
        self.assertEqual(Path(log["path"]).read_bytes(), PAYLOAD)
        self.assertEqual(process["output_sha256"], log["sha256"])
        self.assertEqual(raw["observed_executions"]["command.actual"], "missing")

    def test_invalid_plan_and_unselected_profile_cannot_authorize_failure_lane(self):
        self.prepare()
        self.plan["plan_id"] = "unauthenticated"
        s.write_json(self.planfile, self.plan)
        self.assertIsNone(self.cli(expected=2))
        self.assertFalse(self.evidence.exists())
        self.plan["lanes"], self.plan["selected_checks"], self.plan["expected_lane_ids"] = [], [], []
        self.plan.pop("plan_id")
        self.plan["plan_id"] = s.digest(self.plan)
        s.write_json(self.planfile, self.plan)
        self.assertIsNone(self.cli(expected=2))
        self.assertFalse(self.evidence.exists())

    def test_failure_attempt_cannot_overwrite_original_evidence(self):
        self.prepare(configure_failure=True)
        receipt = self.cli()
        before = {str(p): p.read_bytes() for p in self.evidence.rglob("*") if p.is_file()}
        self.cli(expected=2)
        self.assertEqual({p: Path(p).read_bytes() for p in before}, before)
        self.assertEqual(s.read_json(self.output), receipt)
