#!/usr/bin/env bash
#
# linux-gate.sh — the pre-push gate's Linux stage (nostrc-y9xg).
#
# Builds every default target of SOURCE_DIR, plus Groundhog, with GCC in an
# Ubuntu 24.04 container (scripts/linux-ci.Dockerfile), then runs a smoke ctest
# subset. The macOS-only gate let glibc/GCC-only breaks reach master: nanosleep
# without _POSIX_C_SOURCE under -std=c11, and strong duplicate archive symbols
# that Apple's ld64 accepts. scripts/pre-push runs this stage for every pushed
# commit, in parallel with the macOS build.
#
# Usage: scripts/linux-gate.sh SOURCE_DIR
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
#   NOSTRC_GATE_LINUX_TESTS=0  build only; skip the smoke tests.
#   JOBS                       build and test parallelism (default: container CPUs).
# scripts/pre-push also honours NOSTRC_SKIP_LINUX_GATE=1 (no Docker).
#
# State lives in the named volume nostrc-linux-gate-<arch>: the synced source
# (unchanged files keep their timestamps, so rebuilds are incremental) and the
# build tree. A lock inside it serialises concurrent gates.
# `docker volume rm nostrc-linux-gate-arm64` resets it.
set -euo pipefail
# Bash reads a script while running it: parse all of it first, so a checkout
# that rewrites this file mid-run cannot splice two versions together.
{

if [ $# -ne 1 ] || [ ! -f "$1/CMakeLists.txt" ]; then
    echo "usage: $0 SOURCE_DIR (a nostrc checkout)" >&2
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
if [ "${NOSTRC_GATE_AMD64:-0}" = 1 ]; then
    ARCH=amd64
fi
IMAGE="nostrc-linux-ci:$ARCH"
VOLUME="nostrc-linux-gate-$ARCH"

echo "==> Linux gate ($ARCH): preparing image $IMAGE (the first build takes a few minutes)"
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
BROKEN_ON_LINUX='gnostr-test-plugin-raw-relay-api|gnostr-test-image-viewer-remote-media|gnostr-test-delete-authorization|gnostr-test-ndb-main-thread-violations|gnostr-test-real-bind-latency'
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
  echo "==> Linux gate: waiting for another gate using this volume"
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
echo "==> Linux gate: source synced ($(stamp))"
configure() {
  cmake -S /work/src -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_GROUNDHOG=ON -DBUILD_TESTING=ON \
    -DBLUEPRINT_COMPILER=OFF "-DCMAKE_C_FLAGS=$STRICT_CFLAGS" >/work/configure.log 2>&1
}
# BLUEPRINT_COMPILER=OFF: Gnostr, gnostr-signer and nostr-gtk compile .blp
# into their committed data/ui/*.ui, and Noble'"'"'s blueprint-compiler 0.12 cannot
# compile Gnostr'"'"'s; all four components then bundle the committed .ui.
# Blueprint output is platform independent: the macOS stage and groundhog-ci
# still compile it.
if ! configure; then
  echo "==> Linux gate: configure failed on the cached build tree; retrying from scratch"
  rm -rf /work/build
  configure || { tail -60 /work/configure.log; echo "==> Linux gate: CONFIGURE FAILED"; exit 1; }
fi
echo "==> Linux gate: configured ($(stamp))"
if ! cmake --build /work/build --parallel "$JOBS" >/work/build.log 2>&1; then
  show_errors /work/build.log
  echo "==> Linux gate: BUILD FAILED ($(stamp))"
  exit 1
fi
echo "==> Linux gate: built all targets ($(stamp))"
[ "$RUN_TESTS" = 1 ] || { echo "==> Linux gate: smoke tests skipped (NOSTRC_GATE_LINUX_TESTS=0)"; exit 0; }
# Widget tests need a display and a session bus, as in groundhog-ci.yml. The
# tests run in parallel beside the macOS build; a test that fails is run once
# more on its own, which absorbs a race lost under that load (reported), while
# a real break fails both times.
smoke() {
  dbus-run-session -- xvfb-run -a -s "-screen 0 1280x800x24" \
    ctest --test-dir /work/build --timeout 120 --no-tests=error "$@"
}
if ! smoke --parallel "$JOBS" -E "$SMOKE_EXCLUDE" >/work/ctest.log 2>&1; then
  first="$(grep -E "tests passed" /work/ctest.log)"
  failed="$(sed -n "/The following tests FAILED/,/^Errors while running/ s/^[[:space:]]*[0-9]* - \([^ ]*\) (.*/\1/p" /work/ctest.log | paste -sd " " -)"
  # By name: --rerun-failed goes by test number, and CTest numbers the tests
  # within the -E selection, so it would rerun different tests.
  if ! smoke -R "^($(printf "%s" "$failed" | tr " " "|"))\$" --output-on-failure \
      >/work/ctest-rerun.log 2>&1; then
    echo "$first"
    sed -n "/The following tests FAILED/,/^Errors while running/p" /work/ctest-rerun.log
    echo "==> Linux gate: SMOKE TESTS FAILED ($(stamp)); logs in volume '"$VOLUME"' under /work"
    exit 1
  fi
  rerun="$(grep -cE "^ *[0-9]+/[0-9]+ Test +#" /work/ctest-rerun.log || true)"
  if [ "$rerun" -lt "$(printf "%s\n" $failed | grep -c .)" ]; then
    echo "==> Linux gate: SMOKE TESTS FAILED: the rerun ran $rerun test(s) for: $failed"
    exit 1
  fi
  echo "==> Linux gate: $first; passed when rerun alone: $failed"
fi
echo "==> Linux gate: smoke tests passed, $(grep -E "tests passed" /work/ctest.log | sed "s/.*out of //") run ($(stamp))"'

# --init: a signal to this script (the macOS stage failed) stops the container.
exec docker run --rm --init --platform "linux/$ARCH" \
    -e JOBS="${JOBS:-}" -e RUN_TESTS="${NOSTRC_GATE_LINUX_TESTS:-1}" \
    -e STRICT_CFLAGS="$STRICT_CFLAGS" -e SMOKE_EXCLUDE="$SMOKE_EXCLUDE" \
    -v "$SOURCE:/src:ro" -v "$VOLUME:/work" "$IMAGE" \
    bash -c 'JOBS="${JOBS:-$(nproc)}"; '"$CMD"
}
