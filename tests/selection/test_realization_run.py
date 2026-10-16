from pathlib import Path
from types import SimpleNamespace
from unittest import mock
from realization_support import Project, r


class ExecutionTests(Project):
    def ready(self, selected=None):
        if selected:
            self.plan["selected_checks"] = selected
        realized = self.configure()
        self.build_all()
        return r.resolve_checks(self.inventory, self.plan, realized, "synthetic"), self.audit(realized)

    def test_one_indexed_invocation_with_ordering_and_fixture_closure(self):
        resolved, audit = self.ready()
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit,
                                 evidence_paths=[self.build / "vectors.jsonl"])
        self.assertEqual(receipt["outcome"], "passed", receipt)
        calls = [c for c in receipt["commands"] if c[0] == "ctest"]
        self.assertEqual(len(calls), 1)
        self.assertIn("-I", calls[0])
        self.assertNotIn("--rerun-failed", calls[0])
        self.assertEqual(set(receipt["observed_executions"]), set(resolved["tests"]))
        log = (Path(receipt["evidence_dir"]) / "ctest.log").read_text()
        self.assertLess(log.index("z-first"), log.index("a-model"))

    def test_gui_skip_is_failure_not_acceptance(self):
        resolved, audit = self.ready(["gui"])
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit)
        self.assertEqual(receipt["outcome"], "failed")
        self.assertEqual(receipt["observed_executions"]["gui"], "unexpected_skip")

    def test_attempt_evidence_survives_fixture_reset(self):
        resolved, audit = self.ready()
        receipts = [r.run_resolved(resolved, self.build, self.evidence, attempt=n,
                                  input_audit=audit, evidence_paths=[self.build / "vectors.jsonl"])
                    for n in (1, 2)]
        dirs = [Path(x["evidence_dir"]) for x in receipts]
        self.assertNotEqual(dirs[0], dirs[1])
        for directory in dirs:
            self.assertEqual((directory / "runtime-evidence/vectors.jsonl").read_text(), "evidence-42\n")
            self.assertTrue((directory / "results.xml").exists())
        with self.assertRaises(FileExistsError):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit)

    def test_input_change_after_audit_cannot_reuse_success(self):
        resolved, audit = self.ready()
        (self.root / "shared.h").write_text("#define SHARED 99\n")
        with self.assertRaisesRegex(r.RealizationError, "input changed since audit"):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit)

    def test_fixture_failure_retains_non_execution(self):
        (self.root / "setup.cmake").write_text('message(FATAL_ERROR "fixture failure")\n')
        resolved, audit = self.ready()
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit)
        self.assertEqual(receipt["outcome"], "failed")
        self.assertNotEqual(receipt["observed_executions"]["a-model"], "passed")

    def test_no_audit_is_not_a_passing_receipt(self):
        resolved, _ = self.ready()
        with self.assertRaisesRegex(r.RealizationError, "post-build input audit"):
            r.run_resolved(resolved, self.build, self.evidence)
        self.assertFalse(list(self.evidence.rglob("receipt.json")))

    def test_cli_adapter_and_selector_receipt_fields(self):
        self.inventory["profiles"] = {"synthetic": self.profile}
        self.plan["inventory_hash"] = r._hash(self.inventory)
        self.plan["plan_id"] = r._hash(self.plan)
        planfile = self.root / "plan.json"
        r._atomic(planfile, self.plan)
        output = self.root.parent / "receipt.json"
        args = SimpleNamespace(profile="synthetic", repo=str(self.root), plan=str(planfile),
            configure=True, command="run", phase="all", build_dir=str(self.build),
            resolved=None, output=str(output), attempt=1)
        self.assertEqual(r.cli(args, self.inventory), 0)
        receipt = r._read(output)
        for key in ("lane_id", "profile", "profile_hash", "profile_fingerprint", "plan_id"):
            self.assertTrue(receipt[key])
        self.assertEqual(receipt["plan_id"], self.plan["plan_id"])
        self.assertEqual(receipt["registration_audit"], "passed")
        self.assertEqual(receipt["input_audit"], "passed")
        self.assertEqual(receipt["checks"]["a-model"]["expected_executions"], ["a-model"])
        self.assertEqual(receipt["checks"]["a-model"]["observed_executions"], ["a-model"])

    def test_only_named_incompatibility_with_exact_evidence_is_accepted(self):
        # Synthetic structured-result adversaries, not live MDK evidence.
        path = self.root.parent / "synthetic-junit.xml"
        named = "groundhog-mdk011-interop-mdk09-probe"
        for name, expected in (("ordinary-gui", "failed"), (named, "expected_incompatibility")):
            text = f'<testsuite><testcase name="{name}" status="notrun"><skipped message="SKIP_RETURN_CODE=77"/><system-out>XFAIL [unsupported]: MDK 0.9.0 unsupported proof version 1</system-out></testcase></testsuite>'
            path.write_text(text)
            self.assertEqual(r._execution_outcomes(path, [name], {name: ["mdk09_incompatibility"]})[name], expected)
        for invalid in (text.replace("=77", "=78"), text.replace("XFAIL", "XPASS"),
                        text.replace('<skipped message="SKIP_RETURN_CODE=77"/>', "")):
            path.write_text(invalid)
            self.assertEqual(r._execution_outcomes(path, [named], {named: ["mdk09_incompatibility"]})[named], "failed")

    def test_empty_unrun_or_missing_junit_never_passes(self):
        path = self.root.parent / "synthetic-junit.xml"
        self.assertEqual(r._execution_outcomes(path, ["model"], {}), {"model": "missing"})
        path.write_text('<testsuite><testcase name="model"/></testsuite>')
        self.assertEqual(r._execution_outcomes(path, ["model"], {}), {"model": "failed"})
        path.write_text('<testsuite><testcase name="model" status="run"/><testcase name="model" status="run"/></testsuite>')
        with self.assertRaisesRegex(r.RealizationError, "duplicate JUnit"):
            r._execution_outcomes(path, ["model"], {})

    def test_retry_requires_previous_attempt_evidence(self):
        resolved, audit = self.ready()
        with self.assertRaisesRegex(r.RealizationError, "missing JSON"):
            r.run_resolved(resolved, self.build, self.evidence, attempt=2, input_audit=audit)

    def test_changed_environment_requires_new_profile_realization(self):
        resolved, audit = self.ready()
        with mock.patch.dict("os.environ", {"UNDECLARED_RUNTIME_CHANGE": "different"}):
            with self.assertRaisesRegex(r.RealizationError, "environment changed"):
                r.run_resolved(resolved, self.build, self.evidence, input_audit=audit)

    def test_test_only_phase_refuses_stale_compiled_objects(self):
        resolved, _ = self.ready()
        with (self.root / "main.c").open("a") as stream:
            stream.write("\n/* changed after build */\n")
        realized = r.load_realization(self.build)
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        with self.assertRaisesRegex(r.RealizationError, "not up to date"):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)

    def test_live_image_lifecycle_is_not_faked(self):
        resolved, audit = self.ready()
        resolved["required_capabilities"].append("docker")
        with self.assertRaisesRegex(r.RealizationError, "guarded adapter"):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit)
