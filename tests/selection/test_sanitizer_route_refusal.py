"""Missing phase-aware shared realization is failure, not legacy Ninja parity."""
import ast
import os
import platform
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class SharedSanitizerRouteTests(unittest.TestCase):
    def test_outer_adapter_refuses_to_drop_explicit_phase_maps(self):
        script=(ROOT/'scripts/test-gate.sh').read_text().split("<<'PY'\n",1)[1].rsplit('\nPY',1)[0]
        function=next(n for n in ast.parse(script).body if isinstance(n,ast.FunctionDef) and n.name=='host_guard')
        namespace=dict(platform=platform)
        exec(compile(ast.Module(body=[function],type_ignores=[]),'<host phase guard>','exec'),namespace)
        host={'Darwin':'macos','Linux':'linux','Windows':'windows'}[platform.system()]
        profile=dict(platform=host,architecture='native',phase_environments={'configure':{},'build':{},'test':{'ASAN_OPTIONS':'detect_leaks=1'}})
        with self.assertRaisesRegex(RuntimeError,'phase-aware execution is unsupported'):
            namespace['host_guard'](profile,[],{})

    def test_unsupported_shared_route_stops_before_docker_or_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            tools=Path(tmp);trace=tools/'called'
            docker=tools/'docker';docker.write_text('#!/bin/sh\ntouch "$TRACE"\nexit 99\n');docker.chmod(0o700)
            result=subprocess.run(['bash',str(ROOT/'scripts/linux-gate.sh'),'--sanitizers',str(ROOT)],
                env=dict(os.environ,PATH=str(tools)+os.pathsep+os.environ['PATH'],TRACE=str(trace),
                         NOSTRC_SANITIZER_SHARED_PROFILE='not-yet-phase-aware'),capture_output=True,text=True)
            self.assertEqual(result.returncode,2,result.stdout+result.stderr)
            self.assertIn('shared phase-aware realization/configure route is unsupported',result.stderr)
            self.assertIn('no legacy Ninja fallback',result.stderr)
            self.assertFalse(trace.exists())


if __name__=='__main__': unittest.main()
