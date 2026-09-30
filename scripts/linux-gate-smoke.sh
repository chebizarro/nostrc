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
# deterministic enough that a passing rerun would only hide one. One temporary
# exception, the library-leak rerun rule (nostrc-vpha; gate only, hosted CI
# never reruns): a test whose only report is LSan leak records every frame of
# which is in LIBRARY_LEAK_ALLOW, the sanitizer runtime or a system library is
# rerun once, serially, if no other test implicates the push and at most
# LIBRARY_LEAK_MAX tests leak that way; any failure of the rerun blocks. Such a
# rerun is announced as "!! LIBRARY LEAK RERUN (nostrc-vpha)" and counted in
# gate-history (tag lib-leak). REMOVE the rule (LIBRARY_LEAK_ALLOW and its
# uses) when nostrc-vpha closes: it only absorbs libnostr's exit races.
#
# Environment: JOBS, and SMOKE_EXCLUDE (a ctest -E regex) or TEST_REGEX (a
# ctest -R regex, which wins). Optional: BUILD_DIR (/work/build), STATE_DIR
# (/work), VOLUME (named in messages), GATE_SECONDS (elapsed seconds of the gate
# so far), HISTORY_KEEP (20), TAIL_LINES (200, or "all"), GATE ("Linux gate")
# and SUITE ("smoke tests") for messages, DISPLAY_WRAP (1: under
# dbus-run-session and xvfb-run; 0: bare), CTEST_TIMEOUT (120; empty: CTest's
# default), FORBID_PATTERN (an ERE no ctest log may match, e.g. skipped tests),
# SANITIZER_REPORTS (block, or empty), SOURCE_ROOT (/work/src/: stripped from
# frame paths).
#
# `linux-gate-smoke.sh --classify < OUTPUT` prints how the rule classifies one
# failed test's output (scripts/test-linux-gate-smoke.sh tests it).
set -euo pipefail
{

# nostrc-vpha: the library-leak rerun rule's allow-list. A leak frame is a
# library frame only if its source path (after SOURCE_ROOT) starts with one of
# these, its path is the sanitizer runtime's (/libsanitizer/), or it has no
# source path and its module is under /lib/ or /usr/lib/. Remove with the rule
# when nostrc-vpha closes (AGENTS.md, scripts/README.md).
LIBRARY_LEAK_ALLOW='libnostr/ libgo/ nostr-gobject/ libjson/'
LIBRARY_LEAK_MAX=2
SOURCE_ROOT="${SOURCE_ROOT:-/work/src/}"

# Classifies one failed test's output (stdin): the first line is
#   hard         a report that is not only LSan leaks, or output the parser
#                does not fully understand (fails closed);
#   implicating  LSan leaks only, and a frame of some record is not a
#                library frame;
#   library      LSan leaks only, every frame of every record a library frame;
#   none         no sanitizer report;
# then one line per reason or record (a record: its first three frames above
# the allocator).
CLASSIFY_PY='
import re, sys
allow, root = sys.argv[1].split(), sys.argv[2]
FRAME = re.compile(r"^\s+#\d+ 0x[0-9a-f]+ +(?:in (\S+) (\S+)|\((\S+)\+0x[0-9a-f]+\))")
MODULE = re.compile(r"^\((\S+)\+0x[0-9a-f]+\)$")
LEAK_SUMMARY = re.compile(r"^SUMMARY: AddressSanitizer: \d+ byte\(s\) leaked in \d+ allocation\(s\)\.$")
hard, records, lsan, cur = [], [], False, None

def module_kind(module):
    return "library" if module.startswith(("/lib/", "/usr/lib/")) else "implicates"

def frame(line):
    m = FRAME.match(line)
    if not m:
        return None
    func, loc, bare = m.group(1), m.group(2), m.group(3)
    if bare is not None:
        return ("?", bare, module_kind(bare))
    mm = MODULE.match(loc)
    if mm:
        return (func, mm.group(1), module_kind(mm.group(1)))
    path = re.sub(r"(:\d+)+$", "", loc)
    if "/libsanitizer/" in path:
        return (func, path, "library")
    rel = path[len(root):] if path.startswith(root) else path
    return (func, rel, "library" if rel.startswith(tuple(allow)) else "implicates")

def close():
    global cur
    if cur is not None:
        if not cur:
            hard.append("a leak record without frames")
        else:
            records.append(cur)
    cur = None

for raw in sys.stdin.read().splitlines():  # no empty last line: a cut-off record stays open
    line = raw.rstrip("\r")
    if cur is not None:
        if not line.strip():
            close()
            continue
        f = frame(line)
        if f is None:  # not a recognised frame: it implicates the push
            f = ("?", "unrecognised line: " + line.strip()[:100], "implicates")
        cur.append(f)
        continue
    if re.match(r"^(Direct|Indirect) leak of ", line):
        cur = []
        continue
    m = re.search(r"ERROR: (\w+Sanitizer): ?(.*)$", line)
    if m:
        if m.group(1) == "LeakSanitizer" and m.group(2).strip() == "detected memory leaks":
            lsan = True
        else:
            hard.append(line.strip()[:160])
        continue
    if ": runtime error: " in line:
        hard.append(line.strip()[:160])
        continue
    m = re.search(r"SUMMARY: \w+Sanitizer", line)
    if m and not LEAK_SUMMARY.match(line[m.start():].strip()):
        hard.append(line.strip()[:160])
if cur is not None:
    hard.append("a leak record cut off at the end of the output")
    close()
if lsan and not records:
    hard.append("a LeakSanitizer report with no leak record")
if records and not lsan:
    hard.append("leak records without a LeakSanitizer report")

def show(rec):
    above = [f for f in rec if "/libsanitizer/" not in f[1]][:3]
    return " <- ".join("%s %s" % (f[0], f[1]) for f in above)

if hard:
    print("hard")
    for h in hard:
        print("  hard: " + h)
elif not records:
    print("none")
else:
    implicating = [r for r in records if any(f[2] != "library" for f in r)]
    print("implicating" if implicating else "library")
    for r in records:
        bad = [f for f in r if f[2] != "library"]
        if bad:
            print("  implicating record: %s (not a library frame: %s %s)" % (show(r), bad[0][0], bad[0][1]))
        else:
            print("  library record: " + show(r))
'
classify() {
  python3 -c "$CLASSIFY_PY" "$LIBRARY_LEAK_ALLOW" "$SOURCE_ROOT"
}
if [ "${1:-}" = --classify ]; then
  classify
  exit
fi

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

# How the library-leak rule classifies test $2's output in log $1 (all lines).
classify_in() {
  output_of "$1" "$2" | classify
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

# A sanitizer report blocks without a rerun, except under the library-leak
# rule (nostrc-vpha, see the header): tests whose only report is library-only
# leaks, at most LIBRARY_LEAK_MAX of them, and no other test's report.
lib_leak=""
if [ "$SANITIZER_REPORTS" = block ]; then
  verdicts="$(mktemp -d)"
  reported=""
  for test in $failed; do
    classify_in "$STATE_DIR/ctest.log" "$test" > "$verdicts/$test"
    case "$(head -1 "$verdicts/$test")" in
      hard|implicating) reported="$reported $test" ;;
      library) lib_leak="$lib_leak $test" ;;
    esac
  done
  if [ -n "$reported" ]; then
    for test in $reported $lib_leak; do
      echo "==> $GATE: $test: $(head -1 "$verdicts/$test") sanitizer report"
      tail -n +2 "$verdicts/$test"
    done
    echo "==> $GATE: sanitizer report in:$reported (output above); a report is not rerun"
    echo "==> $GATE: $SUITE_FAILED ($(stamp)); full log: $kept in volume ${VOLUME:-?}"
    exit 1
  fi
  lib_count="$(printf "%s\n" $lib_leak | grep -c . || true)"
  if [ "$lib_count" -gt "$LIBRARY_LEAK_MAX" ]; then
    for test in $lib_leak; do echo "==> $GATE: $test:"; tail -n +2 "$verdicts/$test"; done
    echo "==> $GATE: $lib_count tests leak in libraries only:$lib_leak; more than $LIBRARY_LEAK_MAX in one run is a regression, not a race (nostrc-vpha): not rerun"
    echo "==> $GATE: $SUITE_FAILED ($(stamp)); full log: $kept in volume ${VOLUME:-?}"
    exit 1
  fi
  if [ -n "$lib_leak" ]; then
    echo "!! $GATE: library-only leak in:$lib_leak; rerun once, serially (nostrc-vpha rule)"
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
  case " $lib_leak " in
    *" $test "*) printf '%s\t%s\tlib-leak\n' "$RUN_ID" "$test" >> "$HISTORY/reruns" ;;
    *) printf '%s\t%s\n' "$RUN_ID" "$test" >> "$HISTORY/reruns" ;;
  esac
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
# nostrc-vpha: a library-leak rerun is louder still, and counted apart.
for test in $lib_leak; do
  count="$(awk -F '\t' -v t="$test" 'NR == FNR { recent[$1] = 1; next }
                                      $2 == t && $3 == "lib-leak" && ($1 in recent) { n++ }
                                      END { print n + 0 }' <(recent_gates) "$HISTORY/reruns")"
  echo "!! LIBRARY LEAK RERUN (nostrc-vpha): $test"
  echo "!!   its first run leaked in libraries only (whole output above); the records:"
  tail -n +2 "$verdicts/$test" | sed "s/^/!!   /"
  echo "!!   $test needed a library-leak rerun in $count of the last $gates gate(s)"
done
echo "==> $GATE: $SUITE passed after a rerun, $(printf "%s" "$first" | sed "s/.*out of //") run ($(stamp))"
exit 0
}
