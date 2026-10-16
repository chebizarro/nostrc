from realization_support import Project, r


class SupportInputTests(Project):
    def add_support(self, name, node, properties="", extra=""):
        script = name + ".cmake"
        (self.root / script).write_text('message(STATUS "support input")\n')
        self.inventory["nodes"][node] = {}
        self.inventory["path_rules"].append({"exact": script, "nodes": [node]})
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("nostrc_finalize_test_inventory()", f'''
add_test(NAME {name} COMMAND ${{CMAKE_COMMAND}} -P ${{CMAKE_CURRENT_SOURCE_DIR}}/{script})
nostrc_describe_test({name} OWNER {node} INPUTS {script} EXPECTED fixture)
{properties}
{extra}
nostrc_finalize_test_inventory()
'''))

    def consumer_gaps(self, audit, primary, support, path):
        return [g for g in audit["gaps"] if g["kind"] == "missing_impact_edge"
                and g.get("test") == primary and g.get("support_test") == support
                and g.get("input") == path]

    def test_setup_only_input_requires_each_consumer_impact_path(self):
        # The support itself is owned, but changing testing cannot select this
        # primary through application -> core/vectors. Other checks stay covered.
        self.inventory["checks"]["a-model"]["nodes"].remove("testing")
        realized = self.configure()
        audit = r.check_inputs(self.inventory, realized)
        self.assertFalse(audit["passed"])
        self.assertTrue(self.consumer_gaps(audit, "a-model", "reset", "setup.cmake"))
        self.assertFalse(self.consumer_gaps(audit, "gui", "reset", "setup.cmake"))
        self.assertFalse(any(g.get("test") == "reset" for g in audit["gaps"]))
        self.inventory["checks"]["a-model"]["nodes"].append("testing")
        self.assertTrue(r.check_inputs(self.inventory, realized)["passed"])
        self.assertNotIn("testing", self.inventory["nodes"]["application"]["depends_on"])

    def test_cleanup_only_input_cannot_borrow_cleanup_ownership(self):
        self.add_support("only-cleanup", "cleanup-inputs",
                         "set_tests_properties(only-cleanup PROPERTIES FIXTURES_CLEANUP private)")
        realized = self.configure()
        audit = r.check_inputs(self.inventory, realized)
        self.assertTrue(self.consumer_gaps(audit, "a-model", "only-cleanup", "only-cleanup.cmake"))
        for check in self.inventory["checks"].values():
            check["nodes"].append("cleanup-inputs")
        self.assertTrue(r.check_inputs(self.inventory, realized)["passed"])

    def test_chained_fixture_setup_and_cleanup_are_consumer_inputs(self):
        self.add_support("outer-setup", "outer-inputs", """
set_tests_properties(outer-setup PROPERTIES FIXTURES_SETUP outer FIXTURES_REQUIRED deepest)
set_tests_properties(reset PROPERTIES FIXTURES_REQUIRED outer)
""")
        self.add_support("deep-setup", "deep-inputs",
                         "set_tests_properties(deep-setup PROPERTIES FIXTURES_SETUP deepest)")
        self.add_support("outer-cleanup", "outer-inputs",
                         "set_tests_properties(outer-cleanup PROPERTIES FIXTURES_CLEANUP outer)")
        realized = self.configure()
        audit = r.check_inputs(self.inventory, realized)
        for name in ("outer-setup", "deep-setup", "outer-cleanup"):
            self.assertTrue(self.consumer_gaps(audit, "a-model", name, name + ".cmake"))
        for check in self.inventory["checks"].values():
            check["nodes"] += ["outer-inputs", "deep-inputs"]
        self.assertTrue(r.check_inputs(self.inventory, realized)["passed"])

    def test_authored_depends_only_inputs_are_not_fixture_exemptions(self):
        self.add_support("order-only", "ordering-inputs",
                         extra="set_tests_properties(a-model PROPERTIES DEPENDS order-only)")
        realized = self.configure()
        audit = r.check_inputs(self.inventory, realized)
        self.assertTrue(self.consumer_gaps(audit, "a-model", "order-only", "order-only.cmake"))
        self.assertFalse(self.consumer_gaps(audit, "gui", "order-only", "order-only.cmake"))
        self.inventory["checks"]["a-model"]["nodes"].append("ordering-inputs")
        self.assertTrue(r.check_inputs(self.inventory, realized)["passed"])

    def test_multiple_consumer_owners_are_not_pooled(self):
        self.inventory["nodes"]["other-app"] = {"depends_on": ["core", "vectors"]}
        self.inventory["checks"]["omitted"]["nodes"] = ["other-app"]
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("nostrc_finalize_test_inventory()", """
nostrc_describe_test(omitted OWNER other-app TARGETS shared INPUTS vector.txt)
nostrc_finalize_test_inventory()
"""))
        # Descriptors prohibit duplicate authorship, so adjust the actual loop.
        cmake.write_text(cmake.read_text().replace("foreach(t a-model gui omitted)", "foreach(t a-model gui)")
                         .replace("nostrc_describe_test(omitted OWNER", "set_tests_properties(omitted PROPERTIES FIXTURES_REQUIRED private)\nnostrc_describe_test(omitted OWNER"))
        self.inventory["nodes"]["other-app"]["depends_on"].append("application")
        realized = self.configure()
        audit = r.check_inputs(self.inventory, realized)
        self.assertTrue(self.consumer_gaps(audit, "omitted", "reset", "setup.cmake"))
        self.assertFalse(self.consumer_gaps(audit, "a-model", "reset", "setup.cmake"))
        self.inventory["checks"]["omitted"]["nodes"].append("testing")
        self.assertTrue(r.check_inputs(self.inventory, realized)["passed"])

    def test_multiple_support_input_owners_require_a_real_path_per_consumer(self):
        self.inventory["nodes"]["alternative-fixture"] = {}
        rule = next(x for x in self.inventory["path_rules"] if x.get("exact") == "setup.cmake")
        rule["nodes"].append("alternative-fixture")
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("reset OWNER testing INPUTS", "reset OWNER testing alternative-fixture INPUTS"))
        self.inventory["checks"]["a-model"]["nodes"].remove("testing")
        realized = self.configure()
        audit = r.check_inputs(self.inventory, realized)
        self.assertTrue(self.consumer_gaps(audit, "a-model", "reset", "setup.cmake"))
        # A change maps to both owners, so either actual selection path suffices.
        self.inventory["checks"]["a-model"]["nodes"].append("alternative-fixture")
        audit = r.check_inputs(self.inventory, realized)
        self.assertFalse(self.consumer_gaps(audit, "a-model", "reset", "setup.cmake"))
        # Other private fixture inputs still require testing; one multi-owner
        # input is not a waiver for the rest of the closure.
        self.assertTrue(self.consumer_gaps(audit, "a-model", "second-setup", "noop.cmake"))

    def test_support_target_sources_and_discovered_headers_are_consumer_inputs(self):
        (self.root / "support.h").write_text("#define SUPPORT_VALUE 0\n")
        (self.root / "support.c").write_text('#include "support.h"\nint main(void) { return SUPPORT_VALUE; }\n')
        self.inventory["nodes"]["helper"] = {"depends_on": ["helper-header"]}
        self.inventory["nodes"]["helper-header"] = {}
        self.inventory["path_rules"] += [{"exact": "support.c", "nodes": ["helper"]},
                                          {"exact": "support.h", "nodes": ["helper-header"]}]
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("nostrc_finalize_test_inventory()", """
add_executable(helper support.c)
add_test(NAME native-support COMMAND helper)
set_tests_properties(native-support PROPERTIES FIXTURES_SETUP private)
nostrc_describe_test(native-support OWNER helper TARGETS helper EXPECTED fixture)
nostrc_finalize_test_inventory()
"""))
        realized = self.configure()
        self.build_all()
        audit = self.audit(realized)
        self.assertTrue(self.consumer_gaps(audit, "a-model", "native-support", "support.c"))
        self.assertTrue(any(g["input_kind"] == "header" for g in
                            self.consumer_gaps(audit, "a-model", "native-support", "support.h")))
        for check in self.inventory["checks"].values():
            check["nodes"].append("helper")
        self.assertTrue(self.audit(realized)["passed"])

    def test_unknown_support_registration_and_missing_fixture_stay_blocking(self):
        realized = self.configure()
        realized["descriptors"] = [d for d in realized["descriptors"] if d["name"] != "reset"]
        self.assertFalse(r.check_inputs(self.inventory, realized)["passed"])
        realized["tests"] = [t for t in realized["tests"] if t["name"] not in ("reset", "second-setup")]
        realized["descriptors"] = [d for d in realized["descriptors"] if d["name"] != "second-setup"]
        audit = r.check_inputs(self.inventory, realized)
        self.assertTrue(any(g["kind"] == "support_closure" for g in audit["gaps"]))
