#!/usr/bin/env bash
# Tests scripts/linux-gate-smoke.sh (nostrc-16yi) with a scripted ctest: a
# test that fails in the parallel run and passes alone keeps its first
# output in the gate log and in the volume's history, and its reruns are
# counted across gates; a failure both times blocks. In the sanitizer set's
# mode (nostrc-3han), a failure with a sanitizer report blocks without a
# rerun and prints its whole output, and a skipped test blocks.
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
printf 'XVFB\n' >> "$TRACE"
while [ "${1:-}" = -a ] || [ "${1:-}" = -s ]; do
    if [ "$1" = -s ]; then shift; fi
    shift
done
exec "$@"
MOCK
# FIRST_FAIL: tests failing in the parallel run; RERUN_FAIL: failing alone.
# RERUN_SHORT=1: the rerun runs no test at all. REPORT: failing tests whose
# output holds a sanitizer report (RERUN_REPORT: in the rerun); SKIP: tests
# CTest reports as skipped.
cat > "$tmp/bin/ctest" <<'MOCK'
#!/bin/bash
printf 'CTEST %s\n' "$*" >> "$TRACE"
case " $* " in *" --output-on-failure "*) ;; *) echo "no --output-on-failure" >&2; exit 9 ;; esac
tests="alpha beta gamma"
fail="$FIRST_FAIL"
run=first
case " $* " in *" --parallel "*) ;; *) fail="$RERUN_FAIL"; run=rerun ;; esac
[ "$run" = first ] || [ -z "${CTEST_PARALLEL_LEVEL:-}" ] || printf 'PARALLEL_RERUN\n' >> "$TRACE"
selected="$tests"
for ((i = 1; i <= $#; i++)); do
    if [ "${!i}" = -R ]; then
        j=$((i + 1))
        regex="${!j}"
        selected=""
        [ "$run" = first ] || [ "${RERUN_SHORT:-0}" != 1 ] ||  continue
        for t in $tests; do [[ "$t" =~ $regex ]] && selected="$selected $t"; done
    fi
done
report="${REPORT:-}"
[ "$run" = first ] || report="${RERUN_REPORT:-}"
n=0; total=$(echo $selected | wc -w | tr -d ' '); failed=""
for t in $selected; do
    n=$((n + 1))
    echo "      Start $n: $t"
    if [[ " $fail " == *" $t "* ]]; then
        echo "$n/$total Test #$n: $t .........***Failed    0.10 sec"
        echo "early line of $t"
        echo "OUTPUT-OF-$t-$run"
        if [[ " $report " == *" $t "* ]]; then
            # Stacks follow the first line of a real report, often thousands
            # of lines: its reader must not stop at that line.
            echo "==1==ERROR: LeakSanitizer: detected memory leaks"
            for ((k = 0; k < ${REPORT_LINES:-0}; k++)); do echo "    #$k 0xffff in frame_$k"; done
            echo "SUMMARY: AddressSanitizer: 65 byte(s) leaked in 1 allocation(s)."
        fi
        failed="$failed $t"
    elif [[ " ${SKIP:-} " == *" $t "* ]]; then
        echo "$n/$total Test #$n: $t .........***Skipped   0.10 sec"
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
        TAIL_LINES="${TAIL_LINES:-1}" bash "$scripts/linux-gate-smoke.sh"
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

# ---- the sanitizer set's mode (linux-gate.sh --sanitizers) ----
: > "$tmp/state/gate-history/gates"
: > "$tmp/state/gate-history/reruns"
sanitizers() {
    TEST_REGEX='^(alpha|beta|gamma)$' DISPLAY_WRAP=0 CTEST_TIMEOUT='' TAIL_LINES=all \
        FORBID_PATTERN='\*\*\*Skipped|Not Run' SANITIZER_REPORTS=block \
        GATE="Sanitizer gate" SUITE="sanitizer tests" gate
}
FIRST_FAIL="" RERUN_FAIL="" sanitizers > "$tmp/out" 2>&1 || fail "a clean sanitizer run failed"
grep -q 'Sanitizer gate: sanitizer tests passed, 3 run' "$tmp/out" || fail "no sanitizer pass line"
! grep -q '^XVFB' "$tmp/trace" || fail "the sanitizer set ran under a display (the job has none)"
! grep -q -- '--timeout' "$tmp/trace" || fail "the sanitizer set overrode CTest's timeout"
grep -qF -- '-R ^(alpha|beta|gamma)$' "$tmp/trace" || fail "TEST_REGEX did not select"
! grep -qF -- ' -E ' "$tmp/trace" || fail "the smoke exclusion was applied to the sanitizer set"

# A leak report fails the gate at once: no rerun, the whole output printed.
if FIRST_FAIL="beta" RERUN_FAIL="" REPORT="beta" sanitizers > "$tmp/out" 2>&1; then
    fail "a sanitizer report passed the gate"
fi
[ "$(grep -c '^CTEST' "$tmp/trace")" -eq 1 ] || fail "a test with a sanitizer report was rerun"
grep -q 'early line of beta' "$tmp/out" || fail "the report's whole output was not printed"
grep -q 'SUMMARY: AddressSanitizer: 65 byte' "$tmp/out" || fail "the report was not printed"
grep -q 'sanitizer report in: beta' "$tmp/out" || fail "the reporting test is not named"
grep -q 'SANITIZER TESTS FAILED' "$tmp/out" || fail "no sanitizer failure line"
! grep -q RERUN "$tmp/out" || fail "a report was treated as a flake"
# ... however long the output before it.
if FIRST_FAIL="beta" RERUN_FAIL="" REPORT="beta" REPORT_LINES=20000 sanitizers > "$tmp/out" 2>&1; then
    fail "a sanitizer report after a long output passed the gate"
fi
grep -q 'sanitizer report in: beta' "$tmp/out" || fail "a report after a long output was missed"
[ "$(grep -c '^CTEST' "$tmp/trace")" -eq 1 ] || fail "a long report was rerun"
# ... also beside a failure without one: nothing is rerun.
if FIRST_FAIL="alpha beta" RERUN_FAIL="" REPORT="beta" sanitizers > "$tmp/out" 2>&1; then
    fail "a sanitizer report beside a flake passed the gate"
fi
[ "$(grep -c '^CTEST' "$tmp/trace")" -eq 1 ] || fail "rerun beside a sanitizer report"
grep -q 'OUTPUT-OF-alpha-first' "$tmp/out" || fail "the other failure's output was not printed"

# A failure without a report is a flake as in the smoke run: rerun, counted.
FIRST_FAIL="gamma" RERUN_FAIL="" sanitizers > "$tmp/out" 2>&1 || fail "a sanitizer-set flake failed the gate"
grep -q 'RERUN: failed in the parallel run, passed alone: gamma' "$tmp/out" || fail "sanitizer rerun not surfaced"
grep -q 'gamma needed a rerun in 1 of the last' "$tmp/out" || fail "sanitizer rerun not counted"
grep -q 'sanitizer tests passed after a rerun' "$tmp/out" || fail "no sanitizer pass-after-rerun line"
# ... but a report in the rerun blocks.
if FIRST_FAIL="gamma" RERUN_FAIL="gamma" RERUN_REPORT="gamma" sanitizers > "$tmp/out" 2>&1; then
    fail "a report in the rerun passed the gate"
fi
grep -q 'the rerun alone failed too' "$tmp/out" || fail "no rerun failure line"
grep -q 'SUMMARY: AddressSanitizer' "$tmp/out" || fail "the rerun's report was not printed"

# A skipped test fails the set, as the job's check does.
if FIRST_FAIL="" RERUN_FAIL="" SKIP="alpha" sanitizers > "$tmp/out" 2>&1; then
    fail "a skipped sanitizer test passed the gate"
fi
grep -q 'a test was skipped or did not run' "$tmp/out" || fail "the skip is not named"
# The smoke run allows skips (FORBID_PATTERN unset).
FIRST_FAIL="" RERUN_FAIL="" SKIP="alpha" gate > "$tmp/out" 2>&1 || fail "a smoke skip failed the gate"

# ---- the macOS stage's mode (scripts/pre-push, nostrc-7c1v) ----
# History outside STATE_DIR, no volume, and a serial rerun even when the
# caller's environment sets CTEST_PARALLEL_LEVEL.
macos() {
    : > "$tmp/trace"
    PATH="$tmp/bin:$PATH" TRACE="$tmp/trace" JOBS=2 TEST_REGEX=. DISPLAY_WRAP=0 CTEST_TIMEOUT='' \
        BUILD_DIR="$tmp/build" STATE_DIR="$tmp/macos-state" HISTORY_DIR="$tmp/macos-history" \
        GATE="macOS gate" SUITE="tests" CTEST_PARALLEL_LEVEL=8 \
        bash "$scripts/linux-gate-smoke.sh"
}
mkdir -p "$tmp/macos-state"
FIRST_FAIL="beta" RERUN_FAIL="" macos > "$tmp/out" 2>&1 || fail "a macOS flake failed the gate"
! grep -q '^PARALLEL_RERUN$' "$tmp/trace" || fail "the macOS rerun ran in parallel"
grep -q 'macOS gate: RERUN: failed in the parallel run, passed alone: beta' "$tmp/out" || fail "macOS rerun not surfaced"
grep -q $'\tbeta$' "$tmp/macos-history/reruns" || fail "macOS rerun not counted in HISTORY_DIR"
ls "$tmp/macos-history/"*-first-run.log > /dev/null || fail "macOS first-run log not kept in HISTORY_DIR"
[ ! -e "$tmp/macos-state/gate-history" ] || fail "HISTORY_DIR was ignored"
! grep -q 'in volume' "$tmp/out" || fail "a volume was named without VOLUME"

# ---- real CMake/CTest (their output format, not the mock's) ----
if command -v cmake > /dev/null && command -v ctest > /dev/null; then
    real="$tmp/real"
    mkdir -p "$real/src" "$real/state"
    cat > "$real/src/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(gate_smoke_selftest NONE)
enable_testing()
add_test(NAME flaky_dummy COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/flaky.sh" "${CMAKE_CURRENT_BINARY_DIR}/flaky-count")
add_test(NAME steady_dummy COMMAND sh -c "echo run >> '${CMAKE_CURRENT_BINARY_DIR}/steady-count'")
CMAKE
    # Fails its first run, passes later ones, unless ALWAYS_FAIL=1.
    cat > "$real/src/flaky.sh" <<'FLAKY'
n=$(( $(cat "$1" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$1"
echo "flaky_dummy run $n"
[ "$n" -ge 2 ] && [ "${ALWAYS_FAIL:-0}" != 1 ]
FLAKY
    cmake -S "$real/src" -B "$real/build" > /dev/null
    real_gate() {
        JOBS=2 TEST_REGEX=. DISPLAY_WRAP=0 BUILD_DIR="$real/build" STATE_DIR="$real/state" \
            GATE="Real gate" SUITE="tests" bash "$scripts/linux-gate-smoke.sh"
    }
    real_gate > "$tmp/out" 2>&1 || { cat "$tmp/out"; fail "real CTest: a flake failed the gate"; }
    grep -q 'RERUN: failed in the parallel run, passed alone: flaky_dummy$' "$tmp/out" || fail "real CTest: rerun not surfaced"
    grep -q 'flaky_dummy run 1' "$tmp/out" || fail "real CTest: first output not printed"
    [ "$(cat "$real/build/flaky-count")" = 2 ] || fail "real CTest: flaky_dummy not run exactly twice"
    [ "$(wc -l < "$real/build/steady-count" | tr -d ' ')" = 1 ] || fail "real CTest: a passing test was rerun"
    rm -f "$real/build/flaky-count"
    if ALWAYS_FAIL=1 real_gate > "$tmp/out" 2>&1; then fail "real CTest: a failing rerun passed"; fi
    grep -q 'the rerun alone failed too' "$tmp/out" || fail "real CTest: no rerun failure line"
else
    echo "(no cmake/ctest: the real-CTest case is skipped)"
fi

echo 'linux gate smoke tests passed'
