#!/usr/bin/env bash
# Real shared CLI + CMake/CTest fixture. No mocked planner, resolver or receipts.
set -euo pipefail
scripts="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$scripts/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
python3 - "$root" "$tmp" <<'PY'
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import socket
import subprocess
import sys

root, tmp = map(Path, sys.argv[1:])
repo = tmp / 'repo'
repo.mkdir()

def git(*args):
    return subprocess.check_output(['git', '-C', str(repo), *args], stderr=subprocess.PIPE).decode().strip()

def write(path, text):
    p = repo / path
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)

def commit():
    git('add', '-A')
    git('commit', '-qm', 'fixture')
    return git('rev-parse', 'HEAD')

git('init', '-q')
git('config', 'user.name', 'Gate Test')
git('config', 'user.email', 'gate@example.invalid')
for path in ('scripts/test-gate.sh', 'scripts/test-plan.py', 'scripts/test_selection.py',
             'scripts/test_selection_realization.py', 'scripts/check-macos-keychain.sh',
             'cmake/NostrcTestInventory.cmake'):
    dest = repo / path
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / path, dest)
write('a/main.c', 'int main(void) { return 0; }\n')
write('b/data.txt', 'base\n')
write('docs/readme.md', 'prose\n')
write('.gitignore', '__pycache__/\n')
write('CMakeLists.txt', '''cmake_minimum_required(VERSION 3.22)
project(gate_fixture C)
enable_testing()
include(cmake/NostrcTestInventory.cmake)
add_executable(probe a/main.c)
add_test(NAME a.unit COMMAND probe)
nostrc_describe_test(a.unit OWNER a TARGETS probe)
add_test(NAME b.unit COMMAND ${CMAKE_COMMAND} -E true)
nostrc_describe_test(b.unit OWNER b INPUTS b/data.txt)
nostrc_finalize_test_inventory()
''')
host = {'Darwin': 'macos', 'Linux': 'linux'}[platform.system()]
compiler = 'clang' if host == 'macos' else 'gcc'
flags = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug']
if host == 'macos':
    flags += ['-DNIP55L_SECRET_BACKEND=libsecret', '-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service']
policy = dict(schema_version=1,
    nodes={x: dict(id=x, owner_paths=[x], depends_on=[], rationale='fixture') for x in ('a', 'b')},
    path_rules=[dict(prefix=x+'/', nodes=[x], effect='impact', reason='fixture') for x in ('a','b')] +
        [dict(prefix='docs/', nodes=[], effect='non_execution', reason='prose')] +
        [dict(prefix=x, nodes=[], effect='global', reason='build contract') for x in ('scripts/', 'cmake/', 'tests/', 'CMakeLists.txt', '.gitignore')],
    test_overrides=[],
    profiles={'native': dict(platform=host, architecture='native', compiler=compiler,
        build_type='Debug', instrumentation='none', source_dir='.', configure_args=flags,
        environment={}, capabilities=[], setup='fixture', local_availability='host',
        parallelism=1, timeout_seconds=30)},
    checks={x+'.unit': dict(nodes=[x], profiles=['native'], kind='ctest', cost='ordinary',
        requirement='blocking', expected_outcome='pass', ctest_tests={'native':[x+'.unit']}) for x in ('a','b')},
    release_components={})
write('tests/selection/inventory.json', json.dumps(policy))
base = commit()
git('update-ref', 'refs/remotes/origin/main', base)
write('a/main.c', 'int main(void) { return 0; } /* a */\n')
a_head = commit()
write('b/data.txt', 'changed\n')
head = commit()

def gate(*args, success=True):
    evidence = tmp / ('evidence-' + str(len(list(tmp.glob('evidence-*')))))
    p = subprocess.run(['bash', str(repo/'scripts/test-gate.sh'), '--repo', str(repo),
                        '--evidence-dir', str(evidence), *args], capture_output=True, text=True,
                        env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
    receipts = list(evidence.glob('*/adapter-receipt.json'))
    assert len(receipts) == 1, (p.stdout, p.stderr)
    receipt = json.loads(receipts[0].read_text())
    if (p.returncode == 0) != success:
        for log in evidence.rglob('*.log'):
            print(str(log), log.read_text(errors='replace')[-6000:], file=sys.stderr)
    assert (p.returncode == 0) == success, (p.returncode, p.stdout, p.stderr, receipt)
    return receipt, receipts[0].parent

# Net caller dirt is ignored by --head. Both ref ranges for the same SHA are
# retained and unioned, including a non-ancestor/force-push base.
write('a/main.c', '#error dirty caller must not execute\n')
write('docs/untracked name\nwith newline.md', 'dirty prose\n')
refs = tmp/'refs'
refs.write_text(f'refs/heads/a {head} refs/heads/a {a_head}\nrefs/heads/b {head} refs/heads/b {base}\n')
r, directory = gate('--pre-push', '--refs-file', str(refs))
assert r['status'] == 'partial' and not r['complete']
assert len(r['executions']) == 1
record = r['executions'][0]
plan = json.loads(Path(record['plan']).read_text())
assert len(plan['event']) == 2 and plan['selected_checks'] == ['a.unit', 'b.unit'], plan
lane = record['lanes'][0]
assert lane['status'] == 'passed', lane
backend = json.loads((Path(lane['evidence'])/'receipt.json').read_text())['backend_receipt']
assert backend['outcome'] == 'passed'
assert set(backend['observed_executions']) == {'a.unit','b.unit'}
assert (repo/'a/main.c').read_text().startswith('#error')
print('ok: exact pushed SHA, multi-ref impact union, real resolve/build/run evidence')

# Unknown changes broaden; a deliberate profile projection is visibly partial.
r, _ = gate('--head', head, '--base', a_head, '--broad', '--profile', 'native', '--explain')
p = json.loads(Path(r['executions'][0]['plan']).read_text())
assert p['effective_mode'] == 'broad' and p['partial_profiles'] == ['native']
assert r['status'] == 'planned'

# Dirty content uses a private index and snapshot; staged inputs overwritten in
# the worktree still enter selection. Content changes, not HEAD, change identity.
git('checkout', '--', 'a/main.c')
write('a/main.c', 'int main(void) { return 0; } /* staged */\n')
git('add', 'a/main.c')
write('a/main.c', 'int main(void) { return 0; } /* unstaged */\n')
write('unknown input\nname', 'unknown one\n')
(repo/'_build').mkdir()
write('_build/ignored', 'not a source input\n')
index = Path(git('rev-parse', '--git-path', 'index'))
if not index.is_absolute(): index = repo/index
index_before = index.read_bytes()
status_before = git('status', '--porcelain', '--untracked-files=all')
r, _ = gate('--base', head, '--explain')
p = json.loads(Path(r['executions'][0]['plan']).read_text())
digest = p['source_identity']['snapshot_digest']
assert p['source_identity']['kind'] == 'dirty'
assert p['effective_mode'] == 'broad'
paths = {x.get('new_path') for x in p['changes']}
assert 'unknown input\nname' in paths and not any(x and x.startswith('_build/') for x in paths)
assert index_before == index.read_bytes()
assert status_before == git('status', '--porcelain', '--untracked-files=all')
write('unknown input\nname', 'unknown two\n')
r, _ = gate('--base', head, '--explain')
p2 = json.loads(Path(r['executions'][0]['plan']).read_text())
assert p2['source_identity']['snapshot_digest'] != digest and p2['plan_id'] != p['plan_id']
r, _ = gate('--base', head, '--explain')
p3 = json.loads(Path(r['executions'][0]['plan']).read_text())
assert p2['source_identity'] == p3['source_identity'] and p2['plan_id'] == p3['plan_id']
print('ok: private dirty snapshot, staged/unstaged/unusual untracked inputs, stable content identity')

# The dirty snapshot also goes through the real backend, not only planning.
r, _ = gate('--base', head)
assert r['executions'][0]['lanes'][0]['status'] == 'passed', r
print('ok: real dirty snapshot CLI resolve/run')

# An unmaterializable escaping symlink fails instead of reading host content.
(repo/'external').symlink_to('/etc/passwd')
r, _ = gate('--base', head, '--explain', success=False)
assert 'symlink escapes' in r['errors'][0]
(repo/'external').unlink()

# Clean fixture input again; unavailable required capability cannot silently
# shrink a lane or be waived by legacy bypass flags.
git('reset', '--hard', head)
for p in (repo/'unknown input\nname', repo/'docs/untracked name\nwith newline.md'):
    p.unlink()
policy['profiles']['native']['capabilities'] = ['docker']
write('tests/selection/inventory.json', json.dumps(policy))
unsupported = commit()
r, _ = gate('--head', unsupported, '--base', head, success=False)
assert 'unimplemented guarded adapter' in r['executions'][0]['lanes'][0]['error'], r

# Real CTest cancellation, including a test descendant that ignores TERM.
# The socket signals actual execution readiness; no timing sleeps/retries.
policy['profiles']['native']['capabilities'] = []
policy['checks']['a.unit']['ctest_tests']['native'].append('a.block')
write('tests/selection/inventory.json', json.dumps(policy))
write('a/block.py', '''import json, os, signal, socket
reader, writer = os.pipe()
child = os.fork()
if not child:
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    os.write(writer, b"ready")
    signal.pause()
else:
    os.read(reader, 5)
    peer = socket.socket(socket.AF_UNIX)
    peer.connect(os.environ["GATE_TEST_SOCKET"])
    peer.sendall(json.dumps([os.getpid(), child]).encode())
    peer.close()
    signal.pause()
''')
write('CMakeLists.txt', (repo/'CMakeLists.txt').read_text().replace('nostrc_finalize_test_inventory()\n', ''))
with (repo/'CMakeLists.txt').open('a') as f:
    f.write('\nadd_test(NAME a.block COMMAND "'+sys.executable+'" "${CMAKE_CURRENT_SOURCE_DIR}/a/block.py")\n')
    f.write('nostrc_describe_test(a.block OWNER a INPUTS a/block.py)\nnostrc_finalize_test_inventory()\n')
blocked_head = commit()
server = socket.socket(socket.AF_UNIX)
# macOS Unix-domain paths are bounded to 104 bytes.
sockpath = '/tmp/gate-test-'+str(os.getpid())+'.sock'
server.bind(sockpath)
server.listen(1)
server.settimeout(30)
evidence = tmp/'cancel-evidence'
process = subprocess.Popen(['bash', str(repo/'scripts/test-gate.sh'), '--repo', str(repo),
    '--head', blocked_head, '--base', head, '--evidence-dir', str(evidence)],
    env=dict(os.environ, GATE_TEST_SOCKET=sockpath, PYTHONDONTWRITEBYTECODE='1'),
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
pids = []
try:
    peer, _ = server.accept()
    pids = json.loads(peer.recv(4096))
    peer.close()
    lockfile = Path('/tmp')/('nostrc-test-gate-'+str(os.getuid()))/(hashlib.sha256(b'native').hexdigest()+'.lock')
    with lockfile.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            raise AssertionError('execution did not retain the machine-wide profile lock')
        except BlockingIOError:
            pass
    process.terminate()
    output = process.communicate(timeout=20)[0]
    assert process.returncode == 130, output
    receipts = list(evidence.glob('*/adapter-receipt.json'))
    assert len(receipts) == 1 and json.loads(receipts[0].read_text())['status'] == 'cancelled'
    for pid in pids:
        state = subprocess.run(['ps','-o','stat=','-p',str(pid)], capture_output=True, text=True).stdout.strip()
        assert not state or state.startswith('Z'), ('owned child survived cancellation',pid,state)
    with lockfile.open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
finally:
    if process.poll() is None:
        process.terminate()
        process.communicate(timeout=20)
    for pid in pids:
        try: os.kill(pid, signal.SIGKILL)
        except ProcessLookupError: pass
    server.close()
    os.unlink(sockpath)
print('ok: actual CTest descendant cancellation, retained receipt, cross-checkout profile lock release')

# A bogus old endpoint broadens instead of becoming an empty change set.
r, _ = gate('--head', head, '--base', 'f'*40, '--explain')
p = json.loads(Path(r['executions'][0]['plan']).read_text())
assert p['effective_mode'] == 'broad' and p['broadening_reasons']
# All-zero deletion accepts any OID length and records no fake executions.
refs.write_text('(delete) ' + '0'*64 + ' refs/heads/gone ' + head + '\n')
r, _ = gate('--pre-push', '--refs-file', str(refs))
assert not r['executions'] and len(r['deleted_refs']) == 1
assert len(git('worktree', 'list', '--porcelain').split('worktree ')) == 2
print('ok: capability failure, unknown range fail-broad, generic deleted OIDs, cleanup')
PY
echo 'test-gate tests passed'
