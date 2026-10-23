#!/usr/bin/env bash
# Runs only inside the explicit disposable regression testhost; no native receipt.
set -uo pipefail
mkdir -p "$HOME" /evidence/logs
: > /evidence/commands.tsv
failed=0
run() {
  local name="$1" status
  shift
  printf '=== %s ===\n' "$name"
  "$@" > "/evidence/logs/$name.log" 2>&1
  status=$?
  cat "/evidence/logs/$name.log"
  printf '%s\t%s\n' "$name" "$status" >> /evidence/commands.tsv
  if (( status != 0 )); then failed=1; fi
}
# All mounts/setup are owned by Docker; no privileged init or host service setup.
# Mounting /tmp must not hide the root-owned canonical private X11 target.
run environment python3 - <<'PY'
import os, pathlib, stat
assert os.getuid() == os.getgid() == 1001
p = pathlib.Path('/tmp/.X11-unix')
s = p.lstat()
assert stat.S_ISDIR(s.st_mode) and s.st_uid == 0 and stat.S_IMODE(s.st_mode) == 0o1777
status = pathlib.Path('/proc/self/status').read_text()
for key in ('CapEff', 'CapPrm', 'CapBnd'):
    assert int(next(line.split()[1] for line in status.splitlines() if line.startswith(key + ':')), 16) == 0
assert 'NoNewPrivs:\t1' in status
print(status)
print(os.uname())
PY
run mapping-probe python3 tests/selection/namespace_host_probe.py
run kernel-prerequisite python3 scripts/adapter_namespace_custody.py python3 -c \
  'import os; assert os.getpid() == 2; print("actual private PID/proc mount prerequisite")'
if (( failed == 0 )); then
  # Separate mandatory zero-skip kernel matrix, not a retry of a failed suite.
  run kernel-matrix env PYTHONPATH=tests/selection python3 -m unittest -v \
    test_namespace_custody test_namespace_wrapper_adapter test_native_services test_adapter_custody
  run policy-suite python3 -m unittest discover -s tests/selection -v
  # Preserve the broad static family's original finite checks as well.
  run unsequenced python3 scripts/check-unsequenced-args.py --self-test
  run gui-frame python3 scripts/check-gui-test-gdk-frame.py --self-test
  run linux-packages python3 scripts/check-linux-ci-packages.py --self-test
  run pre-push bash scripts/test-pre-push.sh
  run full-gate bash scripts/test-full-gate.sh
  run linux-smoke bash scripts/test-linux-gate-smoke.sh
  run sanitizer bash scripts/test-sanitizer-gate.sh
  run signer-names bash scripts/check-grotto-names.sh
  # Synthetic guard fixtures cannot live under a /tmp pathname; no real keychain is used.
  run keychain-guard env XDG_CACHE_HOME=/evidence/keychain-cache bash scripts/test-check-macos-keychain.sh
fi
python3 - <<'PY'
import hashlib, json, os, pathlib, re, shutil, subprocess
root = pathlib.Path('/evidence')
commands = {name: int(code) for name, code in (line.split() for line in (root / 'commands.tsv').read_text().splitlines())}
tools = {}
for name in ('python3', 'git', 'cc', 'cmake', 'ninja', 'Xvfb', 'dbus-daemon', 'gnome-keyring-daemon'):
    path = pathlib.Path(shutil.which(name)).resolve()
    tools[name] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
required = {'environment', 'mapping-probe', 'kernel-prerequisite', 'kernel-matrix', 'policy-suite',
            'unsequenced', 'gui-frame', 'linux-packages', 'pre-push', 'full-gate', 'linux-smoke',
            'sanitizer', 'signer-names', 'keychain-guard'}
matrix_log = root / 'logs/kernel-matrix.log'
matrix_text = matrix_log.read_text(errors='replace') if matrix_log.exists() else ''
summary = re.search(r'^Ran (\d+) tests? in ', matrix_text, re.M)
matrix = dict(tests=int(summary[1]) if summary else 0,
              skipped=sum(int(n) for n in re.findall(r'skipped=(\d+)', matrix_text)))
passed = (set(commands) == required and all(code == 0 for code in commands.values())
          and matrix['tests'] >= 79 and matrix['skipped'] == 0)
record = dict(kind='namespace_policy_regression_v1',
    scope='provisioned_container_regressions_not_authored_native_profile',
    source_sha=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
    kernel=list(os.uname()), uid=os.getuid(), gid=os.getgid(), tools=tools,
    commands=commands, kernel_matrix=matrix, not_executed=sorted(required - commands.keys()), passed=passed,
    complete=False, complete_gate_eligible=False, native_profile_acceptance=False)
(root / 'regression.json').write_text(json.dumps(record, indent=2) + '\n')
if not passed:
    raise SystemExit(1)
PY
receipt_status=$?
if (( receipt_status != 0 )); then failed=1; fi
exit "$failed"
