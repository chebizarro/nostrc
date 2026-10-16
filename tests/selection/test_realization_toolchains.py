"""Native compiler proof follows used codemodel languages, not idle CXX slots."""
import copy
import unittest
from realization_support import Project, r


def tools(c="AppleClang", cxx=None):
    return {"toolchains": [{"language": "ASM", "compiler": {"path": "/usr/bin/cc"}},
                           {"language": "C", "compiler": {"id": c, "path": "/usr/bin/cc"}},
                           {"language": "CXX", "compiler": {"id": cxx, "path": "/usr/bin/c++"}}]}


class ToolchainIdentityTests(unittest.TestCase):
    def test_unused_absent_cxx_and_asm_identity_do_not_invalidate_actual_c(self):
        r._validate_native_profile({"compiler": "clang"}, tools(), compiled_languages=["C", "ASM"])
        r._validate_native_profile({"compiler": "clang"}, tools(cxx="GNU"), compiled_languages=["C"])

    def test_active_absent_cxx_identity_fails_closed_without_typeerror(self):
        for missing in ({}, {"id": None}, None, {"id": ""}, {"id": 5}):
            with self.subTest(missing=missing):
                value = tools()
                value["toolchains"][2]["compiler"] = missing
                with self.assertRaisesRegex(r.RealizationError, "missing compiler identity.*CXX"):
                    r._validate_native_profile({"compiler": "clang"}, value, compiled_languages=["C", "CXX"])

    def test_active_c_absent_mismatched_or_duplicate_toolchain_never_passes(self):
        with self.assertRaisesRegex(r.RealizationError, "active language: C"):
            r._validate_native_profile({"compiler": "clang"}, tools(c=None), compiled_languages=["C"])
        with self.assertRaisesRegex(r.RealizationError, "differs from realized"):
            r._validate_native_profile({"compiler": "clang"}, tools(cxx="GNU"), compiled_languages=["C", "CXX"])
        missing = tools()
        missing["toolchains"] = [t for t in missing["toolchains"] if t["language"] != "C"]
        duplicate = tools()
        duplicate["toolchains"].append(copy.deepcopy(duplicate["toolchains"][1]))
        for value in (missing, duplicate):
            with self.assertRaisesRegex(r.RealizationError, "missing/ambiguous active toolchain: C"):
                r._validate_native_profile({"compiler": "clang"}, value, compiled_languages=["C"])

    def test_no_native_or_malformed_compiled_language_proof_is_not_a_compiler_pass(self):
        for languages in ([], ["ASM"], [None], "C", {}):
            with self.subTest(languages=languages), self.assertRaises(r.RealizationError):
                r._validate_native_profile({"compiler": "clang"}, tools(), compiled_languages=languages)

    def test_legacy_cache_with_absent_identity_needs_fresh_codemodel_evidence(self):
        with self.assertRaisesRegex(r.RealizationError, "compiled-language evidence required"):
            r._validate_native_profile({"compiler": "clang"}, tools())
        r._validate_native_profile({"compiler": "clang"}, tools(cxx="Clang"))

    def test_phase_specific_sanitizer_metadata_cannot_be_silently_flattened(self):
        profile = {"compiler": "clang", "phase_environments": {
            "configure": {}, "build": {}, "test": {"ASAN_OPTIONS": "detect_leaks=1"}}}
        r._validate_native_profile(profile, tools(), compiled_languages=["C"])
        self.assertNotIn("ASAN_OPTIONS", r.phase_environment(profile, "/source", "configure", {}))
        self.assertNotIn("ASAN_OPTIONS", r.phase_environment(profile, "/source", "build", {}))
        self.assertEqual(r.phase_environment(profile, "/source", "test", {})["ASAN_OPTIONS"], "detect_leaks=1")
        profile["sanitizer_compatibility"] = {"GH_SAN_CONFIGURE_ENV": [], "GH_SAN_ENV": ["ASAN_OPTIONS=detect_leaks=1"]}
        with self.assertRaisesRegex(r.RealizationError, "invalid sanitizer_compatibility"):
            r._validate_native_profile(profile, tools(), compiled_languages=["C"])


class NativeCodemodelLanguageTests(Project):
    def test_actual_codemodel_languages_are_retained_and_revalidated_on_load(self):
        realized = self.configure()
        self.assertEqual(realized["compiled_languages"], ["C"])
        self.assertEqual(r.load_realization(self.build, self.identity), realized)
        value = copy.deepcopy(realized)
        value["profile"]["compiler"] = "clang" if any(t.get("compiler", {}).get("id") in ("Clang", "AppleClang") for t in value["toolchains"]["toolchains"]) else "gcc"
        value["compiled_languages"] = ["CXX"]
        # This native C-only fixture has no active CXX compiler evidence. Merely
        # naming it in a cached record cannot manufacture compiler acceptance.
        value["toolchains"]["toolchains"] = [t for t in value["toolchains"]["toolchains"] if t["language"] != "CXX"]
        r._atomic(self.build / "nostrc-test-realization.json", value)
        with self.assertRaisesRegex(r.RealizationError, "active toolchain: CXX"):
            r.load_realization(self.build, self.identity)
