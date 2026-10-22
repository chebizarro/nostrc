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


class DefaultBuildContracts(Project):
    def default_contract(self, extra_cmake=""):
        self.inventory["checks"]["build.default"] = {
            "kind": "build_contract", "profiles": ["synthetic"], "nodes": ["application"],
            "build_scope": "default", "build_targets": [],
            "command": ["cmake", "--build", "{build}"], "working_directory": "{source}",
            "environment": {}, "runtime_inputs": []}
        # This app has no CTest registration. An unrelated excluded binary must
        # not be relabelled as native default membership.
        (self.root / "app.c").write_text('#include "app.h"\nint main(void) { return APP; }\n')
        (self.root / "app.h").write_text('#define APP 0\n')
        with (self.root / "CMakeLists.txt").open("a") as stream:
            stream.write('\nadd_executable(app app.c)\nadd_executable(excluded EXCLUDE_FROM_ALL app.c)\n' + extra_cmake)
        self.inventory["path_rules"] += [{"exact": p, "nodes": ["application"]} for p in ("app.c", "app.h")]
        self.plan["selected_checks"] = ["build.default"]
        return self.inventory["checks"]["build.default"]

    def configured_default(self):
        self.refresh_identity()
        return r.configure_profile(self.root, self.build, "synthetic", self.profile,
            self.identity, "inventory-fixture", inventory=self.inventory)

    def test_actual_default_non_test_build_audit_and_execution(self):
        self.default_contract()
        realized = self.configured_default()
        binding = realized["default_build_binding"]
        self.assertFalse(binding["gaps"], binding)
        names = {realized["targets"][key]["name"] for key in binding["target_ids"]}
        self.assertEqual(names, {"app", "shared"})
        self.assertNotIn("all", {t["name"] for t in realized["targets"].values()})
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        pre = r.check_inputs(self.inventory, realized)
        self.assertTrue(pre["passed"], pre["gaps"])
        build = r.build_resolved(resolved, self.build)
        self.assertNotIn("--target", build["commands"][0])
        self.assertTrue((self.build / "app").is_file())
        self.assertFalse((self.build / "excluded").exists())
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        for name in ("app.c", "app.h"):
            self.assertIn(str(self.root / name), audit["input_hashes"])
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(receipt["outcome"], "passed", receipt)
        self.assertEqual(receipt["checks"]["build.default"]["build_scope"], "default")
        self.assertEqual(receipt["default_build_hash"], resolved["default_build_hash"])
        self.assertEqual(len(receipt["commands"]), 1)
        self.assertNotIn("--target", receipt["commands"][0])
        r.load_realization(self.build)  # discovered app.h is not declared-graph drift

    def test_object_static_shared_and_escaped_path_default_closure(self):
        self.default_contract('''
add_library(objects OBJECT "library source.c")
add_library(archive STATIC $<TARGET_OBJECTS:objects>)
add_library(dynamic SHARED "library source.c")
set_target_properties(dynamic PROPERTIES VERSION 1.2 SOVERSION 1)
set_target_properties(app PROPERTIES OUTPUT_NAME "app copy")
''')
        (self.root / 'library source.c').write_text('int library_value(void) { return 0; }\n')
        self.inventory['path_rules'].append({'exact': 'library source.c', 'nodes': ['application']})
        realized = self.configured_default()
        self.assertFalse(realized['default_build_binding']['gaps'], realized['default_build_binding']['gaps'])
        names = {realized['targets'][key]['name'] for key in realized['default_build_binding']['target_ids']}
        self.assertEqual(names, {'app', 'shared', 'objects', 'archive', 'dynamic'})
        resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        r.build_resolved(resolved, self.build)
        audit = self.audit(realized)
        self.assertTrue(audit['passed'], audit['gaps'])
        self.assertTrue((self.build / 'app copy').is_file())
        self.assertIn(str(self.root / 'library source.c'), audit['input_hashes'])

    def test_non_test_input_and_missing_impact_are_fatal(self):
        self.default_contract()
        self.inventory["path_rules"][ -2]["nodes"] = ["other"]
        realized = self.configured_default()
        pre = r.check_inputs(self.inventory, realized)
        self.assertFalse(pre["passed"])
        self.assertTrue(any(g.get("check") == "build.default" and g.get("input") == "app.c" for g in pre["gaps"]), pre)

    def test_reached_excluded_dependency_is_really_default_built(self):
        self.default_contract('add_dependencies(app excluded)\n')
        realized = self.configured_default()
        names = {realized["targets"][key]["name"] for key in realized["default_build_binding"]["target_ids"]}
        self.assertEqual(names, {"app", "shared", "excluded"})
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        r.build_resolved(resolved, self.build)
        self.assertTrue((self.build / "excluded").is_file())
        self.assertTrue(self.audit(realized)["passed"])

    def test_custom_all_and_link_hooks_refuse_before_execution(self):
        for index, custom in enumerate((
            'add_custom_target(opaque ALL COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/opaque-ran")',
            'add_custom_command(TARGET app PRE_BUILD COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/opaque-ran")',
            'add_custom_command(TARGET app PRE_LINK COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/opaque-ran")',
            'add_custom_command(TARGET app POST_BUILD COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/opaque-ran")')):
            with self.subTest(custom=custom):
                if index == 0:
                    self.default_contract()
                    original = (self.root / "CMakeLists.txt").read_text()
                (self.root / "CMakeLists.txt").write_text(original + '\n' + custom + '\n')
                realized = self.configured_default()
                self.assertTrue(realized["default_build_binding"]["gaps"])
                with self.assertRaisesRegex(r.RealizationError, "unsupported default"):
                    r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
                self.assertFalse((self.build / "opaque-ran").exists())

    def test_native_manifest_default_mutation_and_missing_binding_refuse(self):
        self.default_contract()
        realized = self.configured_default()
        unbound = copy.deepcopy(realized)
        del unbound["default_build_binding"]
        with self.assertRaisesRegex(r.RealizationError, "attestation"):
            r._command_specs(self.inventory, unbound)
        manifest = self.build / "build.ninja"
        original = manifest.read_bytes()
        manifest.write_bytes(original.replace(b'default all', b'default shared'))
        with self.assertRaisesRegex(r.RealizationError, "stale realization"):
            r.load_realization(self.build)
        manifest.write_bytes(original)
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        resolved["default_build_hash"] = "forged"
        with self.assertRaisesRegex(r.RealizationError, "default build binding"):
            r.build_resolved(resolved, self.build)

    def test_unselected_default_contract_still_builds_before_full_audit(self):
        self.default_contract()
        self.plan["selected_checks"] = ["a-model"]
        realized = self.configured_default()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        self.assertNotIn("build.default", resolved["command_executions"])
        r.build_resolved(resolved, self.build)
        self.assertTrue((self.build / "app").is_file())
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(receipt["outcome"], "passed")
        self.assertEqual(set(receipt["checks"]), {"a-model"})

    def test_named_nondefault_prerequisites_are_not_dropped(self):
        self.default_contract()
        self.inventory["checks"]["build.named"] = {
            "kind": "build_contract", "profiles": ["synthetic"], "nodes": ["application"],
            "command": ["cmake", "--build", "{build}", "--target", "excluded"],
            "build_targets": ["excluded"], "working_directory": "{source}", "environment": {}}
        self.plan["selected_checks"].append("build.named")
        realized = self.configured_default()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        built = r.build_resolved(resolved, self.build)
        self.assertEqual(len(built["commands"]), 2)
        self.assertNotIn("--target", built["commands"][0])
        self.assertEqual(built["commands"][1][-2:], ["--target", "excluded"])
        self.assertTrue((self.build / "excluded").is_file())
        self.assertTrue(self.audit(realized)["passed"])

    def test_unattested_build_missing_artifact_and_header_index_refuse(self):
        self.default_contract()
        realized = self.configured_default()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        self.build_all()  # physical work alone is not bound prerequisite evidence
        audit = self.audit(realized)
        self.assertFalse(audit["passed"])
        self.assertTrue(any(g["kind"] == "default_build_execution" for g in audit["gaps"]))
        r.build_resolved(resolved, self.build)
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        (self.build / "app").unlink()
        missing = self.audit(realized)
        self.assertTrue(any(g["kind"] == "missing_command_build_artifact" and g.get("target") == "app" for g in missing["gaps"]))
        r.build_resolved(resolved, self.build)
        (self.build / ".ninja_deps").unlink()
        missing = self.audit(realized)
        self.assertTrue(any(g["kind"] == "missing_discovered_header_index" and g.get("target") == "app" for g in missing["gaps"]))

    def test_pending_default_build_and_audited_source_drift_refuse(self):
        self.default_contract()
        realized = self.configured_default()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        r.build_resolved(resolved, self.build)
        audit = self.audit(realized)
        self.assertTrue(audit["passed"])
        import time
        stamp = max(time.time_ns(), (self.build / 'app').stat().st_mtime_ns + 1)
        os.utime(self.root / 'app.c', ns=(stamp, stamp))
        with self.assertRaisesRegex(r.RealizationError, "default is not up to date"):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        raw = r._read(r._attempt_directory(resolved, self.evidence, 1) / "receipt.json")
        self.assertEqual(raw["outcome"], "failed")
        self.assertFalse(raw["diagnostic_eligible"])
        r.build_resolved(resolved, self.build)
        audit = self.audit(realized)
        (self.root / "app.h").write_text('#define APP 1\n')
        with self.assertRaisesRegex(r.RealizationError, "source|snapshot"):
            r.run_resolved(resolved, self.build, self.evidence / "drift", input_audit=audit, build=False)

    def test_manifest_include_without_suffix_and_native_requery(self):
        self.default_contract()
        realized = self.configured_default()
        included = self.build / "extra-manifest"
        included.write_text('# transitive no-extension manifest\n')
        main = self.build / "build.ninja"
        main.write_text(main.read_text() + '\ninclude extra-manifest\n')
        manifests = r._ninja_manifests(self.build)
        self.assertIn(str(included), manifests["files"])
        self.assertTrue(any(edge[2] == str(included) for edge in manifests["includes"]))
        # Even a caller renewing just the superficial generation-file hash
        # cannot hide a changed loaded manifest set from the native binding.
        record = r._read(self.build / "nostrc-test-realization.json")
        record["generation_files"][str(main)] = r.hashlib.sha256(main.read_bytes()).hexdigest()
        r._atomic(self.build / "nostrc-test-realization.json", record)
        with self.assertRaisesRegex(r.RealizationError, "native default graph changed"):
            r.load_realization(self.build)

    def test_nonzero_actual_default_build_retains_failure_not_stamp(self):
        self.default_contract()
        (self.root / "app.c").write_text('not valid C\n')
        realized = self.configured_default()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        with self.assertRaisesRegex(r.RealizationError, "command failed"):
            r.build_resolved(resolved, self.build)
        logs = list(self.build.glob("nostrc-prerequisite-*.log"))
        self.assertEqual(len(logs), 1)
        process = r._read(str(logs[0]) + '.process.json')
        self.assertEqual(process["outcome"], "failed")
        self.assertNotEqual(process["exit_code"], 0)
        self.assertFalse((self.build / "nostrc-default-build-execution.json").exists())

    def test_actual_default_build_cancellation_retains_raw_bytes(self):
        # Ninja's ordinary compiler pool buffers child stdout until task exit.
        # Its real console pool gives the compiler the native output pipe, so
        # emitted bytes precede the cancellation signal at the actual boundary.
        self.default_contract('set_property(TARGET app PROPERTY JOB_POOL_COMPILE console)\n')
        import shutil
        compiler = shutil.which(os.environ.get("CC", "cc"))
        wrapper = self.root / "compiler.py"
        wrapper.write_text(f'''#!{sys.executable}
import os, signal, subprocess, sys
if any(arg.endswith('/app.c') for arg in sys.argv[1:]):
    os.write(1, b'default-cancel:\\xff\\x00\\xfe\\n')
    os.kill(int(os.environ['CANCEL_DEFAULT_PARENT']), signal.SIGTERM)
    signal.pause()
sys.exit(subprocess.call([{compiler!r}, *sys.argv[1:]]))
''')
        wrapper.chmod(0o755)
        self.profile["configure_args"].append('-DCMAKE_C_COMPILER={source_root}/compiler.py')
        self.profile["environment"]["CANCEL_DEFAULT_PARENT"] = str(os.getpid())
        self.inventory["path_rules"].append({"exact": "compiler.py", "nodes": ["application"]})
        realized = self.configured_default()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        with self.assertRaises(KeyboardInterrupt):
            r.build_resolved(resolved, self.build)
        logs = list(self.build.glob("nostrc-prerequisite-*.log"))
        self.assertEqual(len(logs), 1)
        self.assertIn(b'default-cancel:\xff\x00\xfe\n', logs[0].read_bytes())
        process = r._read(str(logs[0]) + '.process.json')
        self.assertEqual(process["outcome"], "cancelled")
        self.assertTrue(process["direct_child_reaped"])
        self.assertFalse(process["cleanup_complete"])
        self.assertFalse((self.build / "nostrc-default-build-execution.json").exists())

    def test_configured_default_environment_configuration_and_runtime_provenance(self):
        check = self.default_contract()
        self.profile["configure_args"].append('-DCMAKE_BUILD_TYPE=Debug')
        check["command"] += ["--config", "Debug"]
        realized = self.configured_default()
        r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        check["environment"] = {"UNCONFIGURED_BUILD_POLICY": "changed"}
        with self.assertRaisesRegex(r.RealizationError, "configured build phase"):
            r._command_specs(self.inventory, realized)
        check["environment"] = {}
        check["command"][-1] = "Release"
        with self.assertRaisesRegex(r.RealizationError, "exact no-target"):
            r._command_specs(self.inventory, realized)
        check["command"][-1] = "Debug"
        unknown = self.build / "unknown-runtime"
        unknown.write_text('old unproven generated bytes')
        check["runtime_inputs"] = ["{build}/unknown-runtime"]
        audit = r.check_inputs(self.inventory, realized)
        self.assertFalse(audit["passed"])
        self.assertTrue(any(g["kind"] == "generated_runtime_without_target_or_file" for g in audit["gaps"]))

    def test_actual_default_cli_and_passing_aggregate_before_identity_negatives(self):
        check = self.default_contract()
        self.inventory["nodes"]["build.app"] = {"depends_on": ["application"]}
        check["nodes"] = ["build.app"]
        (self.root / "build.trigger").write_text('build contract input\n')
        check["runtime_inputs"] = ["{source}/build.trigger"]
        self.inventory["path_rules"].append({"exact": "build.trigger", "nodes": ["build.app"]})
        self.inventory.update(schema_version=1, profiles={"synthetic": self.profile}, release_components={})
        self.profile.update(platform={"darwin": "macos"}.get(sys.platform, sys.platform), architecture="native",
            compiler="clang" if sys.platform == "darwin" else "gcc", build_type="Debug",
            instrumentation="none", setup="default-mini-native", local_availability="host")
        self.profile["configure_args"].append('-DCMAKE_BUILD_TYPE=Debug')
        check["command"] += ["--config", "Debug"]
        for node in self.inventory["nodes"].values():
            node.update(owner_paths=["main.c"], rationale="mini-project")
        for rule in self.inventory["path_rules"]:
            rule.setdefault("effect", "impact")
            rule.setdefault("reason", "mini-project")
        for value in self.inventory["checks"].values():
            value.update(cost="ordinary", requirement="blocking", expected_outcome="pass")
        self.inventory["checks"]["a.model"] = self.inventory["checks"].pop("a-model")
        self.refresh_identity()
        self.git('-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'default build contract fixture')
        self.identity = {'kind': 'committed', 'sha': self.git('rev-parse', 'HEAD').decode().strip()}
        plan = s.make_plan(self.inventory, [{"status": "M", "old_path": "build.trigger", "new_path": "build.trigger"}],
                           head=self.identity["sha"], source_identity=self.identity)
        self.assertEqual(plan["selected_checks"], ["build.default"])
        planfile = self.root.parent / 'default-plan.json'
        r._atomic(planfile, plan)
        spec = importlib.util.spec_from_file_location("default_cli", ROOT / 'scripts/test-plan.py')
        cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cli)
        args = SimpleNamespace(plan=str(planfile), repo=str(self.root), profile='synthetic', configure=True,
            phase='all', build_dir=str(self.build), evidence_dir=str(self.evidence), command='run',
            resolved=None, parallel=1, timeout=20, attempt=1, evidence_path=[])
        receipt, code = cli.realization_command(args, self.inventory, r)
        self.assertEqual(code, 0)
        baseline = s.aggregate(self.inventory, plan, [receipt], complete=True)
        self.assertTrue(baseline['passed'], baseline)
        raw = receipt['backend_receipt']
        self.assertNotIn('--target', raw['commands'][0])
        for mutate in (
            lambda b: b.update(default_build_hash='0' * 64),
            lambda b: b['input_audit'].pop('default_build_execution'),
            lambda b: b['input_audit']['default_build_execution']['process']['argv'].append('--target=shared'),
            lambda b: b['input_audit']['default_build_execution']['process']['argv'].extend(['--', '-n']),
            lambda b: b['input_audit']['default_build_execution']['process']['argv'].extend(['--parallel', '8']),
            lambda b: b['checks']['build.default']['process'].update(argv=['cmake', '--build', 'other']),
            lambda b: b['checks']['build.default']['process'].update(exit_code=False),
            lambda b: b['commands'].remove(b['checks']['build.default']['argv']),
        ):
            altered = copy.deepcopy(receipt)
            mutate(altered['backend_receipt'])
            self.assertFalse(s.aggregate(self.inventory, plan, [altered], complete=True)['passed'])

    def test_cache_launcher_and_configured_ninja_identity_refuse(self):
        self.default_contract()
        self.profile['configure_args'].append('-DCMAKE_C_COMPILER_LAUNCHER=/usr/bin/env')
        realized = self.configured_default()
        self.assertTrue(any(g['kind'] == 'unsupported_default_cache_rule_override' for g in realized['default_build_binding']['gaps']))
        with self.assertRaisesRegex(r.RealizationError, 'unsupported default'):
            r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        self.profile['configure_args'].pop()
        # A fresh build directory avoids deliberately retained launcher cache.
        self.build = self.build.parent / 'fresh-build'
        realized = self.configured_default()
        cache = self.build / 'CMakeCache.txt'
        original = cache.read_text()
        cache.write_text(r.re.sub(r'CMAKE_MAKE_PROGRAM:FILEPATH=.*', 'CMAKE_MAKE_PROGRAM:FILEPATH=/unadmitted/ninja', original))
        with self.assertRaisesRegex(r.RealizationError, 'stale realization'):
            r.load_realization(self.build)
        with self.assertRaisesRegex(r.RealizationError, 'configured/PATH build tool identity'):
            r._default_snapshot(realized)

    def test_default_prerequisite_replay_rejects_target_equals_and_dry_run(self):
        self.default_contract()
        realized = self.configured_default()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        r.build_resolved(resolved, self.build)
        record_path = self.build / 'nostrc-default-build-execution.json'
        original = r._read(record_path)
        sidecar = original['log'] + '.process.json'
        for suffix in (['--target=shared'], ['--', '-n'], ['--parallel', '9']):
            altered = copy.deepcopy(original)
            altered['process']['argv'] += suffix
            r._atomic(record_path, altered)
            r._atomic(sidecar, altered['process'])
            with self.assertRaisesRegex(r.RealizationError, 'physical default prerequisite build evidence changed'):
                r._default_build_execution(realized)
        r._atomic(record_path, original)
        r._atomic(sidecar, original['process'])
        self.assertTrue(self.audit(realized)['passed'])

    def test_default_scope_rejects_target_and_command_laundering(self):
        check = self.default_contract()
        realized = self.configured_default()
        for argv in (["cmake", "--build", "{build}", "--target", "all"],
                     ["cmake", "--build", "{build}", "--parallel", "3"]):
            with self.subTest(argv=argv):
                check["command"] = argv
                with self.assertRaisesRegex(r.RealizationError, "exact no-target"):
                    r._command_specs(self.inventory, realized)
        check.update(kind="command", command=["cmake", "--build", "{build}"])
        del check["build_scope"]
        with self.assertRaisesRegex(r.RealizationError, "not test-phase"):
            r._command_specs(self.inventory, realized)


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

    def test_nak_declaration_presence_and_resolution_binding(self):
        from unittest import mock
        check = self.command(requires=["nak"])
        realized = self.configure()
        with self.assertRaisesRegex(r.RealizationError, "unavailable command capabilities"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        self.profile["capabilities"].append("nak")
        realized = self.configure()
        actual_which = r.shutil.which
        with mock.patch.object(r.shutil, "which", side_effect=lambda name, **kw: None if name == "nak" else actual_which(name, **kw)):
            with self.assertRaisesRegex(r.RealizationError, "actual nak executable"):
                r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        executable = actual_which("nak")
        if executable:
            resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
            proof = resolved["runtime_capability_evidence"]["nak"]
            self.assertEqual(proof['executable'], 'nak')
            self.assertEqual(proof['path'], str(Path(executable).resolve()))
            self.assertEqual(proof["sha256"], r.hashlib.sha256(Path(executable).read_bytes()).hexdigest())
            self.build_all()
            audit = self.audit(realized)
            self.assertTrue(audit["passed"], audit["gaps"])
            resolved["runtime_capability_evidence"]["nak"]["sha256"] = '0' * 64
            with self.assertRaisesRegex(r.RealizationError, "runtime capability executable changed"):
                r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertFalse((self.build / 'proof.txt').exists())

    def test_c_ordinary_tool_receipt_binds_real_nak_not_keyring_custody(self):
        from unittest import mock
        import subprocess
        self.profile['capabilities'] += ['nak', 'gnome_keyring']
        realized = self.configure()
        realized['profile_id'] = 'groundhog.linux'  # reader contract, not profile admission
        realized['source_identity'] = {'kind': 'committed', 'sha': self.git('rev-parse', 'HEAD').decode().strip()}
        executable = r.shutil.which('nak')
        if not executable:
            with self.assertRaisesRegex(r.RealizationError, 'actual nak executable'):
                r._runtime_capability_evidence(realized, ['nak'])
            return
        path = Path(executable).resolve()
        version = subprocess.run([str(path), '--version'], capture_output=True, text=True, check=True)
        tool = {'executable': 'nak', 'path': str(path), 'sha256': r.hashlib.sha256(path.read_bytes()).hexdigest(),
                'version_stdout': version.stdout, 'version_stderr': version.stderr}
        directory = self.root.parent / 'runner'
        receipt_path = directory / 'dependency/ordinary-tool-setup.json'
        setup = {'schema_version': 1, 'kind': 'ordinary_tool_setup_v1', 'profile': 'groundhog.linux',
            'source_identity': realized['source_identity'], 'workflow_sha': realized['source_identity']['sha'],
            'plan_id': 'a' * 64, 'run_id': '123', 'run_attempt': '1', 'setup_verified': True,
            'scope': 'executables_only_not_bus_display_or_profile_acceptance', 'tools': {'nak': tool}}
        with mock.patch.dict(os.environ, RUNNER_TEMP=str(directory), GITHUB_RUN_ID='123', GITHUB_RUN_ATTEMPT='1',
                             GITHUB_WORKFLOW_SHA=realized['source_identity']['sha']):
            r._atomic(receipt_path, setup)
            if not r.re.search(r'(?:^|\s)v?0\.19\.7(?:\s|$)', version.stdout + version.stderr):
                with self.assertRaisesRegex(r.RealizationError, 'version fingerprint'):
                    r._runtime_capability_evidence(realized, ['nak'], 'a' * 64)
                return
            proof = r._runtime_capability_evidence(realized, ['nak'], 'a' * 64)['nak']
            self.assertEqual(proof['setup_receipt']['record'], setup)
            self.assertEqual(proof['path'], str(path))
            for key, value in (('plan_id', 'b' * 64), ('workflow_sha', '0' * 40), ('profile', 'grotto.linux'),
                               ('scope', 'all_runtime_setup'), ('setup_verified', False)):
                altered = copy.deepcopy(setup)
                altered[key] = value
                r._atomic(receipt_path, altered)
                with self.assertRaisesRegex(r.RealizationError, 'setup receipt source/profile/plan/workflow/run identity'):
                    r._runtime_capability_evidence(realized, ['nak'], 'a' * 64)
            r._atomic(receipt_path, setup)
            with mock.patch.object(r.shutil, 'which', return_value=None):
                with self.assertRaisesRegex(r.RealizationError, 'actual gnome-keyring-daemon executable'):
                    r._runtime_capability_evidence(realized, ['gnome_keyring'], 'a' * 64)

    def test_ctest_tool_capability_does_not_grant_command_service_or_implicit_bus(self):
        from unittest import mock
        path = self.root / 'CMakeLists.txt'
        original = path.read_text()
        path.write_text(original.replace('TARGETS shared INPUTS vector.txt)', 'TARGETS shared INPUTS vector.txt REQUIRES nak)'))
        realized = self.configure()
        with self.assertRaisesRegex(r.RealizationError, "unavailable selected capabilities.*nak"):
            r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        path.write_text(original.replace('TARGETS shared INPUTS vector.txt)', 'TARGETS shared INPUTS vector.txt REQUIRES gnome_keyring)'))
        self.profile['capabilities'] += ['gnome_keyring']
        realized = self.configure()
        with mock.patch.object(r.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(r.RealizationError, 'actual gnome-keyring-daemon executable'):
                r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        # Validator-only mock: real daemon and fixture lifecycle are exercised in
        # tests/custody/test_keyring_runtime.py, not inferred from this tool proof.
        with mock.patch.object(r.shutil, 'which', return_value=sys.executable):
            resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        self.assertEqual(resolved['required_capabilities'], ['gnome_keyring'])
        self.assertNotIn('keyring_runtime', resolved)
        self.command(requires=['gnome_keyring'])
        realized = self.configure()
        with self.assertRaisesRegex(r.RealizationError, 'adapter not implemented'):
            r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        self.assertFalse((self.build / 'proof.txt').exists())

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
