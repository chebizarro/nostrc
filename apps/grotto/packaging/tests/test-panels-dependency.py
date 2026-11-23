#!/usr/bin/env python3
"""Exercise Debian dependency generation against an isolated installed SDK record."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]


class PanelsDependency(unittest.TestCase):
    def test_runtime_matches_the_installed_sdk_package_revision(self):
        for tool in ("make", "dpkg-query", "dpkg-gencontrol", "dpkg-checkbuilddeps"):
            self.assertIsNotNone(shutil.which(tool), "requires Debian packaging tools: " + tool)
        with tempfile.TemporaryDirectory(prefix="panels-dependency-") as directory:
            root = Path(directory)
            debian = root / "debian"
            (debian / "grotto/DEBIAN").mkdir(parents=True)
            for name in ("rules", "control", "changelog"):
                shutil.copy(ROOT / "apps/grotto/packaging/debian" / name, debian / name)
            database = root / "dpkg"
            database.mkdir()
            sdk_version = "0.1.1-1+git42.gabc1234"
            def installed(runtime_version):
                (database / "status").write_text(
                    "Package: libgrotto-panels-0-dev\nStatus: install ok installed\n"
                    "Architecture: all\nVersion: " + sdk_version + "\nDescription: fixture SDK\n\n"
                    "Package: libgrotto-panels-0-0\nStatus: install ok installed\n"
                    "Architecture: all\nVersion: " + runtime_version + "\nDescription: fixture runtime\n")
            installed("0.1.0-1")
            commands = root / "bin"
            commands.mkdir()
            # Only redirect database custody and the debhelper staging boundary.
            # Version lookup, control generation and dependency evaluation are real dpkg tools.
            query = shutil.which("dpkg-query")
            gencontrol = shutil.which("dpkg-gencontrol")
            (commands / "dpkg-query").write_text(
                '#!/bin/sh\nexec "' + query + '" --admindir="$TEST_ADMINDIR" "$@"\n')
            (commands / "dh_gencontrol").write_text(
                '#!/bin/sh\n[ "$1" = -- ] && shift\nexec "' + gencontrol +
                '" -pgrotto -Pdebian/grotto "$@"\n')
            for command in commands.iterdir():
                command.chmod(0o755)
            env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"],
                       TEST_ADMINDIR=str(database))
            result = subprocess.run(["make", "-f", "debian/rules", "override_dh_gencontrol"],
                                    cwd=root, env=env, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            control_path = debian / "grotto/DEBIAN/control"
            if not control_path.exists():
                # Without an override, the normal dh sequence calls dh_gencontrol directly.
                subprocess.run([str(commands / "dh_gencontrol"), "--"], cwd=root, env=env,
                               check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            control = control_path.read_text()
            relation = re.search(r"libgrotto-panels-0-0 \([^)]*\)", control)
            self.assertIsNotNone(relation, control)
            self.assertEqual(relation.group(), "libgrotto-panels-0-0 (= " + sdk_version + ")")
            def check():
                return subprocess.run(["dpkg-checkbuilddeps", "--admindir=" + str(database),
                    "-I", "-d", relation.group(), str(debian / "control")],
                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            rejected = check()
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
            self.assertIn("libgrotto-panels-0-0 (= " + sdk_version + ")", rejected.stdout)
            installed(sdk_version)
            accepted = check()
            self.assertEqual(accepted.returncode, 0, accepted.stdout)
            (database / "status").write_text("")
            control_path.unlink()
            missing = subprocess.run(["make", "-f", "debian/rules", "override_dh_gencontrol"],
                cwd=root, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(missing.returncode, 0, missing.stdout)
            self.assertFalse(control_path.exists(), "missing SDK must not emit weakened metadata")


if __name__ == "__main__":
    unittest.main()
