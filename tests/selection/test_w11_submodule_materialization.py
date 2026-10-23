"""Real exact-OID local hydration, not source/profile/build acceptance."""
import ast
import hashlib
import json
import os
from pathlib import Path
from types import SimpleNamespace
import subprocess
import tempfile
import unittest

from selector_support import ROOT, s


class SubmoduleMaterializationTests(unittest.TestCase):
    def test_recursive_selected_gitlinks_use_objects_not_mutable_cache_heads(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            cache, child, inner = (root / name for name in ('cache', 'child', 'inner'))
            def git(repo, *args):
                return subprocess.check_output(['git', '-c', 'protocol.file.allow=always', '-C', str(repo), *args],
                                               stderr=subprocess.PIPE).decode().strip()
            def commit(repo):
                git(repo, 'add', '.')
                git(repo, '-c', 'user.name=Materialization fixture', '-c', 'user.email=fixture@example.invalid',
                    '-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'fixture')
                return git(repo, 'rev-parse', 'HEAD')
            for repo in (cache, child, inner):
                repo.mkdir(); git(repo, 'init', '-q')
                (repo / 'data').write_text(repo.name + '\n')
                commit(repo)
            inner_sha = git(inner, 'rev-parse', 'HEAD')
            git(child, 'submodule', 'add', str(inner), 'nested')
            child_sha = commit(child)
            git(cache, 'submodule', 'add', str(child), 'third_party/nsync')
            git(cache / 'third_party/nsync', 'submodule', 'update', '--init', '--recursive')
            # The source URLs must not be reached: all exact objects are present.
            git(cache, 'config', '-f', '.gitmodules', 'submodule.third_party/nsync.url', 'https://invalid.invalid/nsync')
            commit(cache)
            source = root / 'source'
            subprocess.run(['git', 'clone', '--no-local', str(cache), str(source)], check=True, capture_output=True)
            cached_child = cache / 'third_party/nsync'
            (cached_child / 'data').write_text('newer cache head must not execute\n')
            newer = commit(cached_child)
            self.assertNotEqual(newer, child_sha)
            directory = root / 'evidence'; directory.mkdir()
            text = (ROOT / 'scripts/test-gate.sh').read_text().split("<<'PY'\n", 1)[1].rsplit('\nPY', 1)[0]
            function = next(n for n in ast.parse(text).body if isinstance(n, ast.FunctionDef)
                            and n.name == 'materialize_submodules')
            calls = []
            def command(argv, log):
                calls.append([str(x) for x in argv])
                process = subprocess.run(argv, capture_output=True, timeout=30)
                Path(log).write_bytes(process.stdout + process.stderr)
                self.assertEqual(process.returncode, 0, Path(log).read_text())
            ns = dict(Path=Path, os=os, hashlib=hashlib, shared=s, args=SimpleNamespace(repo=cache), command=command)
            ns['git'] = lambda *args, repo=None: s.git(repo or cache, *args).stdout
            exec(compile(ast.Module(body=[function], type_ignores=[]), 'materialize_submodules', 'exec'), ns)
            initialized = set()
            ns['materialize_submodules'](source, directory, {'source_dir': 'libgo'}, initialized)
            self.assertEqual(initialized, {'third_party/nsync'})
            self.assertEqual(git(source / 'third_party/nsync', 'rev-parse', 'HEAD'), child_sha)
            self.assertEqual(git(source / 'third_party/nsync/nested', 'rev-parse', 'HEAD'), inner_sha)
            self.assertEqual(git(cached_child, 'rev-parse', 'HEAD'), newer)
            self.assertEqual((source / 'third_party/nsync/data').read_text(), 'child\n')
            record = json.loads((directory / 'submodules.json').read_text())
            self.assertEqual(len(record['modules']), 2)
            self.assertTrue(all('source-authored network URL' != row['object_source'] for row in record['modules']))
            self.assertEqual(len(calls), 2)
            self.assertTrue(all('--checkout' in argv for argv in calls))


if __name__ == '__main__':
    unittest.main()
