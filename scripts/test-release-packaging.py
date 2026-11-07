#!/usr/bin/env python3
"""Focused Snap SDK resolution regression; hosted package builds remain separate."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SnapSDKTests(unittest.TestCase):
    def test_production_helper_reroots_sdk_library_hints_only_for_sdk_builds(self):
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake, "Install CMake to run the release packaging regression")
        with tempfile.TemporaryDirectory(prefix="nostrc-snap-sdk-") as temp:
            root = Path(temp)
            commands = root / "bin"
            commands.mkdir()
            configure = root / "scripts/ci/configure-release.sh"
            configure.parent.mkdir(parents=True)
            configure.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$CAPTURE"\n')
            configure.chmod(0o755)
            # Intercept network/build/install commands, not the production helper.
            for tool in ("python3", "cmake", "install", "pkg-config"):
                command = commands / tool
                command.write_text('#!/bin/sh\nprintf "%s\\n" "$GTK_PREFIX"\n'
                                   if tool == "pkg-config" else '#!/bin/sh\nexit 0\n')
                command.chmod(0o755)
            env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"],
                       CRAFT_PART_INSTALL=str(root / "install"), CAPTURE=str(root / "args"))
            for prefix in ("/usr", str(root / "sdk/usr")):
                with self.subTest(prefix=prefix):
                    subprocess.run(["sh", str(ROOT / "scripts/ci/build-snap-part.sh"), "daemon"],
                                   cwd=root, env=dict(env, GTK_PREFIX=prefix), check=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    args = (root / "args").read_text().splitlines()
                    roots = [arg for arg in args if arg.startswith("-DCMAKE_FIND_ROOT_PATH=")]
                    self.assertEqual(bool(roots), prefix != "/usr")
                    if not roots:
                        continue
                    # Real FindPkgConfig first searches its hints with NO_DEFAULT_PATH.
                    # A prefix alone cannot stop a host hint winning over the SDK.
                    host = root / "host/lib"
                    sdk = root / "sdk" / str(host).lstrip("/")
                    for directory in (host, sdk):
                        directory.mkdir(parents=True)
                        (directory / "libnostrc_packaging_fixture.a").touch()
                    check = root / "check.cmake"
                    check.write_text(
                        f'find_library(LIB nostrc_packaging_fixture HINTS "{host}" NO_DEFAULT_PATH REQUIRED)\n'
                        f'if(NOT LIB STREQUAL "{sdk}/libnostrc_packaging_fixture.a")\n'
                        '  message(FATAL_ERROR "host library selected: ${LIB}")\nendif()\n')
                    subprocess.run([cmake, *roots, "-P", str(check)], check=True)


if __name__ == "__main__":
    unittest.main()
