#!/usr/bin/env python3
"""Focused link/install regression for NostrcVisibility, shared and static."""
import pathlib
import platform
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


class VisibilityTests(unittest.TestCase):
    def test_shared_and_static_consumers(self):
        for shared in ("ON", "OFF"):
            with self.subTest(shared=shared), tempfile.TemporaryDirectory(prefix="nostrc-visibility-") as tmp:
                src = pathlib.Path(tmp)
                (src / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.22)
project(visibility_fixture C)
include("{ROOT}/cmake/NostrcVisibility.cmake")
add_library(fixture core.c)
nostrc_library_visibility(fixture FIXTURE_API fixture/export.h "include/fixture")
target_include_directories(fixture PUBLIC "${{CMAKE_CURRENT_SOURCE_DIR}}")
install(TARGETS fixture DESTINATION lib)
install(FILES api.h DESTINATION include)
add_library(bridge SHARED bridge.c)
# Match libjson's supported ELF static-core-from-executable arrangement.
if(NOT BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
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
                    if shared == "ON" and platform.system() == "Linux":
                        symbols = run("nm", "-D", "--defined-only", str(build / "libfixture.so"))
                        self.assertIn(" public_api\n", symbols)
                        self.assertNotIn("private_impl", symbols)
                except subprocess.CalledProcessError as exc:
                    self.fail(exc.stdout)


if __name__ == "__main__":
    unittest.main()
