#!/usr/bin/env bash
# Docker is simulated here; this proves adapter ownership, not live Docker/MDK.
set -euo pipefail
scripts="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
python3 - "$scripts" "$tmp" <<'PY'
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

scripts, tmp = map(Path, sys.argv[1:])
(tmp/'bin').mkdir()
(tmp/'state').mkdir()
(tmp/'source').mkdir()
(tmp/'source/CMakeLists.txt').touch()
docker = tmp/'bin/docker'
docker.write_text('''#!/usr/bin/env python3
import json, os, pathlib, signal, sys
args = sys.argv[1:]
root = pathlib.Path(os.environ['FAKE_DOCKER_STATE'])
if args[0] == 'info':
    print('aarch64')
elif args[0] == 'build':
    image = 'sha256:' + os.environ['IMAGE_CHAR']*64
    pathlib.Path(args[args.index('--iidfile')+1]).write_text(image)
elif args[0] == 'run':
    name = args[args.index('--name')+1]
    (root/(name+'.json')).write_text(json.dumps(dict(args=args, pid=os.getpid())))
    pathlib.Path(args[args.index('--cidfile')+1]).write_text(name)
    print('OWNED_READY '+name, flush=True)
    if not os.environ.get('EXIT_RUN'):
        signal.pause()
elif args[0] == 'rm':
    name = args[-1]
    with (root/'removed').open('a') as f: f.write(name+'\\n')
    path = root/(name+'.json')
    if path.exists():
        try: os.kill(json.loads(path.read_text())['pid'], signal.SIGTERM)
        except ProcessLookupError: pass
else:
    raise SystemExit('unowned docker operation: '+str(args))
''')
docker.chmod(0o755)
base_env = dict(os.environ, PATH=str(tmp/'bin')+os.pathsep+os.environ['PATH'],
                TMPDIR=str(tmp/'state'), FAKE_DOCKER_STATE=str(tmp))

def launch(char, **extra):
    return subprocess.Popen(['bash', str(scripts/'linux-gate.sh'), str(tmp/'source')],
        env=dict(base_env, IMAGE_CHAR=char, **extra), stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)

def ready(p):
    while True:
        line = p.stdout.readline()
        if not line:
            raise AssertionError(('gate exited before readiness', p.wait()))
        if line.startswith('OWNED_READY '):
            return line.split()[1]

first, second = launch('a'), launch('b', NOSTRC_GATE_AMD64='1')
try:
    one, two = ready(first), ready(second)
    assert one != two
    args1 = json.loads((tmp/(one+'.json')).read_text())['args']
    args2 = json.loads((tmp/(two+'.json')).read_text())['args']
    assert 'sha256:'+'a'*64 in args1 and 'sha256:'+'b'*64 in args2
    assert args1[args1.index('--platform')+1] == 'linux/arm64'
    assert args2[args2.index('--platform')+1] == 'linux/amd64'
    assert '--label' in args1 and '--cidfile' in args1
    first.terminate()
    output = first.communicate(timeout=15)[0]
    assert first.returncode == 130, output
    assert second.poll() is None, 'cancellation stopped a sibling run'
    assert (tmp/'removed').read_text().splitlines() == [one]
    second.terminate()
    second.communicate(timeout=15)
    assert set((tmp/'removed').read_text().splitlines()) == {one,two}
    receipts = [json.loads(p.read_text()) for p in (tmp/'state').glob('*/receipt.json')]
    assert len(receipts) == 2
    assert all(r['status'] == 'cancelled' and r['complete'] is False for r in receipts)
    assert {r['image_id'] for r in receipts} == {'sha256:'+'a'*64, 'sha256:'+'b'*64}
finally:
    for p in (first,second):
        if p.poll() is None:
            p.terminate()
            p.communicate(timeout=15)
print('ok: immutable build IDs, architecture, per-run CID/label, owned cancellation, sibling survival')

success = launch('c', EXIT_RUN='1')
ready(success)
success.communicate(timeout=15)
assert success.returncode == 0
assert any(json.loads(p.read_text())['status'] == 'passed' for p in (tmp/'state').glob('*/receipt.json'))
print('ok: successful legacy run is recorded without claiming exact-tree/shared-plan coverage')
bypassed = launch('d', EXIT_RUN='1', NOSTRC_GATE_LINUX_TESTS='0')
ready(bypassed)
bypassed.communicate(timeout=15)
assert bypassed.returncode == 0
assert any(json.loads(p.read_text())['bypasses'] == ['NOSTRC_GATE_LINUX_TESTS=0']
           and json.loads(p.read_text())['status'] == 'bypassed' for p in (tmp/'state').glob('*/receipt.json'))
print('ok: build-only bypass cannot produce a clean passing receipt')
PY
echo 'linux-gate lifecycle tests passed'
