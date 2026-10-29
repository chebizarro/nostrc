"""Physical setup regressions; miniature Git/CMake proof is not hosted acceptance."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from selector_support import ROOT


class W11WorkerSetupTests(unittest.TestCase):

    def test_windows_checkout_preserves_pinned_vector_bytes(self):
        text = (ROOT / '.github/workflows/windows-validation.yml').read_text()
        checkout = text.split('- name: Checkout repository', 1)[1].split('- name: Verify requested checkout', 1)[0]
        settings = dict(re.findall(r"^          (GIT_CONFIG_\w+): '?([^'\n]+)'?$", checkout, re.M))
        self.assertEqual(settings, {'GIT_CONFIG_COUNT': '1', 'GIT_CONFIG_KEY_0': 'core.autocrlf',
                                    'GIT_CONFIG_VALUE_0': 'false'})
        vector = 'nips/nip44/tests/nip44.vectors.json'
        original = (ROOT / vector).read_bytes()
        pinned = '269ed0f69e4c192512cc779e78c555090cebc7c785b609e338a62afc3ce25040'
        self.assertEqual(hashlib.sha256(original).hexdigest(), pinned)
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=str(repo / 'global.gitconfig'))
            env.pop('GIT_CONFIG_COUNT', None)
            def git(*args, extra=None):
                return subprocess.run(['git', *args], cwd=repo, env=dict(env, **(extra or {})),
                                      check=True, capture_output=True)
            git('init')
            git('config', 'core.autocrlf', 'false')
            dest = repo / vector
            dest.parent.mkdir(parents=True)
            dest.write_bytes(original)
            git('add', vector)
            git('config', 'core.autocrlf', 'true')
            dest.unlink()
            git('checkout-index', '-f', '--', vector)
            self.assertNotEqual(hashlib.sha256(dest.read_bytes()).hexdigest(), pinned)
            self.assertIn(b'\r\n', dest.read_bytes())
            dest.unlink()
            git('checkout-index', '-f', '--', vector, extra=settings)
            self.assertEqual(hashlib.sha256(dest.read_bytes()).hexdigest(), pinned)
            self.assertEqual(git('config', '--get', 'core.autocrlf').stdout.strip(), b'true')

    def test_package_binary_path_replaces_launcher_without_weakening_cmake_identity(self):
        text = (ROOT / '.github/workflows/dependency-worker.yml').read_text()
        step = text.split('- name: Match standalone libgo Linux CMake toolchain', 1)[1].split('- name:', 1)[0]
        self.assertIn("if: runner.os == 'Linux' && inputs.profile == 'libgo.linux.asan'", step)
        self.assertIn("'cmake==3.31.6'", step)
        export = next(line.strip() for line in step.splitlines() if '>> "$GITHUB_PATH"' in line)
        cmake = shutil.which('cmake')
        self.assertIsNotNone(cmake, 'CMake is required for the physical setup regression')
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            source = base / 'source'
            source.mkdir()
            (source / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(identity NONE)\nfile(WRITE "${CMAKE_BINARY_DIR}/cmake-path" "${CMAKE_COMMAND}")\n')
            subprocess.run([cmake, '-S', str(source), '-B', str(base / 'before')], check=True, capture_output=True)
            configured = Path((base / 'before/cmake-path').read_text()).resolve()
            launcher = base / 'launcher'
            launcher.mkdir()
            (launcher / 'cmake').write_text('#!/bin/sh\nexec ' + repr(str(configured)) + ' "$@"\n')
            (launcher / 'cmake').chmod(0o755)
            # Package metadata fixture; the physical pinned pip installation is a separate receipt.
            (base / 'cmake.py').write_text('CMAKE_BIN_DIR = ' + repr(str(configured.parent)) + '\n')
            env = dict(os.environ, PATH=str(launcher) + os.pathsep + os.environ['PATH'],
                       PYTHONPATH=str(base), GITHUB_PATH=str(base / 'github-path'))
            self.assertNotEqual(Path(shutil.which('cmake', path=env['PATH'])).resolve(), configured)
            subprocess.run(['bash', '-eu', '-c', export], env=env, check=True, capture_output=True)
            tool_bin = (base / 'github-path').read_text().strip()
            env['PATH'] = tool_bin + os.pathsep + env['PATH']
            selected = shutil.which('cmake', path=env['PATH'])
            subprocess.run([selected, '-S', str(source), '-B', str(base / 'after')], env=env, check=True, capture_output=True)
            self.assertEqual(Path(selected).resolve(), Path((base / 'after/cmake-path').read_text()).resolve())


if __name__ == '__main__':
    unittest.main()
