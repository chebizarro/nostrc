#!/usr/bin/env bash
#
# linux-gate.sh — the pre-push gate's Linux stages (nostrc-y9xg, nostrc-3han).
#
# Default: builds every default target of SOURCE_DIR, plus Groundhog, with GCC
# in an Ubuntu 24.04 container (scripts/linux-ci.Dockerfile), then runs a smoke
# ctest subset. The macOS-only gate let glibc/GCC-only breaks reach master:
# nanosleep without _POSIX_C_SOURCE under -std=c11, and strong duplicate
# archive symbols that Apple's ld64 accepts. scripts/pre-push runs this stage
# for every pushed commit, in parallel with the macOS build.
#
# --sanitizers: the `groundhog-sanitizers` job of groundhog-ci.yml instead, in
# the same image: its configure flags (ASAN+UBSAN), its build targets, its
# "every listed test is registered" and "none skipped" checks, and its tests
# under its ASAN/UBSAN/LSAN options and gnome/groundhog/tests/lsan.supp. All of
# it is read from SOURCE_DIR's workflow at run time (scripts/sanitizer-gate-ci.py),
# so the stage and CI cannot drift apart. A failed test whose output holds a
# sanitizer report blocks without a rerun; any other failure is rerun once as
# in the smoke run. The MLS leaks of nostrc-kdxe reached master because only
# CI ran this job. scripts/pre-push runs it beside the other two stages when
# the pushed range touches what the job tests (sanitizer-gate-ci.py affected).
# It always runs on the host architecture (ASAN does not run under emulation).
#
# Usage: scripts/linux-gate.sh [--sanitizers] SOURCE_DIR
#   SOURCE_DIR (a clean checkout; untracked build directories are copied too)
#   is mounted read-only and never written: the tree is checksum-synced into a
#   named volume and built there, and Blueprint regeneration of committed .ui
#   files is disabled (see BLUEPRINT_COMPILER below).
#
# Environment:
#   NOSTRC_GATE_AMD64=1        build and test linux/amd64 under emulation instead
#                              of the host architecture. Slow, but x86_64 GCC
#                              evaluates call arguments right to left, so
#                              unsequenced-argument bugs show up at run time.
#   NOSTRC_GATE_IMAGE_TAG=...  private Docker image tag for parallel worktrees;
#                              default nostrc-linux-ci:<arch>.
#   NOSTRC_GATE_VOLUME=...     private build/source volume for parallel worktrees;
#                              default nostrc-linux-gate[-asan]-<arch>. The
#                              cross-gate build/test locks remain shared.
#   NOSTRC_GATE_LINUX_TESTS=0  build only; skip the smoke tests (not --sanitizers).
#   JOBS                       build and test parallelism (default: container CPUs).
#   NOSTRC_SANITIZER_TEST_JOBS --sanitizers test parallelism (default: the job's
#                              ctest --parallel; under more load its ASAN tests
#                              pass their timeouts and race into leak reports).
#   NOSTRC_GATE_HOST_BUILD_SIGNAL  --sanitizers: a host file whose creation
#                              says the host's own build is done; the tests wait
#                              for it (scripts/pre-push sets it for the macOS build).
# scripts/pre-push also honours NOSTRC_SKIP_LINUX_GATE=1 (no Docker) and
# NOSTRC_SKIP_SANITIZER_GATE=1.
#
# State lives in the named volume nostrc-linux-gate-<arch> (--sanitizers:
# nostrc-linux-gate-asan-<arch>): the synced source (unchanged files keep their
# timestamps, so rebuilds are incremental) and the build tree. A lock inside it
# serialises concurrent gates. A change of configure arguments starts the build
# tree afresh, so no cached option outlives its removal from CI (nor its
# configure env: CC, CFLAGS).
#
# Load: beside a build or the 14-way smoke run, ASAN tests pass their timeouts
# and lose exit races into leak reports that CI never sees. So the sanitizer
# tests run alone, by two locks in the volume nostrc-linux-gate-tests shared by
# every gate: each Linux build holds build.lock shared and the sanitizer tests
# hold it exclusive (no Linux build of any gate runs beside them), and the test
# runs of both stages take turns on tests.lock. The host's build is outside the
# VM: the sanitizer tests first wait for NOSTRC_GATE_HOST_BUILD_SIGNAL.
# `docker volume rm nostrc-linux-gate-arm64` resets it.
set -euo pipefail
# Bash reads a script while running it: parse all of it first, so a checkout
# that rewrites this file mid-run cannot splice two versions together.
{

MODE=smoke
if [ "${1:-}" = --sanitizers ]; then
    MODE=sanitizers
    shift
fi
if [ $# -ne 1 ] || [ ! -f "$1/CMakeLists.txt" ]; then
    echo "usage: $0 [--sanitizers] SOURCE_DIR (a nostrc checkout)" >&2
    exit 2
fi
SOURCE="$(cd "$1" && pwd)"
SCRIPTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

NATIVE="$(docker info --format '{{.Architecture}}')"
case "$NATIVE" in
    aarch64|arm64) NATIVE=arm64 ;;
    x86_64|amd64) NATIVE=amd64 ;;
esac
ARCH="$NATIVE"
if [ "${NOSTRC_GATE_AMD64:-0}" = 1 ] && [ "$MODE" = smoke ]; then
    ARCH=amd64
fi
IMAGE="${NOSTRC_GATE_IMAGE_TAG:-nostrc-linux-ci:$ARCH}"
VOLUME="nostrc-linux-gate-$ARCH"
GATE="Linux gate"
GATE_CONFIG=""
if [ "$MODE" = sanitizers ]; then
    VOLUME="nostrc-linux-gate-asan-$ARCH"
    GATE="Sanitizer gate"
    # The job as the candidate's CI will run it. Parsed here, before any
    # container starts, so a job this gate cannot reproduce fails at once.
    if ! GATE_CONFIG="$(python3 "$SCRIPTS/sanitizer-gate-ci.py" config \
            --workflow "$SOURCE/.github/workflows/groundhog-ci.yml" --source-root /work/src)"; then
        echo "==> $GATE: cannot read the groundhog-sanitizers job; SANITIZER GATE FAILED" >&2
        exit 1
    fi
fi
VOLUME="${NOSTRC_GATE_VOLUME:-$VOLUME}"

echo "==> $GATE ($ARCH): preparing image $IMAGE (the first build takes a few minutes)"
docker build -q --platform "linux/$ARCH" -t "$IMAGE" - < "$SCRIPTS/linux-ci.Dockerfile" >/dev/null

# GCC 14 and Clang 16 reject these by default, and hosted CI's Clang jobs fail
# on them; Ubuntu 24.04's GCC 13 only warns. The first is the nanosleep class
# (a POSIX function hidden by -std=c11 is implicitly declared). array-bounds is
# the class GCC raised in the Groundhog sanitizer job (nostrc-qp24.91).
STRICT_CFLAGS='-Werror=implicit-function-declaration -Werror=implicit-int -Werror=int-conversion -Werror=incompatible-pointer-types -Werror=array-bounds'

# Smoke set: every registered test except
#  - those over ~13 s each (scrypt, stress, D-Bus contract suites), which hosted
#    CI runs; the rest finish in about half a minute on 14 cores;
#  - gnostr tests that cannot load on Linux yet (nostrc-taue: they link with
#    unresolved symbols, and -z now rejects them). Drop them here when fixed.
SLOW_TESTS='test_nip49_roundtrip|profile_fetch_stress|signer/crypto|gnostr-test-event-model-windowing|nip55l_dbus_contract|libnostr_reconnect_same_second'
BROKEN_ON_LINUX='gnostr-test-image-viewer-remote-media|gnostr-test-delete-authorization|gnostr-test-ndb-main-thread-violations|gnostr-test-real-bind-latency'
# Under emulation (Rosetta, qemu), left out of the NOSTRC_GATE_AMD64=1 run:
#  - nostr_signer_webext_host_e2e: /proc/<pid>/exe of another process names the
#    emulator (/run/rosetta/rosetta), so the signer cannot attest the webext
#    host's executable, its exe: grants never match and the request times out;
#  - test_nostr_gtk_bind_latency_budget: first-run translation stalls the main
#    loop 400-650 ms against its 200 ms budget;
#  - relayd_session_relay_storage: "event lost across restart" on linux/amd64
#    only, emulation artifact or real x86_64 bug still open (nostrc-5djt).
EMULATION_ONLY='nostr_signer_webext_host_e2e|test_nostr_gtk_bind_latency_budget'
AMD64_KNOWN='relayd_session_relay_storage'
if [ "$ARCH" != "$NATIVE" ]; then
    BROKEN_ON_LINUX="$BROKEN_ON_LINUX|$EMULATION_ONLY|$AMD64_KNOWN"
fi
SMOKE_EXCLUDE="^($SLOW_TESTS|$BROKEN_ON_LINUX)\$"

# The script below runs in the container; "$@" there is not this script's.
# shellcheck disable=SC2016
CMD='set -euo pipefail
exec 9>/work/.lock
if ! flock -n 9; then
  echo "==> $GATE: waiting for another gate using this volume"
  flock 9
fi
stamp() { printf "%dm%02ds" $((SECONDS / 60)) $((SECONDS % 60)); }
show_errors() {
  if grep -qE "error:|FAILED:|undefined reference|multiple definition" "$1"; then
    grep -E -B2 -A3 "error:|undefined reference|multiple definition" "$1" | head -80
    grep -E "^FAILED:" "$1" | head -20
  else
    tail -60 "$1"
  fi
}
SECONDS=0
# -c: compare content, not times, and do not copy times (-t): a file whose
# content is unchanged keeps its old timestamp in the volume, so Ninja only
# rebuilds what the candidate really changed.
rsync -rlc --delete --exclude=.git \
  --exclude=/build/ --exclude=/_build/ --exclude="/build-*/" --exclude="/cmake-build-*/" \
  /src/ /work/src/
echo "==> $GATE: source synced ($(stamp))"
if [ "$MODE" = sanitizers ]; then
  # GH_SAN_TESTS, GH_SAN_TARGETS, GH_SAN_CONFIGURE, GH_SAN_ENV and
  # GH_SAN_SKIP_PATTERN, from the job (scripts/sanitizer-gate-ci.py).
  eval "$GATE_CONFIG"
  CONFIGURE_ARGS=("${GH_SAN_CONFIGURE[@]}")
  CONFIGURE_ENV=("${GH_SAN_CONFIGURE_ENV[@]}")
  BUILD_ENV=("${GH_SAN_BUILD_ENV[@]}")
  BUILD_ARGS=(--target "${GH_SAN_TARGETS[@]}")
  BUILT="groundhog and the sanitizer tests"
else
  # BLUEPRINT_COMPILER=OFF: Gnostr, gnostr-signer and nostr-gtk compile .blp
  # into their committed data/ui/*.ui, and Noble'"'"'s blueprint-compiler 0.12
  # cannot compile Gnostr'"'"'s; all four components then bundle the committed
  # .ui. Blueprint output is platform independent: the macOS stage and
  # groundhog-ci still compile it.
  CONFIGURE_ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_GROUNDHOG=ON -DBUILD_TESTING=ON
    -DBLUEPRINT_COMPILER=OFF "-DCMAKE_C_FLAGS=$STRICT_CFLAGS")
  CONFIGURE_ENV=()
  BUILD_ENV=()
  BUILD_ARGS=()
  BUILT="all targets"
fi
# A cache keeps an option (or a compiler) CI no longer passes: other arguments
# or configure env, a fresh tree. (A volume from before this record adopts it.)
CONFIGURE_RECORD="$(printf "%s\n" "${CONFIGURE_ARGS[@]}"; [ ${#CONFIGURE_ENV[@]} -eq 0 ] ||
                    printf "env %s\n" "${CONFIGURE_ENV[@]}")"
if [ -f /work/configure.args ] && [ "$CONFIGURE_RECORD" != "$(cat /work/configure.args)" ]; then
  echo "==> $GATE: configure arguments changed; starting the build tree afresh"
  rm -rf /work/build
fi
configure() {
  env "${CONFIGURE_ENV[@]}" cmake -S /work/src -B /work/build "${CONFIGURE_ARGS[@]}" \
    >/work/configure.log 2>&1
}
if ! configure; then
  echo "==> $GATE: configure failed on the cached build tree; retrying from scratch"
  rm -rf /work/build
  configure || { tail -60 /work/configure.log; echo "==> $GATE: CONFIGURE FAILED"; exit 1; }
fi
printf "%s\n" "$CONFIGURE_RECORD" > /work/configure.args
echo "==> $GATE: configured ($(stamp))"
# Builds share build.lock; the sanitizer tests hold it alone (see above).
exec 7>/gate-lock/build.lock
if ! flock -n -s 7; then
  echo "==> $GATE: waiting for the sanitizer tests of a gate to finish before building ($(stamp))"
  flock -s 7
fi
if ! env "${BUILD_ENV[@]}" cmake --build /work/build --parallel "$JOBS" "${BUILD_ARGS[@]}" \
    >/work/build.log 2>&1; then
  show_errors /work/build.log
  echo "==> $GATE: BUILD FAILED ($(stamp))"
  exit 1
fi
exec 7>&-
echo "==> $GATE: built $BUILT ($(stamp))"
if [ "$MODE" = sanitizers ]; then
  # The path filter scripts/pre-push runs this stage by must cover every
  # source input of this build (scripts/sanitizer-gate-ci.py check-inputs).
  if ! (cd /work/src && python3 /gate/sanitizer-gate-ci.py check-inputs --build-dir /work/build); then
    echo "==> $GATE: THE PATH FILTER MISSES INPUTS OF THIS BUILD ($(stamp))"
    exit 1
  fi
  # As the job does: every listed test is registered, none silently dropped.
  TEST_REGEX="^($(IFS="|"; echo "${GH_SAN_TESTS[*]}"))\$"
  expected="$(printf "%s\n" "${GH_SAN_TESTS[@]}" | sort)"
  registered="$(ctest --test-dir /work/build -N -R "$TEST_REGEX" |
                sed -n "s/^ *Test *#[0-9]*: *//p" | sort)"
  if [ "$registered" != "$expected" ]; then
    diff <(echo "$expected") <(echo "$registered") || true
    echo "==> $GATE: THE SANITIZER TEST SET IS INCOMPLETE ($(stamp))"
    exit 1
  fi
  # The job'"'"'s ASAN/UBSAN/LSAN options, for ctest and every test it starts.
  export "${GH_SAN_ENV[@]}"
  # No display (the job has none), CI'"'"'s default timeout and parallelism,
  # each failed test'"'"'s whole output, the job'"'"'s skip check, and no rerun past
  # a sanitizer report.
  export TEST_REGEX DISPLAY_WRAP=0 CTEST_TIMEOUT= TAIL_LINES=all \
    FORBID_PATTERN="$GH_SAN_SKIP_PATTERN" SANITIZER_REPORTS=block \
    GATE SUITE="sanitizer tests" JOBS="${SANITIZER_TEST_JOBS:-$GH_SAN_TEST_JOBS}"
  # Alone: after the host'"'"'s build, and with no Linux build running (fd 7,
  # exclusive, held through the run and its rerun).
  if [ -n "${GATE_WAIT_FILE:-}" ] && [ ! -e "$GATE_WAIT_FILE" ]; then
    echo "==> $GATE: waiting for the host build to finish ($(stamp))"
    until [ -e "$GATE_WAIT_FILE" ]; do sleep 1; done
  fi
  exec 7>/gate-lock/build.lock
  if ! flock -n -x 7; then
    echo "==> $GATE: waiting for Linux builds to finish ($(stamp))"
    flock -x 7
  fi
  echo "==> $GATE: running ${#GH_SAN_TESTS[@]} tests, $JOBS at a time ($(stamp))"
else
  [ "$RUN_TESTS" = 1 ] || { echo "==> $GATE: smoke tests skipped (NOSTRC_GATE_LINUX_TESTS=0)"; exit 0; }
fi
# One gate test run at a time (smoke or sanitizer, of any push): see above.
exec 8>/gate-lock/tests.lock
if ! flock -n 8; then
  echo "==> $GATE: waiting for another gate'"'"'s test run to finish ($(stamp))"
  flock 8
fi
# The test run, and a rerun of what failed in it: scripts/linux-gate-smoke.sh
# (mounted from beside this script, so the two always match). The locks stay
# held: fds 7 (sanitizers), 8 and 9 are inherited.
GATE_SECONDS=$SECONDS exec bash /gate/smoke.sh'

SIGNAL_MOUNT=()
if [ "$MODE" = sanitizers ] && [ -n "${NOSTRC_GATE_HOST_BUILD_SIGNAL:-}" ]; then
    SIGNAL_MOUNT=(-v "$(dirname "$NOSTRC_GATE_HOST_BUILD_SIGNAL"):/gate-signal:ro"
                  -e "GATE_WAIT_FILE=/gate-signal/$(basename "$NOSTRC_GATE_HOST_BUILD_SIGNAL")")
fi

# --init: a signal to this script (the macOS stage failed) stops the container.
# (${SIGNAL_MOUNT[@]+...}: bash 3.2 calls an empty array unbound under set -u.)
exec docker run --rm --init --platform "linux/$ARCH" ${SIGNAL_MOUNT[@]+"${SIGNAL_MOUNT[@]}"} \
    -e JOBS="${JOBS:-}" -e RUN_TESTS="${NOSTRC_GATE_LINUX_TESTS:-1}" \
    -e STRICT_CFLAGS="$STRICT_CFLAGS" -e SMOKE_EXCLUDE="$SMOKE_EXCLUDE" \
    -e VOLUME="$VOLUME" -e MODE="$MODE" -e GATE="$GATE" -e GATE_CONFIG="$GATE_CONFIG" \
    -e SANITIZER_TEST_JOBS="${NOSTRC_SANITIZER_TEST_JOBS:-}" \
    -v "$SOURCE:/src:ro" -v "$SCRIPTS/linux-gate-smoke.sh:/gate/smoke.sh:ro" \
    -v "$SCRIPTS/sanitizer-gate-ci.py:/gate/sanitizer-gate-ci.py:ro" \
    -v "$VOLUME:/work" -v nostrc-linux-gate-tests:/gate-lock "$IMAGE" \
    bash -c 'JOBS="${JOBS:-$(nproc)}"; '"$CMD"
}
