#!/usr/bin/env bash
#
# linux-gate-smoke.sh — the Linux gate's CTest run, inside its container
# (scripts/linux-gate.sh mounts this file next to the build; nostrc-16yi): the
# smoke subset, or with --sanitizers the Groundhog sanitizer set (nostrc-3han).
# scripts/pre-push also runs it on the host for the macOS stage's full CTest
# run (nostrc-7c1v), so both stages rerun and report alike.
#
# The tests run beside the other stages' builds. A test that fails is run
# once more on its own (serially, whatever CTEST_PARALLEL_LEVEL says, and
# selected by its exact name): that absorbs a race lost under that load,
# while a real break fails both times. The rerun passes only if every test
# that failed shows "Passed" in it; Skipped (a SKIP_RETURN_CODE such as 77)
# or Not Run is a failure (W25 review L1). More than RERUN_MAX failed tests
# are a break, not a flake: they block without a rerun (W25 review N5).
# A rerun is never silent or forgotten:
#  - the first run keeps each failed test's output (--output-on-failure); it
#    is printed (the last TAIL_LINES lines of each) even when the rerun
#    passes, and the whole log is kept in STATE_DIR/gate-history/ (the newest
#    HISTORY_KEEP runs' logs);
#  - every rerun is counted in STATE_DIR/gate-history/reruns, one line per
#    gate and test, and the gate says how many of the last HISTORY_KEEP gates
#    needed a rerun of the same test. A test that keeps needing one is a flake
#    to fix, not to absorb.
# With SANITIZER_REPORTS=block, a failed test whose output holds a sanitizer
# report (ASAN, LSan, UBSAN) is not rerun: leaks and undefined behaviour are
# deterministic enough that a passing rerun would only hide one.
#
# Environment: JOBS, and SMOKE_EXCLUDE (a ctest -E regex) or TEST_REGEX (a
# ctest -R regex, which wins). Optional: BUILD_DIR (/work/build), STATE_DIR
# (/work: the run's logs), HISTORY_DIR (STATE_DIR/gate-history), VOLUME (the
# volume holding them, named in messages), GATE_SECONDS (elapsed seconds of the gate
# so far), HISTORY_KEEP (20), TAIL_LINES (200, or "all"), GATE ("Linux gate")
# and SUITE ("smoke tests") for messages, DISPLAY_WRAP (1: under
# dbus-run-session and xvfb-run; 0: bare), CTEST_TIMEOUT (120; empty: CTest's
# default), FORBID_PATTERN (an ERE no ctest log may match, e.g. skipped tests),
# REQUIRED_PERF_TEST (must be selected with the perf label and pass),
# SANITIZER_REPORTS (block, or empty), RERUN_MAX (5), PROGRESS (1: stream each
# test's result line while the log is written; 0: summary only). An
# interrupted run (SIGINT, SIGTERM) keeps its log so far in the history.
set -euo pipefail
{

BUILD_DIR="${BUILD_DIR:-/work/build}"
STATE_DIR="${STATE_DIR:-/work}"
HISTORY_KEEP="${HISTORY_KEEP:-20}"
TAIL_LINES="${TAIL_LINES:-200}"
GATE="${GATE:-Linux gate}"
SUITE="${SUITE:-smoke tests}"
SUITE_FAILED="$(printf "%s" "$SUITE" | tr "[:lower:]" "[:upper:]") FAILED"
DISPLAY_WRAP="${DISPLAY_WRAP:-1}"
CTEST_TIMEOUT="${CTEST_TIMEOUT-120}"
FORBID_PATTERN="${FORBID_PATTERN:-}"
SANITIZER_REPORTS="${SANITIZER_REPORTS:-}"
REQUIRED_PERF_TEST="${REQUIRED_PERF_TEST:-}"
RERUN_MAX="${RERUN_MAX:-5}"
PROGRESS="${PROGRESS:-0}"
# What the first run was, for messages (W25 review N3: the macOS stage's
# first run is serial unless CTEST_PARALLEL_LEVEL says otherwise).
if [ "$JOBS" -gt 1 ]; then FIRST_RUN="the parallel run"; else FIRST_RUN="the first run"; fi
if [ -n "${TEST_REGEX:-}" ]; then
  SELECT=(-R "$TEST_REGEX")
else
  SELECT=(-E "$SMOKE_EXCLUDE")
fi
SECONDS="${GATE_SECONDS:-0}"
HISTORY="${HISTORY_DIR:-$STATE_DIR/gate-history}"
WHERE="${VOLUME:+ in volume $VOLUME}"
# The first run passes --parallel; the rerun must run alone.
unset CTEST_PARALLEL_LEVEL
mkdir -p "$HISTORY"
touch "$HISTORY/gates" "$HISTORY/reruns"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
printf '%s\n' "$RUN_ID" >> "$HISTORY/gates"

stamp() { printf "%dm%02ds" $((SECONDS / 60)) $((SECONDS % 60)); }

# Keep the newest HISTORY_KEEP kept logs (names sort by time).
prune_history() {
  find "$HISTORY" \( -name "*-first-run.log" -o -name "*-interrupted.log" \) | sort |
    awk -v keep="$HISTORY_KEEP" '{ name[NR] = $0 } END { for (i = 1; i <= NR - keep; i++) print name[i] }' |
    while read -r old; do rm -f "$old"; done
}

# Interrupted (Ctrl-C reaches ctest and this script; the hook's EXIT trap
# then removes STATE_DIR): the log so far goes to the history (W25 review N4).
CURRENT_LOG=""
on_interrupt() {
  trap - INT TERM
  if [ -n "$CURRENT_LOG" ] && [ -f "$CURRENT_LOG" ]; then
    cp "$CURRENT_LOG" "$HISTORY/$RUN_ID-interrupted.log"
    echo "==> $GATE: interrupted ($(stamp)); the log so far: $HISTORY/$RUN_ID-interrupted.log$WHERE" >&2
  fi
  exit 130
}
trap on_interrupt INT TERM

# Widget tests need a display and a session bus, as in groundhog-ci.yml.
smoke() {
  local cmd=(ctest --test-dir "$BUILD_DIR")
  [ -z "$CTEST_TIMEOUT" ] || cmd+=(--timeout "$CTEST_TIMEOUT")
  cmd+=(--no-tests=error --output-on-failure "$@")
  if [ "$DISPLAY_WRAP" = 1 ]; then
    dbus-run-session -- xvfb-run -a -s "-screen 0 1280x800x24" "${cmd[@]}"
  else
    "${cmd[@]}"
  fi
}

# smoke "$@" into log $1, its status returned. With PROGRESS=1 each test's
# result line is also printed as it comes (W25 review N4: the macOS stage was
# silent for its whole run).
run_logged() {
  local log="$1" status=0
  shift
  CURRENT_LOG="$log"
  if [ "$PROGRESS" = 1 ]; then
    set +e
    smoke "$@" 2>&1 | tee "$log" |
      awk '/^ *[0-9]+\/[0-9]+ Test +#[0-9]+: / { print "   " $0; fflush() }'
    status=${PIPESTATUS[0]}
    set -e
  else
    smoke "$@" > "$log" 2>&1 || status=$?
  fi
  CURRENT_LOG=""
  return "$status"
}

# Test name $1 as an exact CTest regex: its regex characters escaped.
exact_re() {
  printf '%s' "$1" | sed 's/[][\.*^$+?(){}|]/\\&/g'
}

# What rerun log $1 says of test $2: "Passed", its CTest status otherwise
# (Failed, Skipped, Not Run, Timeout, ...), or "not run" without a result.
rerun_status() {
  awk -v name="$2" '
    /^ *[0-9]+\/[0-9]+ Test +#[0-9]+: / && $4 == name {
      if ($(NF - 2) == "Passed") {
        status = "Passed"
      } else {
        status = $0
        sub(/^[^*]*\*\*\*/, "", status)
        sub(/ +[0-9.]+ sec$/, "", status)
      }
    }
    END { print (status == "" ? "not run" : status) }' "$1"
}

# The names of the tests a ctest log lists as failed, space-separated.
failed_in() {
  sed -n "/The following tests FAILED/,/^Errors while running/ s/^[[:space:]]*[0-9]* - \([^ ]*\) (.*/\1/p" "$1" |
    paste -sd " " -
}

# What --output-on-failure printed for test $2 in log $1: the lines after its
# result line, up to the next test's start or result, or the summary, without
# trailing blank lines.
output_of() {
  awk -v name="$2" '
    /^ *[0-9]+\/[0-9]+ Test +#[0-9]+: / { on = ($4 == name); blank = 0; next }
    /^ +Start +[0-9]+: / || /^[0-9]+% tests passed/ { on = 0 }
    !on { next }
    /^[[:space:]]*$/ { blank++; next }
    { for (; blank > 0; blank--) print ""; print }
  ' "$1"
}

print_failures() {
  local log="$1" test lines
  shift
  for test in "$@"; do
    lines="$(output_of "$log" "$test" | wc -l | tr -d " ")"
    if [ "$TAIL_LINES" = all ]; then
      echo "---- $test: output of its failed run ($lines lines) ----"
      output_of "$log" "$test"
    else
      echo "---- $test: output of its failed run ($lines lines; the last $TAIL_LINES follow) ----"
      output_of "$log" "$test" | tail -n "$TAIL_LINES"
    fi
    echo "---- end of $test ----"
  done
}

# A report of AddressSanitizer, LeakSanitizer (its SUMMARY is AddressSanitizer's
# when both run) or UndefinedBehaviorSanitizer in test $2's output in log $1.
SANITIZER_REPORT_RE='(ERROR|SUMMARY): [A-Za-z]+Sanitizer|: runtime error: '
# (grep without -q reads to the end: with -q it can stop early and the pipe's
# writer, killed by SIGPIPE, would fail the pipeline under pipefail.)
has_report() {
  output_of "$1" "$2" | grep -E "$SANITIZER_REPORT_RE" > /dev/null
}

# Fail when log $1 matches FORBID_PATTERN (the sanitizer job's "***Skipped|Not
# Run": a test that did not run proves nothing).
check_forbidden() {
  [ -n "$FORBID_PATTERN" ] || return 0
  if grep -Eq -- "$FORBID_PATTERN" "$1"; then
    grep -E -- "$FORBID_PATTERN" "$1" | head -20
    echo "==> $GATE: $SUITE_FAILED ($(stamp)): a test was skipped or did not run (/$FORBID_PATTERN/)"
    exit 1
  fi
}

# The last HISTORY_KEEP gates, oldest first.
recent_gates() {
  tail -n "$HISTORY_KEEP" "$HISTORY/gates"
}

# A perf-labeled test is not a parallel-smoke flake. Run it once, serially,
# after the functional suite. A failure here blocks; RERUN_MAX applies only
# to the parallel functional run.
run_perf() {
  local count expected listing log
  listing="$(ctest --test-dir "$BUILD_DIR" -N -L '^perf$' "${SELECT[@]}")"
  count="$(printf '%s\n' "$listing" | awk '/Total Tests:/ { print $3 }')"
  if [ -n "$REQUIRED_PERF_TEST" ]; then
    expected="$(printf '%s\n' "$listing" | awk -v name="$REQUIRED_PERF_TEST" '
      /^ *Test +#[0-9]+:/ && $3 == name { n++ }
      END { print n+0 }')"
    if [ "${expected:-0}" -ne 1 ]; then
      echo "==> $GATE: required perf test $REQUIRED_PERF_TEST is not registered and selected with the perf label"
      return 1
    fi
  fi
  [ "${count:-0}" -gt 0 ] || return 0
  echo "==> $GATE: running $count perf test(s) serially after $SUITE ($(stamp))"
  log="$STATE_DIR/ctest-perf.log"
  if ! run_logged "$log" -L '^perf$' "${SELECT[@]}"; then
    cp "$log" "$HISTORY/$RUN_ID-perf-run.log"
    cat "$log"
    echo "==> $GATE: PERF TESTS FAILED ($(stamp)); no retry after a serial run"
    return 1
  fi
  check_forbidden "$log"
  if [ -n "$REQUIRED_PERF_TEST" ] &&
     [ "$(rerun_status "$log" "$REQUIRED_PERF_TEST")" != Passed ]; then
    cat "$log"
    echo "==> $GATE: PERF TESTS FAILED: required test $REQUIRED_PERF_TEST did not pass"
    return 1
  fi
  echo "==> $GATE: perf tests passed serially ($(stamp))"
}

[ "$PROGRESS" != 1 ] || echo "==> $GATE: running $SUITE, $JOBS at a time ($(stamp))"
if run_logged "$STATE_DIR/ctest.log" --parallel "$JOBS" -LE '^perf$' "${SELECT[@]}"; then
  check_forbidden "$STATE_DIR/ctest.log"
  run_perf
  echo "==> $GATE: $SUITE passed, $(grep -E "tests passed" "$STATE_DIR/ctest.log" | sed "s/.*out of //") run ($(stamp))"
  exit 0
fi

first="$(grep -E "tests passed" "$STATE_DIR/ctest.log" || true)"
failed="$(failed_in "$STATE_DIR/ctest.log")"
kept="$HISTORY/$RUN_ID-first-run.log"
cp "$STATE_DIR/ctest.log" "$kept"
prune_history
echo "==> $GATE: ${first:-the run failed}; failed in $FIRST_RUN: ${failed:-(none named)}"
if [ -z "$failed" ]; then
  tail -n 60 "$STATE_DIR/ctest.log"
  echo "==> $GATE: $SUITE_FAILED ($(stamp)); no failed test named, full log: $kept$WHERE"
  exit 1
fi
# shellcheck disable=SC2086 # $failed is a list of test names
print_failures "$STATE_DIR/ctest.log" $failed
check_forbidden "$STATE_DIR/ctest.log"

if [ "$SANITIZER_REPORTS" = block ]; then
  reported=""
  for test in $failed; do
    if has_report "$STATE_DIR/ctest.log" "$test"; then reported="$reported $test"; fi
  done
  if [ -n "$reported" ]; then
    echo "==> $GATE: sanitizer report in:$reported (output above); a report is not rerun"
    echo "==> $GATE: $SUITE_FAILED ($(stamp)); full log: $kept$WHERE"
    exit 1
  fi
fi

# shellcheck disable=SC2086
n_failed="$(printf "%s\n" $failed | grep -c .)"
if [ "$n_failed" -gt "$RERUN_MAX" ]; then
  echo "==> $GATE: $n_failed tests failed, more than RERUN_MAX ($RERUN_MAX): a break, not a flake; not rerun"
  echo "==> $GATE: $SUITE_FAILED ($(stamp)); the first run's full log: $kept$WHERE"
  exit 1
fi

# By exact name: --rerun-failed goes by test number, and CTest numbers the
# tests within the -E selection, so it would rerun different tests.
pattern=""
for test in $failed; do
  pattern="${pattern:+$pattern|}$(exact_re "$test")"
done
if ! run_logged "$STATE_DIR/ctest-rerun.log" -R "^($pattern)\$"; then
  echo "==> $GATE: the rerun alone failed too:"
  sed -n "/The following tests FAILED/,/^Errors while running/p" "$STATE_DIR/ctest-rerun.log"
  # shellcheck disable=SC2086
  print_failures "$STATE_DIR/ctest-rerun.log" $(failed_in "$STATE_DIR/ctest-rerun.log")
  echo "==> $GATE: $SUITE_FAILED ($(stamp)); the first run's full log: $kept$WHERE"
  exit 1
fi
check_forbidden "$STATE_DIR/ctest-rerun.log"
# Each failed test must have passed: ctest exits 0 for a skipped test too.
not_passed=""
for test in $failed; do
  status="$(rerun_status "$STATE_DIR/ctest-rerun.log" "$test")"
  [ "$status" = Passed ] || not_passed="$not_passed $test ($status)"
done
if [ -n "$not_passed" ]; then
  echo "==> $GATE: the rerun did not pass:$not_passed"
  echo "==> $GATE: $SUITE_FAILED ($(stamp)); the first run's full log: $kept$WHERE"
  exit 1
fi

for test in $failed; do
  printf '%s\t%s\n' "$RUN_ID" "$test" >> "$HISTORY/reruns"
done
gates="$(recent_gates | wc -l | tr -d " ")"
echo "!! $GATE: RERUN: failed in $FIRST_RUN, passed alone: $failed"
echo "!!   first failure's output above; its full log: $kept$WHERE"
for test in $failed; do
  # The recent gates in which this test needed a rerun.
  count="$(awk -F '\t' -v t="$test" 'NR == FNR { recent[$1] = 1; next }
                                      $2 == t && ($1 in recent) { n++ }
                                      END { print n + 0 }' <(recent_gates) "$HISTORY/reruns")"
  echo "!!   $test needed a rerun in $count of the last $gates gate(s)"
done
run_perf
echo "==> $GATE: $SUITE passed after a rerun, $(printf "%s" "$first" | sed "s/.*out of //") run ($(stamp))"
exit 0
}
