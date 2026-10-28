#!/usr/bin/env bash
# TEMPORARY DIAGNOSTIC (nostrc-2zcw9) -- never merged. Re-runs the
# groundhog-mls-ui-gui prefix that aborted on hosted CI (tests 1-5, seed 29,
# 50 ms jitter) in concurrent workers until the first failure or the budget,
# leaving the failing log and core in GH_TEST_EVIDENCE_DIR.
set -euo pipefail
build="${1:-build}" workers="${2:-4}" budget="${3:-900}"
# One session bus and display for every worker, as the stress step's CTest
# run: separate dbus-run-sessions share the user's AT-SPI socket and break
# each other's accessibility bus on teardown.
if [ -z "${DIAG_2ZCW9_INNER:-}" ]; then
  export DIAG_2ZCW9_INNER=1
  exec dbus-run-session -- xvfb-run -a -s '-screen 0 1280x800x24' "$0" "$@"
fi
evidence="${GH_TEST_EVIDENCE_DIR:?}"
mkdir -p "$evidence"
ulimit -c unlimited
exe=
eval "$(python3 - "$build" <<'PY'
import json, shlex, subprocess, sys
data = json.loads(subprocess.check_output(['ctest', '--test-dir', sys.argv[1], '--show-only=json-v1',
                                           '-R', '^groundhog-mls-ui-gui$'], text=True))
t = data['tests'][0]
print('exe=' + shlex.quote(t['command'][0]))
for p in t.get('properties', []):
    if p['name'] == 'ENVIRONMENT':
        for kv in p['value']:
            k, v = kv.split('=', 1)
            print(f'export {k}={shlex.quote(v)}')
PY
)"
export GH_TEST_ASYNC_SEED=29 GH_TEST_ASYNC_JITTER_MS=50
paths=()
for t in flag-new-group picker-boundaries new-group new-group-formats new-group-format-changed; do
  paths+=(-p "/groundhog/mls-ui-gui/$t")
done
deadline=$(( $(date +%s) + budget ))
fail="$evidence/FAILED"
work() {
  local w="$1" n=0 log
  while [ "$(date +%s)" -lt "$deadline" ] && [ ! -e "$fail" ]; do
    n=$((n + 1)); log="$evidence/w$w-run$n.log"
    if "$exe" --gui "${paths[@]}" > "$log" 2>&1; then
      rm -f "$log"
    else
      echo "worker $w run $n FAILED: $log"; tail -n 5 "$log"; touch "$fail"; return
    fi
  done
  echo "worker $w: $n runs"
}
# Load as in the stress step: the other jittered GUI suites beside it.
load() {
  local name="$1" n=0 log
  while [ "$(date +%s)" -lt "$deadline" ] && [ ! -e "$fail" ]; do
    n=$((n + 1)); log="$evidence/$name-run$n.log"
    if ctest --test-dir "$build" --no-tests=error --output-on-failure -R "^$name\$" > "$log" 2>&1; then
      rm -f "$log"
    else
      echo "load $name run $n FAILED: $log"; tail -n 20 "$log"; touch "$fail"; return
    fi
  done
  echo "load $name: $n runs"
}
for w in $(seq "$workers"); do work "$w" & done
for name in ${DIAG_2ZCW9_LOAD:-groundhog-composer groundhog-attachment-ui groundhog-conversation-menu-gui}; do
  load "$name" &
done
wait
if [ -e "$fail" ]; then
  ls -la "$evidence"
  for core in "$evidence"/core*; do
    [ -f "$core" ] || continue
    bin="${core##*/}"; bin="${bin#core.*.}"; bin="${bin//!//}"
    DEBUGINFOD_URLS=https://debuginfod.ubuntu.com timeout 900 gdb -nx -batch -iex 'set debuginfod enabled on' -ex 'set print frame-arguments all' \
      -ex 'bt full' -ex 'thread apply all bt full' "$bin" -c "$core" > "$core.bt" 2>&1 || true
    sed -n '1,/^Thread [0-9]* (/p' "$core.bt"
  done
  exit 1
fi
echo "no failure within ${budget}s"
