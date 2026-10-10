#!/usr/bin/env bash
# Tests scripts/sanitizer-gate-ci.py (nostrc-3han): the pre-push sanitizer
# stage reads groundhog-ci.yml's groundhog-sanitizers job as CI runs it (env
# of the workflow, job and each step included), fails on a job it cannot
# reproduce, runs only for pushes touching the job's inputs, and its input
# list is checked against the build's ninja graph.
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
    GH_SAN_CONFIGURE_ENV=() GH_SAN_BUILD_ENV=()
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

# Env, wherever the workflow sets it, reaches the phase CI gives it to.
# UBSAN_OPTIONS moved from the test step into the job's env: still applied.
perl -0pe 's/(GROUNDHOG_SANITIZER_TESTS: >-)/UBSAN_OPTIONS: print_stacktrace=1:halt_on_error=1\n      $1/;
           s/\n +UBSAN_OPTIONS: print_stacktrace=1:halt_on_error=1\n +G_SLICE/\n          G_SLICE/' \
    "$workflow" > "$tmp/job-env.yml"
grep -c 'UBSAN_OPTIONS: print_stacktrace' "$tmp/job-env.yml" | grep -qx 1 || fail "job-env mutation"
config "$tmp/job-env.yml"
has UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 "${GH_SAN_ENV[@]}" ||
    fail "UBSAN_OPTIONS in the job env was dropped: ${GH_SAN_ENV[*]}"
has UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 "${GH_SAN_BUILD_ENV[@]}" ||
    fail "the job env did not reach the build step"
# Workflow env reaches every phase; a step's env overrides it, as in Actions.
perl -0pe 's/\njobs:\n/\nenv:\n  ASAN_OPTIONS: detect_leaks=0\n  NOSTRC_WF: on\n\njobs:\n/' \
    "$workflow" > "$tmp/wf-env.yml"
config "$tmp/wf-env.yml"
has NOSTRC_WF=on "${GH_SAN_ENV[@]}" || fail "workflow env did not reach the tests"
has NOSTRC_WF=on "${GH_SAN_CONFIGURE_ENV[@]}" || fail "workflow env did not reach configure"
has ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:verify_asan_link_order=0 "${GH_SAN_ENV[@]}" ||
    fail "the test step's ASAN_OPTIONS did not override the workflow's"
has ASAN_OPTIONS=detect_leaks=0 "${GH_SAN_CONFIGURE_ENV[@]}" || fail "workflow env missing at configure"
# The configure step's CC and CFLAGS reach configure, and only configure.
perl -0pe 's/(      - name: Configure with ASAN and UBSAN\n)/$1        env:\n          CC: clang\n          CFLAGS: -O1\n/' \
    "$workflow" > "$tmp/cc.yml"
config "$tmp/cc.yml"
has CC=clang "${GH_SAN_CONFIGURE_ENV[@]}" && has CFLAGS=-O1 "${GH_SAN_CONFIGURE_ENV[@]}" ||
    fail "configure step env dropped: ${GH_SAN_CONFIGURE_ENV[*]}"
! has CC=clang "${GH_SAN_ENV[@]}" || fail "the configure step's env leaked into the tests"
for e in ${GH_SAN_CONFIGURE_ENV[@]+"${GH_SAN_CONFIGURE_ENV[@]}"} ${GH_SAN_BUILD_ENV[@]+"${GH_SAN_BUILD_ENV[@]}"}; do
    case "$e" in GROUNDHOG_SANITIZER_TESTS=*) fail "the test list is in the configure/build env" ;; esac
done
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
# What the gate would not reproduce: fail, never run weaker than CI.
rejects wf-defaults 's/^jobs:$/defaults:\n  run:\n    shell: sh\njobs:/' 'workflow-level defaults'
rejects job-defaults 's/^(  groundhog-sanitizers:)$/$1\n    defaults:\n      run:\n        working-directory: x/' 'has defaults'
rejects job-container 's/^(  groundhog-sanitizers:)$/$1\n    container: ubuntu:26.04/' 'has container'
rejects job-services 's/^(  groundhog-sanitizers:)$/$1\n    services:\n      relay:\n        image: x/' 'has services'
rejects job-matrix 's/^(  groundhog-sanitizers:)$/$1\n    strategy:\n      matrix:\n        cc: [gcc, clang]/' 'has strategy'
rejects runs-on 'BEGIN { undef $/ } s/(\n  groundhog-sanitizers:\n(?: +#[^\n]*\n)*)    runs-on: ubuntu-24.04/$1    runs-on: ubuntu-26.04/' "runs on 'ubuntu-26.04'"
rejects test-workdir 's/^(      - name: Run them under the sanitizers)$/$1\n        working-directory: build-asan/' 'test step has working-directory'
rejects configure-shell 's/^(      - name: Configure with ASAN and UBSAN)$/$1\n        shell: sh/' 'configure step has shell'
rejects build-if 's/^(      - name: Build the display-free tests)$/$1\n        if: false/' 'build step has if'
rejects env-expression 's/^( +G_SLICE: )always-malloc$/$1\${{ vars.SLICE }}/' 'not a plain value'
rejects github-env 's/^(          cmake -S \. -B build-asan)/          echo CC=clang >> "\$GITHUB_ENV"\n$1/' 'writes \$GITHUB_ENV'
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
    runs-on: ubuntu-24.04
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
[ "${GH_SAN_ENV[*]}" = "GROUNDHOG_SANITIZER_TESTS=groundhog-a other_b ASAN_OPTIONS=a=1:b=2 G_SLICE=always-malloc LSAN_OPTIONS=suppressions=/work/src/s.supp" ] ||
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
    nostr-gtk/src/w.c \
    tests/test_connection_recv_drain.c tests/CMakeLists.txt \
    .github/workflows/groundhog-ci.yml \
    CMakeLists.txt NipOptions.cmake cmake/NostrcTestBus.cmake cmake/GnTest.cmake \
    gnome/seahorse/secret_store.c libjson/src/json.c components/nostrdb/src/nostrdb_storage.c \
    third_party/nostrdb third_party/nsync/internal/mu.c \
    apps/gnostr/data/schemas/org.gnostr.gnostr.gschema.xml scripts/linux-ci.Dockerfile \
    Testing/CMakeLists.txt benchmark/CMakeLists.txt libhanami/CMakeLists.txt \
    tools/CMakeLists.txt scripts/linux-gate.sh scripts/sanitizer-gate-ci.py; do
    affected run "$path"
done
for path in docs/plans/x.md README.md AGENTS.md apps/gnostr/src/main.c \
    gnome/nostr-homed/src/a.c gnome/libnostr-publish/src/p.c libnostr-extra/a.c \
    libgobject/x.c nipsy/x.c Testing/tests/t.c apps/gnostr/tests/t.c \
    .github/workflows/groundhog-ci.yml.bak .github/workflows/static-checks.yml \
    scripts/pre-push apps/gnostr/CMakeLists.txt apps/gnostr/data/schemas/other.xml \
    components/other/x.c third_partyx/y.c cmakefoo/z.cmake libhanami/src/h.c \
    tools/nostr-cli.c benchmark/bench.c gnome/CMakeLists.txt; do
    affected skip "$path"
done
affected skip
affected run docs/a.md gnome/groundhog/README.md
grep -q 'gnome/groundhog/ (1: gnome/groundhog/README.md)' "$tmp/why" || fail "run reason: $(cat "$tmp/why")"
affected skip docs/a.md AGENTS.md
grep -q 'none of its 2 changed file(s) is under' "$tmp/why" || fail "skip reason: $(cat "$tmp/why")"
echo 'ok: the path filter'

# ---- the path filter against the build's ninja graph ------------------------
# A fake ninja answers for a source tree whose build dir is inside it, as in
# CI: generated files and system headers are not source inputs.
src="$tmp/src"
mkdir -p "$src/.github/workflows" "$src/build-asan" "$tmp/nbin"
cp "$workflow" "$src/.github/workflows/groundhog-ci.yml"
cat > "$tmp/nbin/ninja" <<'NINJA'
#!/bin/bash
printf '%s\n' "$*" >> "$NINJA_TRACE"
[ "$1" = -C ] || exit 3
shift 2
case "$1 $2 ${3:-}" in
    "-t inputs build.ninja") printf '../CMakeLists.txt\n../cmake/GnTest.cmake\nCMakeFiles/x.cmake\n' ;;
    "-t inputs "*) printf '../gnome/groundhog/src/a.c\ngnome/groundhog/gen.c\n%s\n' "${EXTRA_INPUT:-../libnostr/src/relay.c}" ;;
    "-t deps "*) printf 'a.o: #deps 3, deps mtime 1 (VALID)\n    ../libjson/include/j.h\n    /usr/include/stdio.h\n    %s/nips/nip19/n.h\n\n' "$SRC_ABS" ;;
    *) exit 3 ;;
esac
NINJA
chmod +x "$tmp/nbin/ninja"
check_inputs() {
    : > "$tmp/ninja-trace"
    (cd "$src" && PATH="$tmp/nbin:$PATH" NINJA_TRACE="$tmp/ninja-trace" SRC_ABS="$src" \
        python3 "$helper" check-inputs --build-dir build-asan)
}
check_inputs > "$tmp/out" 2>&1 || fail "check-inputs rejected covered inputs: $(cat "$tmp/out")"
grep -q 'all 6 source inputs' "$tmp/out" || fail "check-inputs count: $(cat "$tmp/out")"
grep -q -- '-t inputs groundhog test-groundhog-identity .*test_connection_recv_drain$' "$tmp/ninja-trace" ||
    fail "check-inputs did not ask for the job's targets"
if EXTRA_INPUT=../docs/new-input.cmake check_inputs > "$tmp/out" 2>&1; then
    fail "an input outside SANITIZER_BUILD_PATHS passed"
fi
grep -q '^  docs/new-input.cmake$' "$tmp/out" || fail "the uncovered input is not named: $(cat "$tmp/out")"
if EXTRA_INPUT=../gnome/nostr-homed/src/w.c check_inputs > /dev/null 2>&1; then
    fail "gnome/nostr-homed (not in the list) passed"
fi
echo 'ok: the input list against ninja'

echo 'sanitizer gate tests passed'
