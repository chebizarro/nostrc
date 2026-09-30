#!/usr/bin/env bash
#
# linux-gate-smoke.sh — the Linux gate's CTest run, inside its container
# (scripts/linux-gate.sh mounts this file next to the build; nostrc-16yi): the
# smoke subset, or with --sanitizers the Groundhog sanitizer set (nostrc-3han).
#
# The tests run in parallel beside the macOS build. A test that fails is run
# once more on its own: that absorbs a race lost under that load, while a real
# break fails both times. A rerun is never silent or forgotten:
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
# (/work), VOLUME (named in messages), GATE_SECONDS (elapsed seconds of the gate
# so far), HISTORY_KEEP (20), TAIL_LINES (200, or "all"), GATE ("Linux gate")
# and SUITE ("smoke tests") for messages, DISPLAY_WRAP (1: under
# dbus-run-session and xvfb-run; 0: bare), CTEST_TIMEOUT (120; empty: CTest's
# default), FORBID_PATTERN (an ERE no ctest log may match, e.g. skipped tests),
# SANITIZER_REPORTS (block, or empty).
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
if [ -n "${TEST_REGEX:-}" ]; then
  SELECT=(-R "$TEST_REGEX")
else
  SELECT=(-E "$SMOKE_EXCLUDE")
fi
SECONDS="${GATE_SECONDS:-0}"
HISTORY="$STATE_DIR/gate-history"
mkdir -p "$HISTORY"
touch "$HISTORY/gates" "$HISTORY/reruns"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
printf '%s\n' "$RUN_ID" >> "$HISTORY/gates"

stamp() { printf "%dm%02ds" $((SECONDS / 60)) $((SECONDS % 60)); }

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

if smoke --parallel "$JOBS" "${SELECT[@]}" > "$STATE_DIR/ctest.log" 2>&1; then
  check_forbidden "$STATE_DIR/ctest.log"
  echo "==> $GATE: $SUITE passed, $(grep -E "tests passed" "$STATE_DIR/ctest.log" | sed "s/.*out of //") run ($(stamp))"
  exit 0
fi

first="$(grep -E "tests passed" "$STATE_DIR/ctest.log" || true)"
failed="$(failed_in "$STATE_DIR/ctest.log")"
kept="$HISTORY/$RUN_ID-first-run.log"
cp "$STATE_DIR/ctest.log" "$kept"
# Keep the newest HISTORY_KEEP first-run logs (names sort by time).
find "$HISTORY" -name "*-first-run.log" | sort |
  awk -v keep="$HISTORY_KEEP" '{ name[NR] = $0 } END { for (i = 1; i <= NR - keep; i++) print name[i] }' |
  while read -r old; do rm -f "$old"; done
echo "==> $GATE: ${first:-the run failed}; failed in the parallel run: ${failed:-(none named)}"
if [ -z "$failed" ]; then
  tail -n 60 "$STATE_DIR/ctest.log"
  echo "==> $GATE: $SUITE_FAILED ($(stamp)); no failed test named, full log: $kept in volume ${VOLUME:-?}"
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
    echo "==> $GATE: $SUITE_FAILED ($(stamp)); full log: $kept in volume ${VOLUME:-?}"
    exit 1
  fi
fi

# By name: --rerun-failed goes by test number, and CTest numbers the tests
# within the -E selection, so it would rerun different tests.
if ! smoke -R "^($(printf "%s" "$failed" | tr " " "|"))\$" > "$STATE_DIR/ctest-rerun.log" 2>&1; then
  echo "==> $GATE: the rerun alone failed too:"
  sed -n "/The following tests FAILED/,/^Errors while running/p" "$STATE_DIR/ctest-rerun.log"
  # shellcheck disable=SC2086
  print_failures "$STATE_DIR/ctest-rerun.log" $(failed_in "$STATE_DIR/ctest-rerun.log")
  echo "==> $GATE: $SUITE_FAILED ($(stamp)); logs in volume ${VOLUME:-?} under $STATE_DIR"
  exit 1
fi
check_forbidden "$STATE_DIR/ctest-rerun.log"
rerun="$(grep -cE "^ *[0-9]+/[0-9]+ Test +#" "$STATE_DIR/ctest-rerun.log" || true)"
# shellcheck disable=SC2086
if [ "$rerun" -lt "$(printf "%s\n" $failed | grep -c .)" ]; then
  echo "==> $GATE: $SUITE_FAILED: the rerun ran $rerun test(s) for: $failed"
  exit 1
fi

for test in $failed; do
  printf '%s\t%s\n' "$RUN_ID" "$test" >> "$HISTORY/reruns"
done
gates="$(recent_gates | wc -l | tr -d " ")"
echo "!! $GATE: RERUN: failed in the parallel run, passed alone: $failed"
echo "!!   first failure's output above; its full log: $kept in volume ${VOLUME:-?}"
for test in $failed; do
  # The recent gates in which this test needed a rerun.
  count="$(awk -F '\t' -v t="$test" 'NR == FNR { recent[$1] = 1; next }
                                      $2 == t && ($1 in recent) { n++ }
                                      END { print n + 0 }' <(recent_gates) "$HISTORY/reruns")"
  echo "!!   $test needed a rerun in $count of the last $gates gate(s)"
done
echo "==> $GATE: $SUITE passed after a rerun, $(printf "%s" "$first" | sed "s/.*out of //") run ($(stamp))"
exit 0
}
