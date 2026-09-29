#!/usr/bin/env bash
#
# linux-gate-smoke.sh — the Linux gate's smoke CTest run, inside its container
# (scripts/linux-gate.sh mounts this file next to the build; nostrc-16yi).
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
#
# Environment: JOBS, SMOKE_EXCLUDE (a ctest -E regex); optional BUILD_DIR
# (/work/build), STATE_DIR (/work), VOLUME (named in messages), GATE_SECONDS
# (elapsed seconds of the gate so far), HISTORY_KEEP (20), TAIL_LINES (200).
set -euo pipefail
{

BUILD_DIR="${BUILD_DIR:-/work/build}"
STATE_DIR="${STATE_DIR:-/work}"
HISTORY_KEEP="${HISTORY_KEEP:-20}"
TAIL_LINES="${TAIL_LINES:-200}"
SECONDS="${GATE_SECONDS:-0}"
HISTORY="$STATE_DIR/gate-history"
mkdir -p "$HISTORY"
touch "$HISTORY/gates" "$HISTORY/reruns"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
printf '%s\n' "$RUN_ID" >> "$HISTORY/gates"

stamp() { printf "%dm%02ds" $((SECONDS / 60)) $((SECONDS % 60)); }

# Widget tests need a display and a session bus, as in groundhog-ci.yml.
smoke() {
  dbus-run-session -- xvfb-run -a -s "-screen 0 1280x800x24" \
    ctest --test-dir "$BUILD_DIR" --timeout 120 --no-tests=error --output-on-failure "$@"
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
    echo "---- $test: output of its failed run ($lines lines; the last $TAIL_LINES follow) ----"
    output_of "$log" "$test" | tail -n "$TAIL_LINES"
    echo "---- end of $test ----"
  done
}

# The last HISTORY_KEEP gates, oldest first.
recent_gates() {
  tail -n "$HISTORY_KEEP" "$HISTORY/gates"
}

if smoke --parallel "$JOBS" -E "$SMOKE_EXCLUDE" > "$STATE_DIR/ctest.log" 2>&1; then
  echo "==> Linux gate: smoke tests passed, $(grep -E "tests passed" "$STATE_DIR/ctest.log" | sed "s/.*out of //") run ($(stamp))"
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
echo "==> Linux gate: ${first:-the smoke run failed}; failed in the parallel run: ${failed:-(none named)}"
if [ -z "$failed" ]; then
  tail -n 60 "$STATE_DIR/ctest.log"
  echo "==> Linux gate: SMOKE TESTS FAILED ($(stamp)); no failed test named, full log: $kept in volume ${VOLUME:-?}"
  exit 1
fi
# shellcheck disable=SC2086 # $failed is a list of test names
print_failures "$STATE_DIR/ctest.log" $failed

# By name: --rerun-failed goes by test number, and CTest numbers the tests
# within the -E selection, so it would rerun different tests.
if ! smoke -R "^($(printf "%s" "$failed" | tr " " "|"))\$" > "$STATE_DIR/ctest-rerun.log" 2>&1; then
  echo "==> Linux gate: the rerun alone failed too:"
  sed -n "/The following tests FAILED/,/^Errors while running/p" "$STATE_DIR/ctest-rerun.log"
  # shellcheck disable=SC2086
  print_failures "$STATE_DIR/ctest-rerun.log" $(failed_in "$STATE_DIR/ctest-rerun.log")
  echo "==> Linux gate: SMOKE TESTS FAILED ($(stamp)); logs in volume ${VOLUME:-?} under $STATE_DIR"
  exit 1
fi
rerun="$(grep -cE "^ *[0-9]+/[0-9]+ Test +#" "$STATE_DIR/ctest-rerun.log" || true)"
# shellcheck disable=SC2086
if [ "$rerun" -lt "$(printf "%s\n" $failed | grep -c .)" ]; then
  echo "==> Linux gate: SMOKE TESTS FAILED: the rerun ran $rerun test(s) for: $failed"
  exit 1
fi

for test in $failed; do
  printf '%s\t%s\n' "$RUN_ID" "$test" >> "$HISTORY/reruns"
done
gates="$(recent_gates | wc -l | tr -d " ")"
echo "!! Linux gate: RERUN: failed in the parallel run, passed alone: $failed"
echo "!!   first failure's output above; its full log: $kept in volume ${VOLUME:-?}"
for test in $failed; do
  # The recent gates in which this test needed a rerun.
  count="$(awk -F '\t' -v t="$test" 'NR == FNR { recent[$1] = 1; next }
                                      $2 == t && ($1 in recent) { n++ }
                                      END { print n + 0 }' <(recent_gates) "$HISTORY/reruns")"
  echo "!!   $test needed a rerun in $count of the last $gates gate(s)"
done
echo "==> Linux gate: smoke tests passed after a rerun, $(printf "%s" "$first" | sed "s/.*out of //") run ($(stamp))"
exit 0
}
