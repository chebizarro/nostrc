#!/usr/bin/env python3
"""Discovery-contract tests, not daemon/protocol interoperability evidence."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PIN = "64f5b4d482cfe14abad108ffc2d92128f7658311"
OLD = "6aa80504baa0fa312b392009eea0e3e59fb6fe4c"
OPTIONS = ("FIPSD_UDP_INTEROP_RUST_BINARY", "FIPSD_TCP_INTEROP_RUST_BINARY",
           "FIPSD_FIPSCTL_EXECUTABLE", "FIPSD_UDP_INTEROP_RUST_ECHO")


@unittest.skipUnless(os.name == "posix", "POSIX CMake interop discovery")
class Discovery(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fips rust discovery ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        module = self.root / "source/fipsd/tests/PinnedRustInterop.cmake"
        module.parent.mkdir(parents=True)
        shutil.copyfile(Path(__file__).with_name("PinnedRustInterop.cmake"), module)
        pin_file = self.root / "source/libfips-core/UPSTREAM.md"
        pin_file.parent.mkdir()
        pin_file.write_text(f"Pinned commit: `{PIN}`\n")
        self.script = self.root / "discover.cmake"
        self.script.write_text(
            'set(CMAKE_HOST_SYSTEM_NAME Darwin)\nset(CMAKE_HOST_SYSTEM_PROCESSOR arm64)\n'
            f'include("{module}")\n' + "".join(
                f'message(STATUS "RESULT {name}=${{{name}}}")\n' for name in OPTIONS))
        self.env = {k: v for k, v in os.environ.items() if k not in OPTIONS}
        self.env["XDG_CACHE_HOME"] = str(self.root / "cache")
        self.env["HOME"] = str(self.root / "home")

    def cache(self, pin=PIN, version=None):
        release = Path(self.env["XDG_CACHE_HOME"]) / "nostrc/fips" / pin / "Darwin-arm64/release"
        (release / "examples").mkdir(parents=True, exist_ok=True)
        for name in ("fips", "fipsctl", "examples/native-echo"):
            p = release / name
            p.write_text("#!/bin/sh\nprintf '%s\\n' 'fips 0.6.0-dev (rev "
                         + (version or pin[:10]) + ")'\n")
            p.chmod(0o755)
        return release

    def run_cmake(self, *defines, success=True):
        p = subprocess.run(["cmake", *defines, "-P", str(self.script)],
                           cwd=self.root, env=self.env, capture_output=True, text=True)
        if not success:
            self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertIn("not the clean UPSTREAM.md pin", p.stderr)
            return {}
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return dict(line.split("RESULT ", 1)[1].split("=", 1)
                    for line in p.stdout.splitlines() if "RESULT " in line)

    def test_absent_cache_never_falls_back_to_an_old_pin(self):
        self.cache(OLD)
        self.assertTrue(all(not value for value in self.run_cmake().values()))

    def test_pin_and_platform_keyed_cache_with_spaces(self):
        release = self.cache()
        actual = self.run_cmake()
        for name in OPTIONS[:2]:
            self.assertEqual(actual[name], str(release / "fips"))
        self.assertEqual(actual[OPTIONS[2]], str(release / "fipsctl"))
        self.assertEqual(actual[OPTIONS[3]], "")  # stale macOS example gate

    def test_explicit_options_override_environment_and_cache(self):
        self.cache(version=OLD[:10])
        defines = []
        for name in OPTIONS:
            self.env[name] = "/environment/override"
            defines.append(f"-D{name}=/explicit/override")
        self.assertEqual(set(self.run_cmake(*defines).values()), {"/explicit/override"})

    def test_environment_including_empty_disables_auto_discovery(self):
        self.cache()
        for name in OPTIONS:
            self.env[name] = ""
        self.assertTrue(all(not value for value in self.run_cmake().values()))
        for name in OPTIONS:
            self.env[name] = "/environment/override"
        self.assertEqual(set(self.run_cmake().values()), {"/environment/override"})

    def test_explicit_empty_options_disable_auto_discovery(self):
        self.cache()
        self.assertTrue(all(not value for value in self.run_cmake(
            *(f"-D{name}=" for name in OPTIONS)).values()))

    def test_wrong_or_dirty_cached_revision_fails_visibly(self):
        self.cache(version=OLD[:10])
        self.run_cmake(success=False)
        self.cache(version=PIN[:10] + "-dirty")
        self.run_cmake(success=False)

    def test_explicit_upstream_directory_preserves_control_source_build(self):
        release = self.cache()
        self.script.write_text('set(FIPSD_UPSTREAM_CONTROL_DIR "/read-only/source")\n'
                               + self.script.read_text())
        actual = self.run_cmake()
        self.assertEqual(actual[OPTIONS[2]], "")
        self.assertEqual(actual[OPTIONS[0]], str(release / "fips"))

    def test_linux_reference_echo_is_discovered(self):
        release = self.cache()
        platform = release.parent
        platform.rename(platform.with_name("Linux-arm64"))
        self.script.write_text(self.script.read_text().replace("Darwin", "Linux"))
        self.assertEqual(self.run_cmake()[OPTIONS[3]],
                         str(platform.with_name("Linux-arm64") / "release/examples/native-echo"))

    def test_home_cache_fallback(self):
        self.env["XDG_CACHE_HOME"] = str(Path(self.env["HOME"]) / ".cache")
        release = self.cache()
        self.env.pop("XDG_CACHE_HOME")
        self.assertEqual(self.run_cmake()[OPTIONS[0]], str(release / "fips"))


if __name__ == "__main__":
    unittest.main()
