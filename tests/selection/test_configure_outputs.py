"""Real configure-time D-Bus outputs, not invented build-target provenance."""
import copy
import json
import shutil

from realization_support import Project, ROOT, r


class ConfigureOutputTests(Project):
    names = ("grotto_app_dbus_service_configured", "grotto_dbus_service_configured")
    files = ("org.nostr.Grotto.service", "org.nostr.Signer.service")

    def setUp(self):
        super().setUp()
        app = self.root / "app"
        app.mkdir()
        shutil.copy2(ROOT / "cmake/CheckDbusServiceFile.cmake", self.root / "cmake")
        for file in self.files:
            shutil.copy2(ROOT / "apps/grotto/data" / (file + ".in"), app)
        (app / "CMakeLists.txt").write_text('''set(CMAKE_INSTALL_FULL_BINDIR /usr/bin)
configure_file(org.nostr.Grotto.service.in ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Grotto.service @ONLY)
configure_file(org.nostr.Signer.service.in ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Signer.service @ONLY)
foreach(pair "grotto_app_dbus_service_configured|org.nostr.Grotto.service" "grotto_dbus_service_configured|org.nostr.Signer.service")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 name)
  list(GET pair 1 file)
  add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -DSERVICE_FILE=${CMAKE_CURRENT_BINARY_DIR}/${file} -P ${CMAKE_SOURCE_DIR}/cmake/CheckDbusServiceFile.cmake)
  nostrc_describe_test(${name} OWNER application INPUTS
    "${CMAKE_SOURCE_DIR}/cmake/CheckDbusServiceFile.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/${file}.in" "${CMAKE_CURRENT_BINARY_DIR}/${file}")
endforeach()
''')
        p = self.root / "CMakeLists.txt"
        p.write_text(p.read_text().replace("nostrc_finalize_test_inventory()", "add_subdirectory(app)\nnostrc_finalize_test_inventory()"))
        self.inventory["path_rules"] += [
            {"exact": "app/CMakeLists.txt", "nodes": [], "effect": "global"},
            {"prefix": "app/", "nodes": ["application"]}]
        for name in self.names:
            self.inventory["checks"][name] = {"kind": "ctest", "profiles": ["synthetic"],
                                              "ctest_tests": [name], "nodes": ["application"]}

    def output(self, index=0):
        return self.build / "app" / self.files[index]

    def gaps(self, realized, after_build=False):
        return r.check_inputs(self.inventory, realized, after_build=after_build)["gaps"]

    def test_actual_services_pre_post_audit_and_execution_without_fake_targets(self):
        realized = self.configure()
        self.assertEqual(set(realized["configure_outputs"]), {str(self.output(i)) for i in range(2)})
        self.assertEqual({t["name"] for t in realized["targets"].values()}, {"shared"})
        self.assertEqual(self.gaps(realized), [])
        for i, name in enumerate(self.names):
            desc = next(d for d in realized["descriptors"] if d["name"] == name)
            test = next(t for t in realized["tests"] if t["name"] == name)
            self.assertEqual(r._mapped_targets(test, desc, realized), [])
            spec = realized["configure_outputs"][str(self.output(i))]
            self.assertEqual(spec["producer"], str(self.root / "app/CMakeLists.txt"))
            self.assertEqual(spec["source"], str(self.root / "app" / (self.files[i] + ".in")))
            plan = dict(self.plan, selected_checks=[name])
            resolved = r.resolve_checks(self.inventory, plan, realized, "synthetic")
            self.assertEqual(resolved["build_targets"], [])
        self.build_all()
        audit = self.audit(realized)
        self.assertTrue(audit["passed"], audit["gaps"])
        self.assertGreater(audit["header_entries"], 0)
        for i in range(2):
            self.assertIn(str(self.output(i)), audit["input_hashes"])
            self.assertIn(str(self.root / "app" / (self.files[i] + ".in")), audit["input_hashes"])
        r._command(["ctest", "--test-dir", str(self.build), "-R", "^grotto_.*service_configured$", "--output-on-failure"])
        self.assertEqual(r.load_realization(self.build, self.identity), realized)

    def test_omitted_services_keep_source_and_producer_impact_audits(self):
        realized = self.configure()
        self.assertNotIn(self.names[0], r.resolve_checks(self.inventory, self.plan, realized, "synthetic")["tests"])
        # Derived provenance must audit the source even if a descriptor drops it.
        for desc in realized["descriptors"]:
            if desc["name"] in self.names:
                desc["inputs"] = [p for p in desc["inputs"] if not p.endswith(".in")]
        self.inventory["path_rules"].append({"exact": "app/org.nostr.Grotto.service.in", "nodes": ["other"]})
        gaps = self.gaps(realized)
        self.assertTrue(any(g["kind"] == "missing_impact_edge" and g.get("test") == self.names[0]
                            and g.get("input") == "app/org.nostr.Grotto.service.in" for g in gaps), gaps)
        self.inventory["path_rules"] = [rule for rule in self.inventory["path_rules"]
                                         if rule.get("exact") != "app/CMakeLists.txt"]
        self.inventory["path_rules"].append({"exact": "app/CMakeLists.txt", "nodes": ["other"]})
        self.assertTrue(any(g["kind"] == "missing_impact_edge" and g.get("input") == "app/CMakeLists.txt"
                            for g in self.gaps(realized)))

    def test_missing_or_changed_source_producer_and_output_fail_closed(self):
        realized = self.configure()
        for path in (self.root / "app/CMakeLists.txt", self.root / "app" / (self.files[0] + ".in"), self.output()):
            original = path.read_bytes()
            for mutation in (None, original + b"\n# changed\n"):
                with self.subTest(path=path, missing=mutation is None):
                    if mutation is None:
                        path.unlink()
                    else:
                        path.write_bytes(mutation)
                    gaps = self.gaps(realized)
                    self.assertTrue(any(g["kind"] == "stale_configure_output" and g["changed"] == str(path) for g in gaps), gaps)
                    with self.assertRaises(r.RealizationError):
                        r.load_realization(self.build, self.identity)
                    path.write_bytes(original)

    def test_trace_and_file_api_provenance_are_bound(self):
        realized = self.configure()
        bound = realized["generation_files"]
        paths = [p for p in bound if p.endswith("nostrc-configure-trace.jsonl") or "/cmakeFiles-v1-" in p]
        self.assertEqual(len(paths), 2)
        for value in paths:
            path = type(self.build)(value)
            original = path.read_bytes()
            path.write_bytes(original + b"\n")
            with self.assertRaisesRegex(r.RealizationError, "stale realization metadata"):
                r.load_realization(self.build, self.identity)
            path.write_bytes(original)
        changed = copy.deepcopy(realized)
        changed["configure_outputs"] = {}
        self.assertNotEqual(r._profile_fingerprint(changed), realized["profile_fingerprint"])
        changed["profile_fingerprint"] = r._profile_fingerprint(changed)
        self.assertEqual(sum(g["kind"] == "generated_runtime_without_target" for g in self.gaps(changed)), 2)

    def test_file_write_is_not_configure_output_provenance(self):
        p = self.root / "app/CMakeLists.txt"
        p.write_text(p.read_text().replace(
            "configure_file(org.nostr.Grotto.service.in ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Grotto.service @ONLY)",
            'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Grotto.service" "unproven")'))
        realized = self.configure()
        self.assertNotIn(str(self.output()), realized["configure_outputs"])
        self.assertTrue(any(g["kind"] == "generated_runtime_without_target" and g["test"] == self.names[0]
                            for g in self.gaps(realized)))

    def test_ambiguous_producers_and_included_modules_remain_unsupported(self):
        p = self.root / "app/CMakeLists.txt"
        original = p.read_text()
        call = "configure_file(org.nostr.Grotto.service.in ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Grotto.service @ONLY)"
        for replacement in (call + "\n" + call, "include(configure.cmake)",
                            call + "\ninclude(configure.cmake)"):
            with self.subTest(replacement=replacement):
                (p.parent / "configure.cmake").write_text(call)
                p.write_text(original.replace(call, replacement))
                realized = self.configure()
                self.assertNotIn(str(self.output()), realized["configure_outputs"])
                self.assertTrue(any(g["kind"] == "generated_runtime_without_target" and g["test"] == self.names[0]
                                    for g in self.gaps(realized)))

    def test_unresolved_relative_producer_cannot_overwrite_supported_output(self):
        p = self.root / "app/CMakeLists.txt"
        p.write_text(p.read_text() + '\ninclude(overwrite.cmake)\n')
        (p.parent / "other.in").write_text("different producer\n")
        (p.parent / "overwrite.cmake").write_text('configure_file(other.in org.nostr.Grotto.service @ONLY)\n')
        realized = self.configure()
        self.assertEqual(self.output().read_text(), "different producer\n")
        self.assertEqual(realized["configure_outputs"], {})
        self.assertTrue(any(g["kind"] == "generated_runtime_without_target" and g["test"] == self.names[0]
                            for g in self.gaps(realized)))

    def test_missing_file_api_input_or_directory_cannot_establish_provenance(self):
        realized = self.configure()
        reply = self.build / ".cmake/api/v1/reply"
        cmake_files = json.loads(next(reply.glob("cmakeFiles-v1-*.json")).read_text())
        codemodel = json.loads(next(reply.glob("codemodel-v2-*.json")).read_text())
        directories = codemodel["configurations"][0]["directories"]
        for remove in ("app/CMakeLists.txt", "app/org.nostr.Grotto.service.in"):
            evidence = copy.deepcopy(cmake_files)
            evidence["inputs"] = [p for p in evidence["inputs"] if p["path"] != remove]
            outputs = r._configure_file_outputs(self.build / "nostrc-configure-trace.jsonl", evidence,
                                               directories, self.root, self.build, realized["configure_outputs"])
            self.assertNotIn(str(self.output()), outputs)
        self.assertEqual(r._configure_file_outputs(self.build / "nostrc-configure-trace.jsonl", cmake_files,
                                                  [], self.root, self.build, realized["configure_outputs"]), {})

    def test_external_symlink_output_remains_blocking(self):
        realized = self.configure()
        outside = self.build.parent / "outside.service"
        outside.write_bytes(self.output().read_bytes())
        self.output().unlink()
        self.output().symlink_to(outside)
        self.assertTrue(any(g["kind"] == "stale_configure_output" for g in self.gaps(realized)))
        # Even identical bytes outside the admitted output path are not custody.
        with self.assertRaisesRegex(r.RealizationError, "stale realization metadata"):
            r.load_realization(self.build, self.identity)
