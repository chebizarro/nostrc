#!/usr/bin/env python3
"""Focused Snap SDK resolution regression; hosted package builds remain separate."""
import os
from pathlib import Path
import re
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


class MacOSTLSDependencyTests(unittest.TestCase):
    def test_lws_linkage_selects_matching_openssl_major(self):
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake, "Install CMake to run the TLS dependency regression")
        with tempfile.TemporaryDirectory(prefix="nostrc-tls-abi-") as temp:
            root = Path(temp)
            for major in (3, 4):
                with self.subTest(major=major):
                    prefix = f"/opt/homebrew/opt/openssl@{major}"
                    script = root / "check.cmake"
                    script.write_text(
                        f'include("{ROOT}/cmake/NostrcMacOSOpenSSL.cmake")\n'
                        f'set(linkage "libwebsockets.dylib:\\n\\t{prefix}/lib/libssl.{major}.dylib (compatibility version {major}.0.0)\\n")\n'
                        'nostrc_openssl_root_from_linkage(actual "${linkage}")\n'
                        f'if(NOT actual STREQUAL "{prefix}")\n'
                        '  message(FATAL_ERROR "wrong TLS dependency: ${actual}")\nendif()\n')
                    subprocess.run([cmake, "-P", str(script)], check=True)


class FipsdPackagingVersionTests(unittest.TestCase):
    """fipsd/packaging reads one version: declare_component_version in
    fipsd/CMakeLists.txt, the source VERSION_MANIFEST.md names and the value
    scripts/tag_release.py tags from. Every recipe that must agree with it
    is checked here, so a bump that misses one cannot ship."""

    def setUp(self):
        self.packaging = ROOT / "fipsd/packaging"
        self.declared = subprocess.run(
            ["sh", str(self.packaging / "common/version.sh"), str(ROOT)],
            check=True, stdout=subprocess.PIPE, text=True).stdout.strip()

    def test_version_sh_reads_the_declared_component_version(self):
        cmake = (ROOT / "fipsd/CMakeLists.txt").read_text()
        match = re.search(r"^declare_component_version\(FIPSD (\d+) (\d+) (\d+)\)$", cmake, re.M)
        self.assertIsNotNone(match)
        self.assertEqual(self.declared, ".".join(match.groups()))

    def test_manifest_declares_the_same_fipsd_version(self):
        rows = [line for line in (ROOT / "VERSION_MANIFEST.md").read_text().splitlines()
                if line.startswith("| fipsd |")]
        self.assertEqual(len(rows), 1)
        cells = [cell.strip() for cell in rows[0].strip("|").split("|")]
        self.assertEqual(cells[2], self.declared)

    def test_debian_changelog_release_entry_matches(self):
        first = (self.packaging / "debian/changelog").read_text().splitlines()[0]
        self.assertEqual(first.split()[:2], ["fipsd", f"({self.declared}-1)"])

    def test_rpm_spec_matches(self):
        spec = (self.packaging / "rpm/fipsd.spec").read_text()
        self.assertIn(f"%global upstream_version {self.declared}\n", spec)

    def test_pkgbuild_matches(self):
        pkgbuild = (self.packaging / "arch/PKGBUILD").read_text()
        self.assertIn(f"_version={self.declared}\n", pkgbuild)

    def test_debian_dev_version_sorts_before_the_release(self):
        """A tree that is not at the fipsd-v tag gets <declared>~git...-1,
        which dpkg orders before <declared>-1, so a dev package never shadows
        the release it is heading for."""
        deb = subprocess.run(
            ["sh", str(self.packaging / "common/version.sh"), "--deb", str(ROOT)],
            check=True, stdout=subprocess.PIPE, text=True).stdout.strip()
        self.assertTrue(deb.startswith(self.declared), deb)
        self.assertTrue(deb.endswith("-1"), deb)
        at_tag = subprocess.run(
            ["git", "-C", str(ROOT), "describe", "--tags", "--exact-match", "HEAD"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout.strip()
        if at_tag == f"fipsd-v{self.declared}":
            self.assertEqual(deb, f"{self.declared}-1")
        else:
            self.assertRegex(deb, rf"^{re.escape(self.declared)}~git\d{{8}}\.[0-9a-f]{{10}}(\.dirty)?-1$")

    def test_source_archive_takes_the_fipsd_name(self):
        script = (ROOT / "scripts/make-desktop-source-archive.sh").read_text()
        self.assertIn("name=${3:-groundhog}-$version", script)


if __name__ == "__main__":
    unittest.main()
