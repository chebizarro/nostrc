#!/usr/bin/env bash
# Tests scripts/linux-gate-smoke.sh (nostrc-16yi) with a scripted ctest: a
# test that fails in the parallel run and passes alone keeps its first
# output in the gate log and in the volume's history, and its reruns are
# counted across gates; a failure both times blocks.
set -euo pipefail
scripts="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/state"

cat > "$tmp/bin/dbus-run-session" <<'MOCK'
#!/bin/bash
[ "$1" = -- ] && shift
exec "$@"
MOCK
cat > "$tmp/bin/xvfb-run" <<'MOCK'
#!/bin/bash
while [ "${1:-}" = -a ] || [ "${1:-}" = -s ]; do
    if [ "$1" = -s ]; then shift; fi
    shift
done
exec "$@"
MOCK
# FIRST_FAIL: tests failing in the parallel run; RERUN_FAIL: failing alone.
# RERUN_SHORT=1: the rerun runs no test at all.
cat > "$tmp/bin/ctest" <<'MOCK'
#!/bin/bash
printf 'CTEST %s\n' "$*" >> "$TRACE"
case " $* " in *" --output-on-failure "*) ;; *) echo "no --output-on-failure" >&2; exit 9 ;; esac
tests="alpha beta gamma"
fail="$FIRST_FAIL"
run=first
selected="$tests"
for ((i = 1; i <= $#; i++)); do
    if [ "${!i}" = -R ]; then
        j=$((i + 1))
        regex="${!j}"
        selected=""
        [ "${RERUN_SHORT:-0}" = 1 ] || for t in $tests; do [[ "$t" =~ $regex ]] && selected="$selected $t"; done
        fail="$RERUN_FAIL"
        run=rerun
    fi
done
n=0; total=$(echo $selected | wc -w | tr -d ' '); failed=""
for t in $selected; do
    n=$((n + 1))
    echo "      Start $n: $t"
    if [[ " $fail " == *" $t "* ]]; then
        echo "$n/$total Test #$n: $t .........***Failed    0.10 sec"
        echo "early line of $t"
        echo "OUTPUT-OF-$t-$run"
        failed="$failed $t"
    else
        echo "$n/$total Test #$n: $t .........   Passed    0.10 sec"
    fi
done
nf=$(echo $failed | wc -w | tr -d ' ')
echo
echo "$(( total ? (total - nf) * 100 / total : 0 ))% tests passed, $nf tests failed out of $total"
if [ "$nf" -gt 0 ]; then
    echo
    echo "The following tests FAILED:"
    i=0; for t in $failed; do i=$((i + 1)); echo "	  $i - $t (Failed)"; done
    echo "Errors while running CTest"
    exit 8
fi
MOCK
chmod +x "$tmp/bin/"*

gate() {
    : > "$tmp/trace"
    PATH="$tmp/bin:$PATH" TRACE="$tmp/trace" JOBS=4 SMOKE_EXCLUDE='^slow$' \
        BUILD_DIR="$tmp/build" STATE_DIR="$tmp/state" VOLUME=test-volume HISTORY_KEEP=3 \
        TAIL_LINES=1 bash "$scripts/linux-gate-smoke.sh"
}
fail() { echo "FAIL: $*" >&2; exit 1; }

# All pass: one run, nothing about reruns.
FIRST_FAIL="" RERUN_FAIL="" gate > "$tmp/out" 2>&1 || fail "a clean run failed"
grep -q 'smoke tests passed, 3 run' "$tmp/out" || fail "no pass line"
[ "$(grep -c '^CTEST' "$tmp/trace")" -eq 1 ] || fail "a clean run reran"
! grep -q RERUN "$tmp/out" || fail "a clean run reported a rerun"

# beta fails in the parallel run and passes alone: the gate passes, prints
# beta's first output (its last TAIL_LINES lines), keeps the log, counts it.
FIRST_FAIL="beta" RERUN_FAIL="" gate > "$tmp/out" 2>&1 || fail "a passing rerun failed the gate"
grep -q 'OUTPUT-OF-beta-first' "$tmp/out" || fail "the first failure's output was not printed"
! grep -q 'early line of beta' "$tmp/out" || fail "TAIL_LINES was not applied"
grep -q -- '---- beta: output of its failed run (2 lines' "$tmp/out" || fail "no output header"
grep -q 'RERUN: failed in the parallel run, passed alone: beta' "$tmp/out" || fail "rerun not surfaced"
grep -q 'beta needed a rerun in 1 of the last 2 gate(s)' "$tmp/out" || fail "rerun not counted"
grep -q 'smoke tests passed after a rerun' "$tmp/out" || fail "no pass-after-rerun line"
grep -qF -- "-R ^(beta)\$" "$tmp/trace" || fail "the rerun did not select beta by name"
kept="$(ls "$tmp/state/gate-history/"*-first-run.log)"
grep -q 'OUTPUT-OF-beta-first' "$kept" || fail "the first run's log was not kept"
grep -q "$(basename "$kept")" "$tmp/out" || fail "the kept log is not named"

# Again: counted across gates; only the last HISTORY_KEEP (3) gates count, and
# only that many first-run logs are kept.
FIRST_FAIL="beta" RERUN_FAIL="" gate > "$tmp/out" 2>&1 || fail "second rerun gate failed"
grep -q 'beta needed a rerun in 2 of the last 3 gate(s)' "$tmp/out" || fail "second rerun not counted"
FIRST_FAIL="beta gamma" RERUN_FAIL="" gate > "$tmp/out" 2>&1 || fail "third rerun gate failed"
grep -q 'beta needed a rerun in 3 of the last 3 gate(s)' "$tmp/out" || fail "third rerun not counted"
grep -q 'gamma needed a rerun in 1 of the last 3 gate(s)' "$tmp/out" || fail "gamma not counted"
grep -q 'OUTPUT-OF-gamma-first' "$tmp/out" || fail "gamma's first output missing"
FIRST_FAIL="" RERUN_FAIL="" gate > /dev/null 2>&1 || fail "clean gate failed"
FIRST_FAIL="beta" RERUN_FAIL="" gate > "$tmp/out" 2>&1 || fail "fifth gate failed"
grep -q 'beta needed a rerun in 2 of the last 3 gate(s)' "$tmp/out" || fail "old gates still counted"
[ "$(ls "$tmp/state/gate-history/"*-first-run.log | wc -l | tr -d ' ')" -eq 3 ] ||
    fail "first-run logs not pruned to HISTORY_KEEP"

# A failure both times blocks, with both outputs.
if FIRST_FAIL="alpha" RERUN_FAIL="alpha" gate > "$tmp/out" 2>&1; then
    fail "a test failing alone too passed the gate"
fi
grep -q 'OUTPUT-OF-alpha-first' "$tmp/out" || fail "first output missing on a real failure"
grep -q 'OUTPUT-OF-alpha-rerun' "$tmp/out" || fail "rerun output missing on a real failure"
grep -q 'SMOKE TESTS FAILED' "$tmp/out" || fail "no failure line"

# A rerun that ran nothing is not a pass.
if FIRST_FAIL="alpha" RERUN_FAIL="" RERUN_SHORT=1 gate > "$tmp/out" 2>&1; then
    fail "an empty rerun passed the gate"
fi
grep -q 'the rerun ran 0 test(s) for: alpha' "$tmp/out" || fail "empty rerun not named"

echo 'linux gate smoke tests passed'
