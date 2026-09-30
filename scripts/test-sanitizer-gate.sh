#!/usr/bin/env bash
# Tests scripts/sanitizer-gate-ci.py (nostrc-3han): the pre-push sanitizer
# stage reads groundhog-ci.yml's groundhog-sanitizers job as CI runs it, fails
# on a job it cannot reproduce, and runs only for pushes touching what the job
# tests.
set -euo pipefail
scripts="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$scripts/.." && pwd)"
helper="$scripts/sanitizer-gate-ci.py"
workflow="$root/.github/workflows/groundhog-ci.yml"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

# config FILE: the helper's bash assignments for FILE, evaluated here.
config() {
    python3 "$helper" config --workflow "$1" --source-root /work/src > "$tmp/config.sh"
    GH_SAN_TESTS=() GH_SAN_TARGETS=() GH_SAN_CONFIGURE=() GH_SAN_ENV=() GH_SAN_SKIP_PATTERN=""
    # shellcheck disable=SC1091
    . "$tmp/config.sh"
}
has() { local x="$1"; shift; for e in "$@"; do [ "$e" = "$x" ] && return 0; done; return 1; }
# The real workflow through a perl substitution (the same on macOS and Linux).
mutate() { perl -pe "$1" "$workflow"; }

# ---- the real job -----------------------------------------------------------
config "$workflow"
# The list is the job's, word for word: compare with the folded block itself.
listed="$(awk '/GROUNDHOG_SANITIZER_TESTS: >-/ { on = 1; next }
               on && /^ *[a-z_]+[a-z0-9_-]*( |$)/ && !/:/ { printf "%s ", $0; next }
               on { exit }' "$workflow" | tr -s ' \n' '\n\n' | sed '/^$/d')"
[ "$(printf '%s\n' "${GH_SAN_TESTS[@]}")" = "$listed" ] || fail "tests differ from the workflow's list"
[ "${#GH_SAN_TESTS[@]}" -ge 40 ] || fail "only ${#GH_SAN_TESTS[@]} tests read"
has groundhog-mls-service "${GH_SAN_TESTS[@]}" || fail "groundhog-mls-service (nostrc-kdxe) not read"
has test_connection_recv_drain "${GH_SAN_TESTS[@]}" || fail "the last (non-groundhog) test not read"
[ "${GH_SAN_TARGETS[0]}" = groundhog ] || fail "groundhog is not built first"
has test-groundhog-mls-service "${GH_SAN_TARGETS[@]}" || fail "groundhog-* not mapped to test-*"
has test_connection_recv_drain "${GH_SAN_TARGETS[@]}" || fail "other tests not kept as targets"
[ "${#GH_SAN_TARGETS[@]}" -eq $((${#GH_SAN_TESTS[@]} + 1)) ] || fail "target count"
[ "${GH_SAN_CONFIGURE[0]} ${GH_SAN_CONFIGURE[1]}" = "-G Ninja" ] || fail "configure starts ${GH_SAN_CONFIGURE[*]}"
for flag in -DGNOSTR_ENABLE_ASAN=ON -DGNOSTR_ENABLE_UBSAN=ON -DBUILD_GROUNDHOG=ON -DBUILD_APPS=OFF; do
    has "$flag" "${GH_SAN_CONFIGURE[@]}" || fail "configure lacks $flag"
done
has ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:verify_asan_link_order=0 "${GH_SAN_ENV[@]}" ||
    fail "ASAN_OPTIONS not read: ${GH_SAN_ENV[*]}"
has UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 "${GH_SAN_ENV[@]}" || fail "UBSAN_OPTIONS not read"
has G_SLICE=always-malloc "${GH_SAN_ENV[@]}" || fail "G_SLICE not read"
has LSAN_OPTIONS=suppressions=/work/src/gnome/groundhog/tests/lsan.supp:print_suppressions=1 \
    "${GH_SAN_ENV[@]}" || fail "LSAN_OPTIONS not read with \$PWD as the source root"
[ -f "$root/gnome/groundhog/tests/lsan.supp" ] || fail "the suppressions file moved"
[ "$GH_SAN_SKIP_PATTERN" = '\*\*\*Skipped|Not Run' ] || fail "skip pattern: $GH_SAN_SKIP_PATTERN"
[ "$GH_SAN_TEST_JOBS" = 2 ] || fail "test parallelism: $GH_SAN_TEST_JOBS"
printf 'ok: the real job (%d tests)\n' "${#GH_SAN_TESTS[@]}"

# Read at run time: a test added to the job is in the gate's set at once.
mutate 's/test_connection_recv_drain$/test_connection_recv_drain groundhog-new-probe/' \
    > "$tmp/added.yml"
config "$tmp/added.yml"
has groundhog-new-probe "${GH_SAN_TESTS[@]}" || fail "an added test was not read"
has test-groundhog-new-probe "${GH_SAN_TARGETS[@]}" || fail "an added test is not built"
mutate 's/-DGNOSTR_ENABLE_UBSAN=ON/-DGNOSTR_ENABLE_UBSAN=ON -DNEW_FLAG=1/' > "$tmp/flag.yml"
config "$tmp/flag.yml"
has -DNEW_FLAG=1 "${GH_SAN_CONFIGURE[@]}" || fail "an added configure flag was not read"
mutate 's/halt_on_error=1:verify_asan_link_order=0/halt_on_error=1:verify_asan_link_order=0:x=1/' \
    > "$tmp/opt.yml"
config "$tmp/opt.yml"
has ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:verify_asan_link_order=0:x=1 "${GH_SAN_ENV[@]}" ||
    fail "a changed ASAN option was not read"
mutate 's/^( *)(export LSAN_OPTIONS=)/$1export G_DEBUG=fatal-criticals\n$1$2/' > "$tmp/export.yml"
config "$tmp/export.yml"
has G_DEBUG=fatal-criticals "${GH_SAN_ENV[@]}" || fail "an added export was not read"
mutate 's/ctest --test-dir build-asan --parallel 2/ctest --test-dir build-asan --parallel 3/' > "$tmp/jobs.yml"
config "$tmp/jobs.yml"
[ "$GH_SAN_TEST_JOBS" = 3 ] || fail "a changed ctest --parallel was not read"
echo 'ok: the job is read at run time'

# ---- a job the gate cannot reproduce fails, never guesses -------------------
rejects() {  # rejects NAME PERL-EXPRESSION EXPECTED-MESSAGE
    mutate "$2" > "$tmp/$1.yml"
    cmp -s "$workflow" "$tmp/$1.yml" && fail "$1: the mutation changed nothing"
    if python3 "$helper" config --workflow "$tmp/$1.yml" > /dev/null 2> "$tmp/err"; then
        fail "$1: accepted"
    fi
    grep -q "$3" "$tmp/err" || fail "$1: wrong message: $(cat "$tmp/err")"
}
rejects renamed-job 's/^  groundhog-sanitizers:/  groundhog-asan:/' 'no job groundhog-sanitizers'
rejects no-tests 's/GROUNDHOG_SANITIZER_TESTS: >-/OTHER_TESTS: >-/' 'has no GROUNDHOG_SANITIZER_TESTS'
rejects configure-extra 's/^( *)(-DGNOSTR_ENABLE_ASAN=ON -DGNOSTR_ENABLE_UBSAN=ON)$/$1$2\n          cmake --build build-asan --target x/' 'runs 2 commands'
rejects configure-env 's/-DGNOSTR_ENABLE_UBSAN=ON$/-DGNOSTR_ENABLE_UBSAN=ON -DX=\$HOME/' 'expand variables'
rejects build-mapping 's/targets\+=\("test-/targets+=("t-/' 'build step changed shape'
rejects ctest-option 's/--output-on-failure --no-tests=error \\/--output-on-failure --no-tests=error --repeat until-pass:3 \\/' "passes ctest '--repeat'"
rejects bare-assignment 's/^( *)(export LSAN_OPTIONS=)/$1GLIB_DEBUG=all\n$1$2/' 'test step sets a variable'
rejects inline-env 's/          ctest --test-dir build-asan --parallel/          G_DEBUG=x ctest --test-dir build-asan --parallel/' 'test step sets a variable'
rejects export-only 's/^( *)(export LSAN_OPTIONS=)/$1export G_DEBUG\n$1$2/' 'test step sets a variable'
rejects export-expansion 's/suppressions=\$PWD/suppressions=\$HOME/' 'expands more than \$PWD'
rejects no-skip-check "s/grep -Eq '[^']*' build-asan/grep -q x build-asan/" 'no longer rejects skipped'
rejects other-selection 's/<<< "\$GROUNDHOG_SANITIZER_TESTS"/<<< "\$OTHER"/' 'no longer selects'
if python3 "$helper" config --workflow "$tmp/none.yml" > /dev/null 2>&1; then
    fail "a missing workflow was accepted"
fi
echo 'ok: an unreadable job fails'

# ---- the YAML subset --------------------------------------------------------
cat > "$tmp/mini.yml" <<'YML'
on:
  push:
    branches: [main]
jobs:
  groundhog-sanitizers:
    env:
      GROUNDHOG_SANITIZER_TESTS: >-
        groundhog-a
        other_b   # kept inside folded text
    steps:
      - name: "Configure: quoted"
        run: cmake -S . -B out -G Ninja '-DX=a b'
      - name: Build
        run: |
          # comment lines are skipped
          targets=(groundhog extra)
          for test in $GROUNDHOG_SANITIZER_TESTS; do
            case "$test" in groundhog-*) targets+=("test-$test") ;; *) targets+=("$test") ;; esac
          done
          cmake --build out --parallel 2 --target "${targets[@]}"
      - name: Run
        env:
          ASAN_OPTIONS: 'a=1:b=2'
          G_SLICE: always-malloc # trailing comment
        run: |
          export LSAN_OPTIONS="suppressions=${PWD}/s.supp"
          read -ra tests <<< "$GROUNDHOG_SANITIZER_TESTS"
          regex="^($(IFS='|'; echo "${tests[*]}"))\$"
          ctest --test-dir out -R "$regex" | tee out/ctest.log
          if grep -Eq 'Skipped' out/ctest.log; then exit 1; fi
YML
config "$tmp/mini.yml"
[ "${GH_SAN_TESTS[*]}" = "groundhog-a other_b # kept inside folded text" ] ||
    fail "folded scalar: ${GH_SAN_TESTS[*]}"
perl -pi -e 's/   # kept inside folded text//' "$tmp/mini.yml"
config "$tmp/mini.yml"
[ "${GH_SAN_TESTS[*]}" = "groundhog-a other_b" ] || fail "folded scalar: ${GH_SAN_TESTS[*]}"
[ "${GH_SAN_TARGETS[*]}" = "groundhog extra test-groundhog-a other_b" ] || fail "targets: ${GH_SAN_TARGETS[*]}"
[ "${#GH_SAN_CONFIGURE[@]}" -eq 3 ] && [ "${GH_SAN_CONFIGURE[2]}" = "-DX=a b" ] ||
    fail "quoted configure argument: ${GH_SAN_CONFIGURE[*]}"
[ "${GH_SAN_ENV[*]}" = "ASAN_OPTIONS=a=1:b=2 G_SLICE=always-malloc LSAN_OPTIONS=suppressions=/work/src/s.supp" ] ||
    fail "env: ${GH_SAN_ENV[*]}"
[ "$GH_SAN_SKIP_PATTERN" = Skipped ] || fail "skip pattern: $GH_SAN_SKIP_PATTERN"
[ "$GH_SAN_TEST_JOBS" = 1 ] || fail "no --parallel is not serial: $GH_SAN_TEST_JOBS"
echo 'ok: the YAML subset'

# ---- the path filter ----------------------------------------------------------
affected() {  # affected EXPECTED(run|skip) PATH...
    local want="$1" got=run
    shift
    printf '%s\n' "$@" | python3 "$helper" affected > "$tmp/why" || got=skip
    [ "$got" = "$want" ] || fail "paths [$*]: $got, expected $want ($(cat "$tmp/why"))"
}
for path in gnome/groundhog/src/mls/gh-mls-service.c gnome/groundhog/tests/lsan.supp \
    libnostr/src/event.c nostr-gobject/src/nostr_relay.c libmarmot/src/mls/x.c \
    marmot-gobject/src/y.c libgo/fiber/sched/z.c nips/nip44/src/core/a.c \
    tests/test_connection_recv_drain.c tests/CMakeLists.txt \
    .github/workflows/groundhog-ci.yml; do
    affected run "$path"
done
for path in docs/plans/x.md README.md AGENTS.md apps/gnostr/src/main.c nostr-gtk/src/w.c \
    gnome/nostr-homed/src/a.c gnome/libnostr-publish/src/p.c libnostr-extra/a.c \
    libgobject/x.c nipsy/x.c Testing/tests/t.c apps/gnostr/tests/t.c \
    .github/workflows/groundhog-ci.yml.bak .github/workflows/static-checks.yml \
    scripts/pre-push CMakeLists.txt; do
    affected skip "$path"
done
affected skip
affected run docs/a.md gnome/groundhog/README.md
grep -q 'gnome/groundhog/ (1: gnome/groundhog/README.md)' "$tmp/why" || fail "run reason: $(cat "$tmp/why")"
affected skip docs/a.md AGENTS.md
grep -q 'none of its 2 changed file(s) is under' "$tmp/why" || fail "skip reason: $(cat "$tmp/why")"
echo 'ok: the path filter'

echo 'sanitizer gate tests passed'
