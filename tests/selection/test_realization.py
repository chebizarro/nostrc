from unittest import mock
from realization_support import Project, ROOT, r


class RealizationTests(Project):
    def test_fresh_query_before_configure_and_load(self):
        actual = r._command
        def checked(argv, **kwargs):
            if argv[0] == "cmake":
                self.assertTrue((self.build / ".cmake/api/v1/query/client-nostrc-test-selection/codemodel-v2").exists())
            return actual(argv, **kwargs)
        with mock.patch.object(r, "_command", side_effect=checked):
            realized = self.configure()
        self.assertEqual(r.load_realization(self.build, self.identity), realized)
        self.assertEqual(len(realized["descriptors"]), 7)
        self.assertTrue(any(t["name"] == "shared" for t in realized["targets"].values()))

    def test_failed_configure_invalidates_previous_realization(self):
        self.configure()
        with (self.root / "CMakeLists.txt").open("a") as f:
            f.write('\nmessage(FATAL_ERROR "deliberate failure")\n')
        with self.assertRaises(r.RealizationError):
            self.configure()
        with self.assertRaises(r.RealizationError):
            r.load_realization(self.build)

    def test_stale_and_missing_replies_are_not_empty_success(self):
        self.configure()
        with mock.patch.object(r, "_command", return_value=""):
            with self.assertRaisesRegex(r.RealizationError, "stale File API"):
                self.configure()
        for path in (self.build / ".cmake/api/v1/reply").glob("index-*.json"):
            path.unlink()
        with mock.patch.object(r, "_command", return_value=""):
            with self.assertRaisesRegex(r.RealizationError, "File API"):
                self.configure()

    def test_native_reconfigure_invalidates_load(self):
        self.configure()
        r._command(["cmake", "-S", str(self.root), "-B", str(self.build),
                    "-DNOSTRC_TEST_SELECTION_TOKEN=external-configure"])
        with self.assertRaisesRegex(r.RealizationError, "stale"):
            r.load_realization(self.build)

    def test_dirty_identity_is_not_just_head(self):
        one = self.configure()
        changed = dict(self.identity, snapshot_digest="dirty-two")
        with self.assertRaisesRegex(r.RealizationError, "identity changed"):
            r.load_realization(self.build, changed)
        self.identity = changed
        two = self.configure()
        self.assertNotEqual(one["profile_fingerprint"], two["profile_fingerprint"])

    def test_fixture_order_and_model_gui_distinction(self):
        realized = self.configure()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        self.assertEqual(resolved["primary_tests"], ["a-model"])
        self.assertEqual(resolved["support_tests"], ["cleanup", "reset", "second-setup", "z-first"])
        self.assertNotIn("gui", resolved["tests"])
        self.plan["selected_checks"].append("gui")
        resolved = r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        self.assertEqual(resolved["primary_tests"], ["a-model", "gui"])
        self.assertEqual(resolved["build_targets"], ["shared"])

    def test_missing_and_duplicate_primary_registration(self):
        realized = self.configure()
        self.inventory["checks"]["a-model"]["ctest_tests"]["synthetic"] = ["missing"]
        with self.assertRaisesRegex(r.RealizationError, "missing selected"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")
        self.inventory["checks"]["a-model"]["ctest_tests"]["synthetic"] = ["gui"]
        with self.assertRaisesRegex(r.RealizationError, "multiply claimed"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")

    def test_duplicate_descriptor_override_authority_fails(self):
        realized = self.configure()
        self.inventory["test_overrides"] = [{"name": "a-model", "owner": ["application"]}]
        with self.assertRaisesRegex(r.RealizationError, "duplicate.*authority"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")

    def test_missing_target_and_wrapped_command_require_metadata(self):
        realized = self.configure()
        for desc in realized["descriptors"]:
            if desc["name"] == "a-model":
                desc["targets"] = ["not-a-target"]
        with self.assertRaisesRegex(r.RealizationError, "unknown descriptor target"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")

    def test_ambiguous_command_does_not_guess_from_test_name(self):
        realized = self.configure()
        realized["descriptors"] = [d for d in realized["descriptors"] if d["name"] != "a-model"]
        for test in realized["tests"]:
            if test["name"] == "a-model":
                test["command"] = ["/usr/bin/env", str(self.build / "shared")]
        with self.assertRaisesRegex(r.RealizationError, "ambiguous/unmapped"):
            r.resolve_checks(self.inventory, self.plan, realized, "synthetic")

    def test_root_and_standalone_libgo_finalize_guards(self):
        # Full native libgo/root commands are a separate focused validation, not
        # simulated by these structural assertions or the tiny project above.
        root = (ROOT / "CMakeLists.txt").read_text()
        libgo = (ROOT / "libgo/CMakeLists.txt").read_text()
        self.assertEqual(root.count("nostrc_finalize_test_inventory()"), 1)
        self.assertIn('if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)\n  nostrc_finalize_test_inventory()', libgo)
        self.assertIn('include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NostrcTestInventory.cmake")', libgo)

    def test_generated_command_recovery_is_literal_not_name_mapping(self):
        self.build = self.build.parent / "build with spaces"
        realized = self.configure()
        model = next(t for t in realized["tests"] if t["name"] == "a-model")
        self.assertEqual(model["command"], [str(self.build / "shared")])
        self.assertFalse((self.build / "shared").exists())
        self.assertEqual(r._mapped_targets(model, {}, realized),
                         [next(t["id"] for t in realized["targets"].values() if t["name"] == "shared")])

    def test_generated_command_lexer_preserves_quoted_arguments(self):
        text = r'''[=[odd name]=] "/a b/exec" "quote\"line\n" "\$literal" "semi;colon")'''
        self.assertEqual(r._cmake_literal_arguments(text, 0),
                         ["odd name", "/a b/exec", 'quote"line\n', "$literal", "semi;colon"])
        with self.assertRaisesRegex(r.RealizationError, "nonliteral"):
            r._cmake_literal_arguments('"name" "${dynamic}/exec")', 0)

    def test_native_profile_cannot_mislabel_platform_architecture_or_build(self):
        import platform
        other = "windows" if platform.system() != "Windows" else "linux"
        with self.assertRaisesRegex(r.RealizationError, "platform"):
            r._validate_native_profile({"platform": other})
        with self.assertRaisesRegex(r.RealizationError, "architecture"):
            r._validate_native_profile({"architecture": "not-this-host"})
        self.profile["build_type"] = "Release"
        with self.assertRaisesRegex(r.RealizationError, "build type"):
            self.configure()
        self.assertFalse((self.build / "nostrc-test-realization.json").exists())

    def test_component_directory_ownership_does_not_use_execution_name(self):
        for directory, name in (("nips/nip44", "unrelated-looking"), ("mystery", "nip44_test")):
            path = self.root / directory
            path.mkdir(parents=True)
            (path / "CMakeLists.txt").write_text(f'add_test(NAME {name} COMMAND ${{CMAKE_COMMAND}} -E true)\n')
        cmake = self.root / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("nostrc_finalize_test_inventory()",
            "add_subdirectory(nips/nip44)\nadd_subdirectory(mystery)\nnostrc_finalize_test_inventory()"))
        realized = self.configure()
        descriptors = {d["name"]: d for d in realized["descriptors"]}
        self.assertEqual(descriptors["unrelated-looking"]["owner"], ["nip44"])
        self.assertEqual(descriptors["unrelated-looking"]["owner_authority"], "component_directory")
        self.assertEqual(descriptors["nip44_test"]["owner"], [])
        self.assertEqual(descriptors["nip44_test"]["owner_authority"], "unresolved")

    def test_groundhog_packaging_and_late_graph_contract(self):
        text = (ROOT / "gnome/groundhog/TestInventory.cmake").read_text()
        for runtime in ("flake.nix", "packaging/archlinux/groundhog/PKGBUILD",
                        "packaging/rpm/groundhog.spec", "packaging/debian-desktop/changelog",
                        "CheckPackagingVersion.cmake"):
            self.assertIn(runtime, text)
        self.assertIn("groundhog-mls-ui-gui test-groundhog-mls-ui", text)
        cmake = (ROOT / "gnome/groundhog/CMakeLists.txt").read_text()
        self.assertGreater(cmake.index('include("${CMAKE_CURRENT_SOURCE_DIR}/TestInventory.cmake")'),
                           cmake.index("target_link_libraries(test-groundhog-mdk011-interop PRIVATE groundhog-voice"))
        self.assertIn("src/mls/gh-mls-invitee.c src/ui/gh-mls-copy.c", cmake)
