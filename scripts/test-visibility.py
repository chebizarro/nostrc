#!/usr/bin/env python3
"""Focused link/install regression for NostrcVisibility, shared and static."""
import pathlib
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


class VisibilityTests(unittest.TestCase):
    def test_compile_only_probes_recover_cached_failure(self):
        for language, extension in (("C", "c"), ("CXX", "cpp")):
            with self.subTest(language=language), tempfile.TemporaryDirectory(prefix="nostrc-visibility-probe-") as tmp:
                src = pathlib.Path(tmp)
                (src / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.22)
project(visibility_probe {language})
include("{ROOT}/cmake/NostrcVisibility.cmake")
# Simulate a configure retried after a missing sanitizer runtime was installed.
set(COMPILER_HAS_HIDDEN_VISIBILITY "" CACHE INTERNAL "" FORCE)
set(COMPILER_HAS_HIDDEN_INLINE_VISIBILITY "" CACHE INTERNAL "" FORCE)
# Compiler attributes must remain detectable even when executables cannot link.
set(CMAKE_EXE_LINKER_FLAGS "-lmissing_nostrc_sanitizer_runtime")
set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
add_library(fixture SHARED core.{extension})
nostrc_library_visibility(fixture FIXTURE_API fixture/export.h "include/fixture")
if(NOT CMAKE_TRY_COMPILE_TARGET_TYPE STREQUAL "EXECUTABLE")
  message(FATAL_ERROR "visibility helper leaked its compile-only setting")
endif()
file(READ "${{CMAKE_CURRENT_BINARY_DIR}}/include/fixture/export.h" exports)
if(NOT exports MATCHES "define FIXTURE_API __attribute__")
  message(FATAL_ERROR "empty FIXTURE_API after cached probe failure")
endif()
set(CMAKE_EXE_LINKER_FLAGS "")
add_executable(consumer main.{extension})
target_link_libraries(consumer PRIVATE fixture)
''')
                (src / f"core.{extension}").write_text(
                    '#include "fixture/export.h"\n'
                    'FIXTURE_API extern int public_data;\nint public_data = 42;\n')
                (src / f"main.{extension}").write_text(
                    'extern int public_data;\nint main(void) { return public_data != 42; }\n')
                build = src / "build"
                try:
                    run("cmake", "-S", str(src), "-B", str(build))
                    run("cmake", "--build", str(build))
                    run(str(build / "consumer"))
                except subprocess.CalledProcessError as exc:
                    self.fail(exc.stdout)

    def test_broken_visibility_probe_fails_configure(self):
        with tempfile.TemporaryDirectory(prefix="nostrc-visibility-probe-") as tmp:
            src = pathlib.Path(tmp)
            (src / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.22)
project(broken_visibility_probe C)
include("{ROOT}/cmake/NostrcVisibility.cmake")
set(CMAKE_REQUIRED_FLAGS "-fnostrc-invalid-compiler-flag")
add_library(fixture SHARED core.c)
nostrc_library_visibility(fixture FIXTURE_API fixture/export.h "include/fixture")
''')
            (src / "core.c").write_text('int public_data = 42;\n')
            result = subprocess.run(
                ["cmake", "-S", str(src), "-B", str(src / "build")],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("COMPILER_HAS_HIDDEN_VISIBILITY failed for fixture", result.stdout)
            self.assertIn("refusing empty", result.stdout)
            self.assertIn("toolchain", result.stdout)

    def test_disabled_glib_nip77_header(self):
        # This installed header is intentionally harmless without GLib enabled,
        # even when no optional GLib library/export header was generated.
        run("cc", "-E", "-x", "c", "-I" + str(ROOT / "nips/nip77/include"), "-",
            input='#include <nostr/nip77/negentropy-glib.h>\n')

    def test_nip_generator(self):
        with tempfile.TemporaryDirectory(prefix="nostrc-nip-generator-") as tmp:
            src = pathlib.Path(tmp)
            (src / "nips").mkdir()
            (src / "cmake").mkdir()
            shutil.copy(ROOT / "cmake/NostrcVisibility.cmake", src / "cmake")
            run("bash", str(ROOT / "nips/create_nip.sh"), "nip-fixture", cwd=src / "nips")
            run("bash", str(ROOT / "nips/generate_cmake.sh"), cwd=src)
            package = src / "nips/nip-fixture"
            header = package / "include/nostr/nip_fixture/nip-fixture.h"
            with header.open("a") as out:
                out.write('\nNOSTR_NIP_FIXTURE_API int fixture_api(void);\n')
            with (package / "src/nip-fixture.c").open("a") as out:
                out.write('int fixture_api(void) { return 7; }\n')
            (src / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.22)
project(generated_nip C)
add_library(nostr INTERFACE)
set(ENABLE_NIP_FIXTURE ON)
include(NipOptions.cmake)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE nip-fixture)
''')
            (src / "main.c").write_text('#include <nostr/nip_fixture/nip-fixture.h>\nint main(void) { return fixture_api() != 7; }\n')
            for shared in ("ON", "OFF"):
                build = src / ("build-" + shared)
                run("cmake", "-S", str(src), "-B", str(build), f"-DBUILD_SHARED_LIBS={shared}")
                run("cmake", "--build", str(build))
                run(str(build / "consumer"))

    def test_shared_and_static_consumers(self):
        for kind, shared in (("", "ON"), ("STATIC", "OFF"), ("OBJECT", "OFF")):
            with self.subTest(kind=kind, shared=shared), tempfile.TemporaryDirectory(prefix="nostrc-visibility-") as tmp:
                src = pathlib.Path(tmp)
                (src / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.22)
project(visibility_fixture C)
include("{ROOT}/cmake/NostrcVisibility.cmake")
add_library(fixture {kind} core.c)
nostrc_library_visibility(fixture FIXTURE_API fixture/export.h "include/fixture")
target_include_directories(fixture PUBLIC "${{CMAKE_CURRENT_SOURCE_DIR}}")
install(TARGETS fixture DESTINATION lib)
install(FILES api.h DESTINATION include)
add_library(bridge SHARED bridge.c)
# Match libjson's supported ELF static-core-from-executable arrangement.
if(NOT BUILD_SHARED_LIBS AND NOT "{kind}" STREQUAL "OBJECT" AND UNIX AND NOT APPLE)
  target_include_directories(bridge PRIVATE $<TARGET_PROPERTY:fixture,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(bridge PRIVATE $<TARGET_PROPERTY:fixture,INTERFACE_COMPILE_DEFINITIONS>)
  target_link_libraries(bridge INTERFACE fixture)
else()
  target_link_libraries(bridge PRIVATE fixture)
endif()
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE bridge fixture)
''')
                (src / "api.h").write_text('#include "fixture/export.h"\nFIXTURE_API int public_api(void);\n')
                (src / "core.c").write_text('#include "api.h"\nint private_impl(void) { return 42; }\nint public_api(void) { return private_impl(); }\n')
                (src / "bridge.c").write_text('#include "api.h"\nint bridge(void) { return public_api(); }\n')
                (src / "main.c").write_text('int bridge(void); int main(void) { return bridge() != 42; }\n')
                build = src / "build"
                prefix = src / "prefix"
                try:
                    run("cmake", "-S", str(src), "-B", str(build),
                        f"-DBUILD_SHARED_LIBS={shared}", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
                        f"-DCMAKE_INSTALL_PREFIX={prefix}")
                    run("cmake", "--build", str(build), "-j2")
                    run(str(build / "consumer"))
                    run("cmake", "--install", str(build))
                    # An installed header must not rely on source/build-tree paths.
                    run("cc", "-Werror", "-I" + str(prefix / "include"), "-x", "c", "-c", "-",
                        "-o", str(src / "installed.o"),
                        input='#include <api.h>\nint use_api(void) { return public_api(); }\n')
                    if (shared == "ON" or kind == "OBJECT") and platform.system() == "Linux":
                        library = "libbridge.so" if kind == "OBJECT" else "libfixture.so"
                        symbols = run("nm", "-D", "--defined-only", str(build / library))
                        self.assertIn(" public_api\n", symbols)
                        self.assertNotIn("private_impl", symbols)
                except subprocess.CalledProcessError as exc:
                    self.fail(exc.stdout)

    def test_hyphenated_target_static_define(self):
        # Target names such as marmot-gobject must yield valid C macro names.
        with tempfile.TemporaryDirectory(prefix="nostrc-visibility-") as tmp:
            src = pathlib.Path(tmp)
            (src / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.22)
project(hyphenated_fixture C)
include("{ROOT}/cmake/NostrcVisibility.cmake")
add_library(fixture-core STATIC core.c)
nostrc_library_visibility(fixture-core FIXTURE_API fixture/export.h "include/fixture")
get_target_property(_defs fixture-core INTERFACE_COMPILE_DEFINITIONS)
file(WRITE "${{CMAKE_BINARY_DIR}}/interface.txt" "${{_defs}}")
''')
            (src / "core.c").write_text('#include "fixture/export.h"\nFIXTURE_API int public_api(void) { return 42; }\n')
            build = src / "build"
            try:
                run("cmake", "-S", str(src), "-B", str(build))
                run("cmake", "--build", str(build))
            except subprocess.CalledProcessError as exc:
                self.fail(exc.stdout)
            self.assertEqual((build / "interface.txt").read_text(),
                             "FIXTURE_CORE_STATIC_DEFINE")


    def test_loadable_module_exports_exact_entry_points(self):
        # PAM/NSS/PKCS#11-style modules: export exactly the loader entry points,
        # hide the module's own helpers and anything from linked archives, and
        # fail the build when an entry point is hidden or undefined.
        cases = (
            ("exact", "ENTRY", "entry_one entry_two", None),
            ("hidden", "", "entry_one entry_two", "Missing"),
            ("undefined", "ENTRY", "entry_one entry_two entry_three", None),
        )
        for name, annotation, symbols, message in cases:
            with self.subTest(case=name), tempfile.TemporaryDirectory(prefix="nostrc-module-") as tmp:
                src = pathlib.Path(tmp)
                # A sibling directory includes the helper first; include_guard
                # must not leave the module's directory without its state.
                (src / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.22)
project(module_fixture C)
add_subdirectory(library)
add_subdirectory(module)
''')
                (src / "library").mkdir()
                (src / "library/CMakeLists.txt").write_text(f'''
include("{ROOT}/cmake/NostrcVisibility.cmake")
add_library(helpers STATIC ../helpers.c)
set_target_properties(helpers PROPERTIES POSITION_INDEPENDENT_CODE ON)
''')
                (src / "module").mkdir()
                (src / "module/CMakeLists.txt").write_text(f'''
include("{ROOT}/cmake/NostrcVisibility.cmake")
add_library(plugin MODULE ../plugin.c)
target_link_libraries(plugin PRIVATE helpers)
nostrc_module_visibility(plugin SYMBOLS {symbols})
''')
                (src / "helpers.c").write_text("int archive_helper(void) { return 2; }\n")
                (src / "plugin.c").write_text(
                    '#define ENTRY __attribute__((visibility("default")))\n'
                    "int archive_helper(void);\n"
                    "int module_helper(void) { return archive_helper(); }\n"
                    f"ENTRY int entry_one(void) {{ return module_helper(); }}\n"
                    f"{annotation} int entry_two(void) {{ return 1; }}\n")
                build = src / "build"
                run("cmake", "-S", str(src), "-B", str(build))
                result = subprocess.run(["cmake", "--build", str(build)], text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                if name != "exact":
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    if message:
                        self.assertIn(message, result.stdout)
                    continue
                self.assertEqual(result.returncode, 0, result.stdout)
                if platform.system() == "Linux":
                    module = next(build.glob("module/*plugin*.so"))
                    symbols = run("nm", "-D", "--defined-only", str(module))
                    names = sorted(line.split()[-1] for line in symbols.splitlines())
                    self.assertEqual(names, ["entry_one", "entry_two"])


if __name__ == "__main__":
    unittest.main()
