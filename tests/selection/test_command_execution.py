"""Real native command/build-contract executions; no mocked process successes."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import signal
import sys
from types import SimpleNamespace
from realization_support import Project, ROOT, r
from selector_support import s


class CommandExecutionTests(Project):
    def command(self, body=None, **extra):
        (self.root / "command.py").write_text(body or '''import os, pathlib, sys
assert pathlib.Path.cwd() == pathlib.Path(sys.argv[1])
assert os.environ['COMMAND_POLICY'] == 'bound'
assert pathlib.Path('vector.txt').read_text() == 'test vector\\n'
p = pathlib.Path(sys.argv[2]); p.write_text('actual command evidence\\n')
print('command executed', flush=True)
''')
        check = {"kind": "command", "profiles": ["synthetic"], "nodes": ["application", "testing"],
                 "command": ["python3", "{source}/command.py", "{source}", "{build}/proof.txt"],
                 "working_directory": "{source}", "environment": {"COMMAND_POLICY": "bound"},
                 "runtime_inputs": ["{source}/command.py", "{source}/vector.txt"],
                 "build_targets": [], "required_evidence": ["proof.txt"], **extra}
        self.inventory["path_rules"].append({"exact": "command.py", "nodes": ["application"]})
        self.inventory["checks"]["command.actual"] = check
        self.plan["selected_checks"] = ["command.actual"]
        return check

    def ready(self, source_only=False):
        self.refresh_identity()
        realized = (r.configure_command_profile(self.root, self.build, "synthetic", self.profile,
                    self.identity, "inventory-fixture") if source_only else self.configure())
        if not source_only:
            self.build_all()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        return resolved, audit

    def test_source_only_command_without_ctest_or_compiler_claim(self):
        self.command()
        # Source-only realization has no CTest registrations. Only command
        # ownership is applicable; don't invent missing CTest membership.
        self.inventory["checks"] = {"command.actual": self.inventory["checks"]["command.actual"]}
        self.profile["configure_args"] = []
        resolved, audit = self.ready(source_only=True)
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(receipt["outcome"], "passed", receipt)
        self.assertEqual(receipt["checks"]["command.actual"]["exit_code"], 0)
        self.assertEqual(len(receipt["commands"]), 1)
        self.assertNotIn("ctest", receipt["commands"][0])
        self.assertTrue(receipt["diagnostic_eligible"])
        self.assertEqual((self.build / "proof.txt").read_text(), "actual command evidence\n")
        self.assertTrue(receipt["checks"]["command.actual"]["artifacts"]["proof.txt"]["sha256"])
        self.assertTrue(receipt["input_audit"]["input_hashes"])

    def test_source_build_overlap_cannot_hide_untracked_source_inputs(self):
        self.command()
        self.refresh_identity()
        for build in (self.root, self.root.parent):
            with self.subTest(build=build), self.assertRaisesRegex(r.RealizationError, "cannot contain"):
                r.configure_command_profile(self.root, build, "synthetic", self.profile,
                                            self.identity, "inventory-fixture")
        self.assertFalse((self.root / "nostrc-command-generation.json").exists())

    def test_mixed_command_and_single_ctest_run(self):
        self.command()
        self.plan["selected_checks"].append("a-model")
        resolved, audit = self.ready()
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(receipt["outcome"], "passed", receipt)
        self.assertEqual(sum(c[0] == "ctest" for c in receipt["commands"]), 1)
        self.assertEqual(set(receipt["checks"]), {"a-model", "command.actual"})
        self.assertEqual(set(receipt["observed_executions"]), set(resolved["expected_executions"]))

    def test_real_native_build_contract_exact_target_and_header_inputs(self):
        self.inventory["checks"]["build.actual"] = {"kind": "build_contract", "profiles": ["synthetic"],
            "nodes": ["application"], "command": ["cmake", "--build", "{build}", "--target", "shared"],
            "working_directory": "{source}", "environment": {}, "runtime_inputs": [], "build_targets": []}
        self.plan["selected_checks"] = ["build.actual"]
        resolved, audit = self.ready()
        self.assertEqual(resolved["contract_build_targets"], ["shared"])
        self.assertIn(str(self.root / "shared.h"), audit["input_hashes"])
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(receipt["outcome"], "passed", receipt)
        record = receipt["checks"]["build.actual"]
        self.assertEqual(record["exit_code"], 0)
        self.assertIn("no work to do", Path(record["log"]).read_text())
        self.assertNotIn("ctest", [c[0] for c in receipt["commands"]])
        self.assertTrue((self.build / "shared").is_file())

    def test_absent_command_wrong_cwd_substitution_and_capability_denied(self):
        check = self.command()
        for field, value, message in (
                ("command", ["no-such-nostrc-command-executable"], "unavailable"),
                ("working_directory", str(self.root.parent), "cwd"),
                ("working_directory", "relative", "cwd"),
                ("command", ["python3", "{unknown}/command.py"], "substitution"),
                ("runtime_inputs", [], "explicit source"),
                ("requires", ["unavailable_capability"], "capabilities")):
            with self.subTest(field=field, value=value):
                original = copy.deepcopy(check[field]) if field in check else None
                check[field] = value
                realized = self.configure()
                with self.assertRaisesRegex(r.RealizationError, message):
                    r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
                if original is None:
                    del check[field]
                else:
                    check[field] = original

    def test_unknown_or_disagreeing_build_contract_target_fails(self):
        check = self.command()
        check.update(kind="build_contract", command=["cmake", "--build", "{build}", "--target", "absent"])
        realized = self.configure()
        with self.assertRaisesRegex(r.RealizationError, "unknown command build target"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        check.update(command=["cmake", "--build", "{build}", "--target", "shared"], build_targets=["other"])
        with self.assertRaisesRegex(r.RealizationError, "disagreement"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        check.update(command=["cmake", "--build", "{build}", "--target", "shared", "--clean-first"])
        with self.assertRaisesRegex(r.RealizationError, "exact cmake"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")

    def test_missing_or_nonzero_evidence_never_passes(self):
        for body in ("print('no evidence')\n", "import sys; print('nonzero'); sys.exit(9)\n"):
            with self.subTest(body=body):
                self.command(body)
                resolved, audit = self.ready()
                receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
                record = receipt["checks"]["command.actual"]
                self.assertEqual(receipt["outcome"], "failed")
                self.assertEqual(record["missing_evidence"], ["proof.txt"])
                self.assertFalse(receipt["diagnostic_eligible"])
                self.assertTrue(Path(record["log"]).is_file())

    def test_stale_artifact_is_not_same_execution_evidence(self):
        self.command()
        resolved, audit = self.ready()
        (self.build / "proof.txt").write_text("old attempt")
        with self.assertRaisesRegex(r.RealizationError, "stale"):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        receipt = r._read(r._attempt_directory(resolved, self.evidence, 1) / "receipt.json")
        self.assertEqual(receipt["outcome"], "failed")

    def test_input_drift_and_wrong_checkout_are_not_success(self):
        self.command()
        resolved, audit = self.ready()
        (self.root / "vector.txt").write_text("changed input\n")
        with self.assertRaisesRegex(r.RealizationError, "snapshot digest"):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertFalse(list(self.evidence.rglob("receipt.json")))
        self.assertTrue(list(self.evidence.rglob("preexecution-failure.txt")))

    def test_command_interpreter_script_and_audit_content_cannot_evade_binding(self):
        check = self.command()
        # Omitting the literal argv script from runtime_inputs does not hide it
        # from source ownership and impact-edge auditing.
        check["runtime_inputs"] = ["{source}/vector.txt"]
        self.inventory["path_rules"][-1]["nodes"] = ["other"]
        realized = self.configure()
        self.build_all()
        audit = self.audit(realized)
        self.assertFalse(audit["passed"])
        self.assertTrue(any(g["kind"] == "missing_impact_edge" and g.get("input") == "command.py" for g in audit["gaps"]))
        self.inventory["path_rules"][-1]["nodes"] = ["application"]
        resolved, audit = self.ready()
        audit["input_hashes"] = {}
        with self.assertRaisesRegex(r.RealizationError, "content hash"):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)

    def test_declared_live_capability_is_not_adapter_attestation(self):
        self.command(requires=["docker"])
        self.profile["capabilities"].append("docker")
        resolved, audit = self.ready()
        with self.assertRaisesRegex(r.RealizationError, "guarded adapter"):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertFalse(list(self.evidence.rglob("receipt.json")))

    def test_command_source_mutation_after_start_is_failed_evidence(self):
        self.command("from pathlib import Path; Path('vector.txt').write_text('mutated')\n", required_evidence=[])
        resolved, audit = self.ready()
        with self.assertRaisesRegex(r.RealizationError, "snapshot digest"):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        receipt = r._read(r._attempt_directory(resolved, self.evidence, 1) / "receipt.json")
        self.assertEqual(receipt["outcome"], "failed")
        self.assertFalse(receipt["diagnostic_eligible"])

    def test_timeout_and_actual_sigterm_retain_failure_and_cancellation(self):
        for body, expected in (("import signal; print('waiting', flush=True); signal.pause()\n", "failed"),
                ("import os,signal; print('cancelling',flush=True); os.kill(os.getppid(),signal.SIGTERM); signal.pause()\n", "cancelled")):
            with self.subTest(expected=expected):
                self.command(body, required_evidence=[])
                resolved, audit = self.ready()
                with self.assertRaises(r.RealizationError):
                    r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False, timeout=0.25)
                directory = r._attempt_directory(resolved, self.evidence, 1)
                receipt = r._read(directory / "receipt.json")
                self.assertEqual(receipt["outcome"], expected)
                self.assertFalse(receipt["diagnostic_eligible"])
                self.assertNotEqual(receipt["observed_executions"].get("command.actual"), "passed")
                log = directory / "command-command.actual.log"
                self.assertEqual(log.read_bytes(), b"cancelling\n" if expected == "cancelled" else b"waiting\n")
                status = r._read(str(log) + ".process.json")
                self.assertEqual(status["outcome"], expected)
                self.assertEqual(receipt["process_error"], status)
                self.assertEqual(receipt["job_conclusion"], "failure")
                self.assertFalse(receipt["complete_gate_eligible"])

    def test_command_cli_mixed_selected_skip_stays_failure(self):
        self.cli_run(source_only=False)

    def test_command_cli_source_only_success_has_bound_normalized_receipt(self):
        self.cli_run(source_only=True)

    def test_real_native_prerequisite_build_failure_retains_log_and_process_status(self):
        self.cli_run(source_only=False, fail_build=True)

    def cli_run(self, source_only, fail_build=False):
        self.command()
        # A complete policy/plan uses the actual CLI orchestrator, not a fake
        # normalize-only fixture. The fixture is deliberately not live acceptance.
        self.inventory.update(schema_version=1, profiles={"synthetic": self.profile}, release_components={})
        self.profile.update(platform={"darwin": "macos"}.get(sys.platform, sys.platform), architecture="native",
                            compiler="clang" if sys.platform == "darwin" else "gcc", build_type="Debug",
                            instrumentation="none", setup="mini-native", local_availability="host")
        self.profile["configure_args"] += ["-DCMAKE_BUILD_TYPE=Debug"]
        for node in self.inventory["nodes"].values():
            node.update(owner_paths=["main.c"], rationale="mini-project")
        for rule in self.inventory["path_rules"]:
            rule.setdefault("effect", "impact")
            rule.setdefault("reason", "mini-project")
        for check in self.inventory["checks"].values():
            check.update(cost="ordinary", requirement="blocking", expected_outcome="pass")
        self.inventory["checks"]["a.model"] = self.inventory["checks"].pop("a-model")
        if source_only:
            self.inventory["checks"] = {"command.actual": self.inventory["checks"]["command.actual"]}
            self.profile["configure_args"] = []
        if fail_build:
            (self.root / "main.c").write_text("this is not valid C;\n")
        self.refresh_identity()
        plan = s.make_plan(self.inventory, [], head=self.identity["sha"], mode="broad", source_identity=self.identity)
        planfile = self.root.parent / "plan.json"
        r._atomic(planfile, plan)
        spec = importlib.util.spec_from_file_location("command_cli", ROOT / "scripts/test-plan.py")
        cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cli)
        args = SimpleNamespace(plan=str(planfile), repo=str(self.root), profile="synthetic", configure=True,
            phase="all", build_dir=str(self.build), evidence_dir=str(self.evidence), command="run",
            resolved=None, parallel=1, timeout=20, attempt=1, evidence_path=[])
        # The GUI is an actual selected skip in broad mode: it must fail even
        # while the command succeeds, not become a passing lane by normalization.
        if fail_build:
            with self.assertRaisesRegex(s.CheckFailure, "prerequisite build failed"):
                cli.realization_command(args, self.inventory, r)
            status_files = list(self.evidence.rglob("build-attempt-1.log.process.json"))
            self.assertEqual(len(status_files), 1)
            status = r._read(status_files[0])
            self.assertEqual(status["outcome"], "failed")
            self.assertNotEqual(status["exit_code"], 0)
            self.assertTrue(status_files[0].with_name("build-attempt-1.log").read_text())
            return
        receipt, code = cli.realization_command(args, self.inventory, r)
        self.assertEqual(receipt["checks"]["command.actual"]["outcome"], "passed")
        if source_only:
            self.assertEqual(code, 0)
            self.assertEqual(receipt["job_conclusion"], "success")
            self.assertTrue(receipt["diagnostic_eligible"])
            self.assertEqual(receipt["source_identity"], self.identity)
            self.assertTrue(receipt["backend_receipt"]["input_audit"]["input_hashes"])
            self.assertTrue(s.aggregate(self.inventory, plan, [receipt], complete=False)["passed"])
        else:
            self.assertEqual(code, 1)
            self.assertEqual(receipt["checks"]["gui"]["outcome"], "unexpected_skip")
            self.assertEqual(receipt["job_conclusion"], "failure")
