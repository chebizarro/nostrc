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
        # A fixture of this test's run, e.g. an LSan report (nostrc-vpha rule).
        if [ -f "${FIX:-/nonexistent}/$t.$run" ]; then cat "$FIX/$t.$run"; fi
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
        BUILD_DIR="$tmp/build" STATE_DIR="$tmp/state" VOLUME=test-volume HISTORY_KEEP=3 FIX="$tmp/fix" \
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


# ---- the library-leak rerun rule (nostrc-vpha; remove with it) ----
# Fixtures in the real LSan format (frames from the gate's container).
mkdir -p "$tmp/fix" "$tmp/cls"
lsan_header() { printf '==7==ERROR: LeakSanitizer: detected memory leaks\n\n'; }
lsan_summary() { printf 'SUMMARY: AddressSanitizer: 512 byte(s) leaked in 2 allocation(s).\n'; }
# The nostrc-vpha stack: libnostr's connection channels, on a GIO worker.
# $1 replaces frame #5 (default: nostr-gobject's connect thread).
lib_record() {
    cat <<REC
Direct leak of 448 byte(s) in 1 object(s) allocated from:
    #0 0xffffa42e76d0 in malloc ../../../../src/libsanitizer/asan/asan_malloc_linux.cpp:69
    #1 0xaaaadf6fb1d8 in go_channel_create /work/src/libgo/src/channel.c:978
    #2 0xaaaadf6cb804 in nostr_connection_new /work/src/libnostr/src/connection.c:1070
    #3 0xaaaadf6cb900 in nostr_relay_connect /work/src/libnostr/src/relay.c:821
    #4 0xaaaadf6656dc in gnostr_relay_connect /work/src/nostr-gobject/src/nostr_relay.c:990
    ${1:-#5 0xaaaadf6657aa in nostr_relay_connect_async_thread /work/src/nostr-gobject/src/nostr_async.c:45}
    #6 0xffff88ec2810  (/lib/aarch64-linux-gnu/libgio-2.0.so.0+0xc2810) (BuildId: 9ee169d35bbdc7d1)
    #7 0xffff8acf59ac  (/lib/aarch64-linux-gnu/libglib-2.0.so.0+0x959ac) (BuildId: c1a3727b895b1c60)
    #8 0xffff8b25f3d0 in asan_thread_start ../../../../src/libsanitizer/asan/asan_interceptors.cpp:234
    #9 0xffffa2685828  (/lib/aarch64-linux-gnu/libc.so.6+0x85828) (BuildId: 27027b96e5b8c475)

REC
}
lib_report() { lsan_header; lib_record; lib_record; lsan_summary; }
gh_report() {
    lsan_header; lib_record
    lib_record "#5 0xaaaac503bf98 in on_connected /work/src/gnome/groundhog/src/relay/gh-relay-gnostr.c:241"
    lsan_summary
}
# OpenSSL per-thread state through Groundhog's store (seen in the gate):
# its stack runs through gh-store.c, so it implicates the push.
openssl_report() {
    lsan_header
    cat <<'REC'
Direct leak of 2712 byte(s) in 3 object(s) allocated from:
    #0 0xffff8b2e76d0 in malloc ../../../../src/libsanitizer/asan/asan_malloc_linux.cpp:69
    #1 0xffff8940c884 in CRYPTO_zalloc (/lib/aarch64-linux-gnu/libcrypto.so.3+0x20c884) (BuildId: fc8e675a4a75f005)
    #2 0xffff893bd2b4  (/lib/aarch64-linux-gnu/libcrypto.so.3+0x1bd2b4) (BuildId: fc8e675a4a75f005)
    #3 0xffff893be1e0 in ERR_set_mark (/lib/aarch64-linux-gnu/libcrypto.so.3+0x1be1e0) (BuildId: fc8e675a4a75f005)
    #9 0xffff893efa98 in PKCS5_PBKDF2_HMAC (/lib/aarch64-linux-gnu/libcrypto.so.3+0x1efa98) (BuildId: fc8e675a4a75f005)
    #12 0xffff8b0f87f0 in sqlcipher_codec_key_derive (/lib/aarch64-linux-gnu/libsqlcipher.so.1+0x587f0) (BuildId: 2d9e9a276aa63daf)
    #19 0xffff8b0e36f0 in sqlite3_finalize (/lib/aarch64-linux-gnu/libsqlcipher.so.1+0x436f0) (BuildId: 2d9e9a276aa63daf)
    #20 0xaaaac5147cf8 in store_query_text /work/src/gnome/groundhog/src/store/gh-store.c:925
    #25 0xaaaac503bf98 in open_worker /work/src/gnome/groundhog/src/app/gh-account-store.c:358
    #26 0xffff88ec2810  (/lib/aarch64-linux-gnu/libgio-2.0.so.0+0xc2810) (BuildId: 9ee169d35bbdc7d1)
    #29 0xffff8b25f3d0 in asan_thread_start ../../../../src/libsanitizer/asan/asan_interceptors.cpp:234

REC
    lsan_summary
}
classifies() {  # classifies NAME EXPECTED: stdin through --classify
    local got
    got="$(bash "$scripts/linux-gate-smoke.sh" --classify | tee "$tmp/cls/$1")"
    [ "$(printf '%s\n' "$got" | head -1)" = "$2" ] ||
        fail "classifier, $1: $(printf '%s' "$got" | tr '\n' ' '), expected $2"
}

# The frame classifier.
lib_report | classifies library-only library
grep -q '^  library record: go_channel_create libgo/src/channel.c <- nostr_connection_new libnostr/src/connection.c <- nostr_relay_connect libnostr/src/relay.c$' \
    "$tmp/cls/library-only" || fail "library record summary: $(cat "$tmp/cls/library-only")"
gh_report | classifies groundhog-frame implicating
grep -q 'not a library frame: on_connected gnome/groundhog/src/relay/gh-relay-gnostr.c' "$tmp/cls/groundhog-frame" ||
    fail "the implicating frame is not named"
{ lsan_header; lib_record "#5 0xffffdeadbeef  (<unknown module>)"; lsan_summary; } |
    classifies unknown-module implicating
{ lsan_header; lib_record "#5 0xaaaadf6657aa in some_function"; lsan_summary; } |
    classifies unrecognised-frame implicating
{ lsan_header; lib_record "Thread T3 created by T0 here:"; lsan_summary; } |
    classifies non-frame-line implicating
openssl_report | classifies openssl-via-store implicating
{ lsan_header; lib_record "#5 0xaaaadf6657aa in marmot_group_add /work/src/libmarmot/src/group.c:77"; lsan_summary; } |
    classifies libmarmot-frame implicating
{ lsan_header; lib_record "#5 0xaaaadf48182c in _start (/work/build/gnome/groundhog/test-groundhog-relay-wire+0x3b182c)"; lsan_summary; } |
    classifies project-module implicating
{ lsan_header; lsan_summary; } | classifies header-no-record hard
{ lsan_header; lib_record | sed '$d'; } | classifies truncated-record hard
{ printf '==9==ERROR: AddressSanitizer: heap-use-after-free on address 0x6020 at pc 0xaaaa\n'; lib_report; } |
    classifies uaf-and-leak hard
{ lib_report; printf 'gh-store.c:12:3: runtime error: signed integer overflow\n'; } | classifies ubsan hard
{ printf '==7==ERROR: LeakSanitizer: tracer caught signal 11\n'; lib_record; } | classifies lsan-internal-error hard
{ lib_report; printf 'SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior x.c:1:2\n'; } |
    classifies other-summary hard
{ lib_record; lsan_summary; } | classifies records-without-header hard
printf 'not ok 3 /groundhog/relay/x - timed out\n' | classifies no-report none
# Another source root (hosted CI's) is stripped too.
lib_report | sed 's|/work/src/|/home/runner/work/nostrc/nostrc/|' |
    SOURCE_ROOT=/home/runner/work/nostrc/nostrc/ classifies other-root library

# The rerun policy, in the sanitizer mode.
fixture() { rm -f "$tmp/fix/"*; }
: > "$tmp/state/gate-history/gates"
: > "$tmp/state/gate-history/reruns"

# A library-only leak is rerun once, serially, and passes, loudly and counted.
fixture; lib_report > "$tmp/fix/beta.first"
FIRST_FAIL="beta" RERUN_FAIL="" sanitizers > "$tmp/out" 2>&1 || fail "a library-only leak blocked: $(tail -5 "$tmp/out")"
[ "$(grep -c '^CTEST' "$tmp/trace")" -eq 2 ] || fail "a library-only leak was not rerun exactly once"
grep -qF -- '-R ^(beta)$' "$tmp/trace" || fail "the library leak was not rerun by name"
[ "$(grep '^CTEST' "$tmp/trace" | tail -1 | grep -c -- '--parallel')" -eq 0 ] || fail "the rerun was not serial"
grep -q '^!! LIBRARY LEAK RERUN (nostrc-vpha): beta$' "$tmp/out" || fail "the library-leak rerun was not announced"
grep -q '^!!     library record: go_channel_create libgo' "$tmp/out" || fail "the records were not printed"
grep -q 'early line of beta' "$tmp/out" || fail "the first run's whole output was not printed"
grep -q 'beta needed a library-leak rerun in 1 of the last 1 gate(s)' "$tmp/out" || fail "the library-leak rerun was not counted"
awk -F '\t' '$2 == "beta" && $3 == "lib-leak" { found = 1 } END { exit !found }' \
    "$tmp/state/gate-history/reruns" || fail "no lib-leak tag in gate-history"
grep -q 'sanitizer tests passed after a rerun' "$tmp/out" || fail "no pass line after the library-leak rerun"
FIRST_FAIL="beta" RERUN_FAIL="" sanitizers > "$tmp/out" 2>&1 || fail "second library-leak gate blocked"
grep -q 'beta needed a library-leak rerun in 2 of the last 2 gate(s)' "$tmp/out" || fail "library-leak reruns not counted across gates"

# The same leak again in the rerun blocks.
fixture; lib_report > "$tmp/fix/beta.first"; lib_report > "$tmp/fix/beta.rerun"
if FIRST_FAIL="beta" RERUN_FAIL="beta" sanitizers > "$tmp/out" 2>&1; then fail "a library leak repeated in the rerun passed"; fi
grep -q 'the rerun alone failed too' "$tmp/out" || fail "no rerun failure line"
absent_line() { if grep -q "$1" "$2"; then fail "$3"; fi; }
absent_line 'LIBRARY LEAK RERUN (nostrc-vpha)' "$tmp/out" "a failed library-leak rerun was announced as passed"

# A record with a Groundhog frame blocks at once.
fixture; gh_report > "$tmp/fix/beta.first"
if FIRST_FAIL="beta" RERUN_FAIL="" sanitizers > "$tmp/out" 2>&1; then fail "a Groundhog-frame leak passed"; fi
[ "$(grep -c '^CTEST' "$tmp/trace")" -eq 1 ] || fail "a Groundhog-frame leak was rerun"
grep -q 'sanitizer report in: beta' "$tmp/out" || fail "the implicating test is not named"

# A library-only leak beside another test's Groundhog leak: nothing is rerun.
fixture; lib_report > "$tmp/fix/alpha.first"; gh_report > "$tmp/fix/beta.first"
if FIRST_FAIL="alpha beta" RERUN_FAIL="" sanitizers > "$tmp/out" 2>&1; then fail "a library leak beside a Groundhog leak passed"; fi
[ "$(grep -c '^CTEST' "$tmp/trace")" -eq 1 ] || fail "rerun beside a Groundhog leak"

# Library-only leaks plus a use-after-free, the OpenSSL-via-store leak, a
# libmarmot frame, an unsymbolized project module, a header with no record:
# each blocks without a rerun.
for case in uaf openssl marmot module header; do
    fixture
    case "$case" in
        uaf) { printf '==9==ERROR: AddressSanitizer: heap-use-after-free on address 0x6020\n'; lib_report; } ;;
        openssl) openssl_report ;;
        marmot) { lsan_header; lib_record "#5 0xaaaa in marmot_group_add /work/src/libmarmot/src/group.c:77"; lsan_summary; } ;;
        module) { lsan_header; lib_record "#5 0xaaaa in _start (/work/build/gnome/groundhog/test-groundhog-relay-wire+0x3b182c)"; lsan_summary; } ;;
        header) { lsan_header; lsan_summary; } ;;
    esac > "$tmp/fix/beta.first"
    if FIRST_FAIL="beta" RERUN_FAIL="" sanitizers > "$tmp/out" 2>&1; then fail "$case: passed"; fi
    [ "$(grep -c '^CTEST' "$tmp/trace")" -eq 1 ] || fail "$case: rerun"
    absent_line 'LIBRARY LEAK RERUN' "$tmp/out" "$case: announced a library-leak rerun"
done

# Three library-leak tests in one run: a regression, not a race.
fixture; for t in alpha beta gamma; do lib_report > "$tmp/fix/$t.first"; done
if FIRST_FAIL="alpha beta gamma" RERUN_FAIL="" sanitizers > "$tmp/out" 2>&1; then fail "three library-leak tests passed"; fi
[ "$(grep -c '^CTEST' "$tmp/trace")" -eq 1 ] || fail "three library-leak tests were rerun"
grep -q '3 tests leak in libraries only: alpha beta gamma; more than 2' "$tmp/out" || fail "no regression message"

# Two are rerun, and with them a failure that has no report at all.
fixture; for t in alpha beta; do lib_report > "$tmp/fix/$t.first"; done
FIRST_FAIL="alpha beta gamma" RERUN_FAIL="" sanitizers > "$tmp/out" 2>&1 || fail "two library leaks and a flake blocked"
grep -qF -- '-R ^(alpha|beta|gamma)$' "$tmp/trace" || fail "not all three were rerun"
grep -q '^!! LIBRARY LEAK RERUN (nostrc-vpha): alpha$' "$tmp/out" && grep -q '^!! LIBRARY LEAK RERUN (nostrc-vpha): beta$' "$tmp/out" ||
    fail "both library-leak reruns must be announced"
absent_line 'LIBRARY LEAK RERUN (nostrc-vpha): gamma' "$tmp/out" "a report-free flake was announced as a library leak"

# The smoke run (no SANITIZER_REPORTS) never classifies: a report there is
# just a failure, rerun as before.
fixture; gh_report > "$tmp/fix/beta.first"
FIRST_FAIL="beta" RERUN_FAIL="" gate > "$tmp/out" 2>&1 || fail "the smoke run applied the sanitizer rule"
echo 'ok: the library-leak rerun rule (nostrc-vpha)'

echo 'linux gate smoke tests passed'
