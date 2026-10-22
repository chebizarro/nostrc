"""Explicit shared routing never bypasses capability or metadata failures."""
import ast
import os
import platform
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

ROOT=Path(__file__).resolve().parents[2]


class SharedSanitizerRouteTests(unittest.TestCase):
    def test_phase_metadata_does_not_bypass_dangerous_capability_guard(self):
        script=(ROOT/'scripts/test-gate.sh').read_text().split("<<'PY'\n",1)[1].rsplit('\nPY',1)[0]
        function=next(n for n in ast.parse(script).body if isinstance(n,ast.FunctionDef) and n.name=='host_guard')
        namespace=dict(platform=platform, args=SimpleNamespace(linux_custody='legacy'))
        exec(compile(ast.Module(body=[function],type_ignores=[]),'<host phase guard>','exec'),namespace)
        host={'Darwin':'macos','Linux':'linux','Windows':'windows'}[platform.system()]
        profile=dict(platform=host,architecture='native',capabilities=['docker'],
                     phase_environments={'configure':{},'build':{},'test':{'ASAN_OPTIONS':'detect_leaks=1'}})
        with self.assertRaisesRegex(RuntimeError,'unimplemented guarded adapter'):
            namespace['host_guard'](profile,[],{'checks':{}},'fixture')

    def test_outer_tool_capabilities_refuse_missing_nak_and_unattested_keyring(self):
        script=(ROOT/'scripts/test-gate.sh').read_text().split("<<'PY'\n",1)[1].rsplit('\nPY',1)[0]
        function=next(n for n in ast.parse(script).body if isinstance(n,ast.FunctionDef) and n.name=='host_guard')
        namespace=dict(platform=platform, os=os, shutil=SimpleNamespace(which=lambda name: None),
                       args=SimpleNamespace(linux_custody='legacy'))
        exec(compile(ast.Module(body=[function],type_ignores=[]),'<host tool guard>','exec'),namespace)
        host={'Darwin':'macos','Linux':'linux','Windows':'windows'}[platform.system()]
        check=dict(kind='command',profiles=['fixture'],build_targets=[],requires=['nak'])
        profile=dict(platform=host,architecture='native',capabilities=['nak'])
        with self.assertRaisesRegex(RuntimeError,'actual nak executable; lane not launched'):
            namespace['host_guard'](profile,['tool'],{'checks':{'tool':check}},'fixture')
        check['requires']=['gnome_keyring']
        profile['capabilities']=['gnome_keyring','private_bus']
        with self.assertRaisesRegex(RuntimeError,'native adapter not implemented'):
            namespace['host_guard'](profile,['tool'],{'checks':{'tool':check}},'fixture')

    def test_shared_keyring_ctest_executable_capability_passes_allowlist_but_commands_refuse(self):
        import sys
        from unittest import mock
        class ReachedExecutionPlanning(Exception): pass
        source = (ROOT / 'scripts/sanitizer-gate-ci.py').read_text()
        function = next(n for n in ast.parse(source).body if isinstance(n, ast.FunctionDef) and n.name == 'run_shared')
        profile = dict(capabilities=['gnome_keyring'])
        check = dict(kind='ctest', profiles=['fixture'], requires=['gnome_keyring'])
        policy = dict(profiles={'fixture': profile}, checks={'tool': check})
        def planning(*args): raise ReachedExecutionPlanning()
        shared = SimpleNamespace(load_inventory=lambda path: policy, commit_sha=planning)
        namespace = dict(Path=Path, os=os, sys=sys, WORKFLOW='unused', WorkflowError=RuntimeError,
            configuration=lambda *args: {'GH_SAN_TEST_JOBS': '1'},
            runpy=SimpleNamespace(run_path=lambda path: {'selection': shared}),
            shutil=SimpleNamespace(which=lambda name: sys.executable))
        exec(compile(ast.Module(body=[function], type_ignores=[]), '<shared capability route>', 'exec'), namespace)
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ):
            os.environ.pop('RERUN_MAX', None)
            os.environ.pop('NOSTRC_CTEST_QUARANTINE_PLATFORM', None)
            args = SimpleNamespace(source_root=str(ROOT), evidence_dir=str(Path(tmp) / 'ctest'),
                inventory='fixture', shared_profile='fixture', test_jobs=1, jobs=1)
            # Validator-only stand-in, not daemon/service proof: actual keyring
            # execution is separately required by the contained lifecycle suite.
            with self.assertRaises(ReachedExecutionPlanning): namespace['run_shared'](args)
            check['kind'] = 'command'
            args.evidence_dir = str(Path(tmp) / 'command')
            with self.assertRaisesRegex(RuntimeError, 'native shared adapter not implemented'):
                namespace['run_shared'](args)

    def test_unknown_shared_profile_stops_before_docker_or_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            tools=Path(tmp);trace=tools/'called'
            docker=tools/'docker';docker.write_text('#!/bin/sh\ntouch "$TRACE"\nexit 99\n');docker.chmod(0o700)
            result=subprocess.run(['bash',str(ROOT/'scripts/linux-gate.sh'),'--sanitizers',str(ROOT)],
                env=dict(os.environ,PATH=str(tools)+os.pathsep+os.environ['PATH'],TRACE=str(trace),
                         NOSTRC_SANITIZER_SHARED_PROFILE='not-yet-phase-aware'),capture_output=True,text=True)
            self.assertEqual(result.returncode,2,result.stdout+result.stderr)
            self.assertIn('missing shared sanitizer metadata',result.stderr)
            self.assertFalse(trace.exists())


if __name__=='__main__': unittest.main()
