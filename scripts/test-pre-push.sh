#!/usr/bin/env bash
set -euo pipefail

scripts="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
repo="$tmp/main"
branch="$tmp/candidate"
mkdir -p "$repo/scripts" "$repo/.beads/hooks" "$tmp/bin" "$tmp/docker-bin"
git init -q "$repo"
git -C "$repo" config user.name Test
git -C "$repo" config user.email test@example.invalid
cp "$scripts/pre-push" "$repo/scripts/pre-push"
cp "$scripts/install-hooks.sh" "$repo/scripts/install-hooks.sh"
cp "$scripts/linux-gate.sh" "$repo/scripts/linux-gate.sh"
cp "$scripts/linux-gate-smoke.sh" "$repo/scripts/linux-gate-smoke.sh"
cp "$scripts/sanitizer-gate-ci.py" "$repo/scripts/sanitizer-gate-ci.py"
# The sanitizer stage reads the candidate's groundhog-sanitizers job.
mkdir -p "$repo/.github/workflows"
cp "$scripts/../.github/workflows/groundhog-ci.yml" "$repo/.github/workflows/groundhog-ci.yml"
printf 'FROM scratch\n' > "$repo/scripts/linux-ci.Dockerfile"
cat > "$repo/.beads/hooks/pre-push" <<'HOOK'
#!/bin/sh
# --- BEGIN BEADS INTEGRATION ---
printf 'BEADS\n' >> "$TRACE"
if [ "${DRAIN_STDIN:-0}" = 1 ]; then cat >/dev/null; fi
if [ "${FAIL_STAGE:-}" = beads ]; then exit 1; fi
# --- END BEADS INTEGRATION ---
HOOK
chmod +x "$repo/.beads/hooks/pre-push"
printf 'base\n' > "$repo/marker"
cat > "$repo/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.22)
project(pre_push_fixture NONE)
include(CTest)
CMAKE
git -C "$repo" add .
git -C "$repo" commit -qm base
base="$(git -C "$repo" rev-parse HEAD)"
git -C "$repo" update-ref refs/remotes/origin/master "$base"
git -C "$repo" config core.hooksPath "$repo/.beads/hooks"
"$repo/scripts/install-hooks.sh" >/dev/null
mkdir -p "$repo/_build"
printf 'keep\n' > "$repo/_build/sentinel"
git -C "$repo" worktree add -qb candidate "$branch" "$base"
mkdir -p "$branch/gnome/groundhog"
printf 'candidate\n' > "$branch/marker"
printf 'change\n' > "$branch/gnome/groundhog/feature"
git -C "$branch" add marker gnome/groundhog/feature
git -C "$branch" commit -qm groundhog
candidate="$(git -C "$branch" rev-parse HEAD)"
printf 'dirty working tree\n' > "$branch/marker"

cat > "$tmp/bin/cmake" <<'MOCK'
#!/bin/bash
set -eu
if [ "$1" = -S ]; then
    printf 'CONFIGURE %s\n' "$*" >> "$TRACE"
    [ "$2" != "$MAIN" ]
    grep -qx candidate "$2/marker"
    [ "${FAIL_STAGE:-}" != configure ]
else
    printf 'BUILD %s\n' "$*" >> "$TRACE"
    sleep "${BUILD_SLEEP:-0}"
    [ "${FAIL_STAGE:-}" != build ]
    if [ -n "${SPLICE:-}" ]; then
        printf '\nprintf "SPLICED\\n" >> "$TRACE"; exit 1\n' >> "$SPLICE"
    fi
fi
MOCK
# One test, "dummy", in CTest's output format: FAIL_STAGE=test fails it in
# every run, FAIL_STAGE=flaky only in the first (parallel) run.
cat > "$tmp/bin/ctest" <<'MOCK'
#!/bin/bash
set -eu
printf 'CTEST %s\n' "$*" >> "$TRACE"
if [ "${3:-}" = -N ]; then
    if [[ " $* " == *" -L ^perf$ "* ]]; then
        printf 'Total Tests: 0\n'
    else
        printf 'Total Tests: 1\n'
    fi
    exit 0
fi
run=rerun
case " $* " in *" --parallel "*) run=first ;; esac
if [ "$run" = rerun ] && [ -n "${CTEST_PARALLEL_LEVEL:-}" ]; then
    printf 'PARALLEL_RERUN\n' >> "$TRACE"
fi
fail=0
case "${FAIL_STAGE:-}" in
    test) fail=1 ;;
    flaky) [ "$run" = rerun ] || fail=1 ;;
esac
echo "      Start 1: dummy"
if [ "$fail" = 1 ]; then
    echo "1/1 Test #1: dummy ......................***Failed    0.10 sec"
    echo "DUMMY-OUTPUT-$run"
    echo
    echo "0% tests passed, 1 tests failed out of 1"
    echo
    echo "The following tests FAILED:"
    echo "	  1 - dummy (Failed)"
    echo "Errors while running CTest"
    exit 8
fi
echo "1/1 Test #1: dummy ......................   Passed    0.10 sec"
echo
echo "100% tests passed, 0 tests failed out of 1"
MOCK
# Docker stands in for the Linux stage. It is on PATH in every run, including
# the real-CMake ones, so no test starts a container.
cat > "$tmp/docker-bin/docker" <<'MOCK'
#!/bin/bash
set -eu
case "$1" in
    info)
        [ "${DOCKER_DOWN:-0}" != 1 ] || exit 1
        [ "${2:-}" != --format ] || echo aarch64 ;;
    build) ;;
    run)
        stage=linux
        case " $* " in *" nostrc-linux-gate-asan-"*) stage=sanitizer ;; esac
        # One trace line: GATE_CONFIG spans several.
        printf 'DOCKER_RUN %s %s\n' "$stage" "$(printf '%s' "$*" | tr '\n' ' ')" >> "$TRACE"
        src=""
        for arg; do case "$arg" in *:/src:ro) src="${arg%:/src:ro}" ;; esac; done
        # The container gets its own read-only checkout of the candidate.
        [ -n "$src" ] && [ "$src" != "$MAIN" ]
        case "$src" in */checkout) exit 1 ;; esac
        grep -qx candidate "$src/marker"
        if [ "$stage" = sanitizer ]; then
            # ... and the job as read from that checkout's workflow.
            case " $* " in *" GATE_CONFIG=GH_SAN_TESTS=(groundhog-identity "*) ;; *) exit 1 ;; esac
            case " $* " in *" MODE=sanitizers "*) ;; *) exit 1 ;; esac
            # Its tests wait for the host build's signal (a mounted file).
            signal_dir="" wait_file=""
            for arg; do
                case "$arg" in
                    *:/gate-signal:ro) signal_dir="${arg%:/gate-signal:ro}" ;;
                    GATE_WAIT_FILE=/gate-signal/*) wait_file="${arg#GATE_WAIT_FILE=/gate-signal/}" ;;
                esac
            done
            [ -n "$signal_dir" ] && [ -n "$wait_file" ] || { echo NO_SIGNAL_MOUNT >> "$TRACE"; exit 1; }
            for _ in $(seq 1 200); do [ -e "$signal_dir/$wait_file" ] && break; sleep 0.1; done
            [ -e "$signal_dir/$wait_file" ] || { echo NO_SIGNAL >> "$TRACE"; exit 1; }
            printf 'SANITIZER_TESTS\n' >> "$TRACE"
        fi
        sleep "${LINUX_SLEEP:-0}"
        [ "${FAIL_STAGE:-}" != "$stage" ] ;;
    *) exit 1 ;;
esac
MOCK
cat > "$tmp/docker-bin/pkg-config" <<'MOCK'
#!/bin/bash
set -eu
[ "$1" = --exists ] && [ "$2" = libqrencode ]
[ "${FAIL_STAGE:-}" != qrencode ]
MOCK
chmod +x "$tmp/bin/cmake" "$tmp/bin/ctest" "$tmp/docker-bin/docker" "$tmp/docker-bin/pkg-config"

# A bare "! grep" never fails a set -e script (bash exempts negated
# commands); this does.
absent() {
    if grep -q "$@"; then
        echo "unexpectedly found: $*" >&2
        exit 1
    fi
}

run_hook() {
    local fail_stage="${1:-}"
    local drain_stdin="${2:-0}"
    local tools_path="$tmp/docker-bin:$PATH"
    if [ "${3:-mock}" != real ]; then tools_path="$tmp/bin:$tools_path"; fi
    : > "$tmp/trace"
    printf 'refs/heads/candidate %s refs/heads/candidate %s\n' "$candidate" "$base" |
        (cd "$branch" && PATH="$tools_path" TRACE="$tmp/trace" MAIN="$repo" \
            FAIL_STAGE="$fail_stage" DRAIN_STDIN="$drain_stdin" "$repo/.beads/hooks/pre-push")
}
assert_clean() {
    [ "$(cat "$repo/_build/sentinel")" = keep ]
    [ "$(git -C "$repo" worktree list --porcelain | grep -c '^worktree ')" -eq 2 ]
    [ ! -e "$branch/_build" ]
}
run_hook > "$tmp/output" 2>&1
grep -q '^BEADS$' "$tmp/trace"
grep -q 'BUILD_GROUNDHOG=ON' "$tmp/trace"
grep -q '^BUILD ' "$tmp/trace"
grep -q '^CTEST ' "$tmp/trace"
grep -q '^DOCKER_RUN linux .*/linux-src:/src:ro' "$tmp/trace"
# The candidate touches gnome/groundhog: the sanitizer stage runs beside it.
grep -q '^DOCKER_RUN sanitizer .*nostrc-linux-gate-asan-arm64:/work' "$tmp/trace"
grep -q 'sanitizer stage runs: the range touches gnome/groundhog/' "$tmp/output"
# Its tests start only after the host (macOS) build.
awk '/^BUILD /{b=NR} /^SANITIZER_TESTS$/{t=NR} END{exit !(b && t && b < t)}' "$tmp/trace"
absent 'SKIPPED' "$tmp/output"
assert_clean

# A Groundhog build fails before configure with an actionable macOS dependency hint.
if run_hook qrencode > "$tmp/no-qrencode-output" 2>&1; then
    echo "missing libqrencode unexpectedly passed pre-push" >&2
    exit 1
fi
grep -q "brew install qrencode" "$tmp/no-qrencode-output"
absent '^CONFIGURE ' "$tmp/trace"
assert_clean

# An upstream hook may consume all of stdin; the build gate must still see the ref.
run_hook "" 1
grep -q '^BEADS$' "$tmp/trace"
grep -q '^CONFIGURE ' "$tmp/trace"
grep -q '^CTEST ' "$tmp/trace"
assert_clean

# No input must block, rather than letting a drained or malformed chain pass.
: > "$tmp/trace"
if (cd "$branch" && PATH="$tmp/bin:$tmp/docker-bin:$PATH" TRACE="$tmp/trace" MAIN="$repo" \
    "$repo/.beads/hooks/pre-push" </dev/null) > "$tmp/no-refs-output" 2>&1; then
    echo 'Empty pre-push input was accepted' >&2
    exit 1
fi
grep -q 'No pre-push refs received' "$tmp/no-refs-output"
absent '^CONFIGURE ' "$tmp/trace"
assert_clean

# Real CMake/CTest with no add_test calls returns success; the gate must reject it.
if run_hook "" 0 real > "$tmp/no-tests-output" 2>&1; then
    echo 'Zero registered CTest tests were accepted' >&2
    exit 1
fi
grep -q 'No registered CTest tests.*count=0' "$tmp/no-tests-output"
assert_clean

# The macOS run goes through linux-gate-smoke.sh: a test that fails and then
# passes alone passes the push, loudly, and is counted in the shared history.
CTEST_PARALLEL_LEVEL=3 run_hook flaky > "$tmp/flaky-output" 2>&1
grep -qF -- '--no-tests=error --output-on-failure --parallel 3 -LE ^perf$ -R .' "$tmp/trace"
grep -qF -- '-R ^(dummy)$' "$tmp/trace"
absent '^PARALLEL_RERUN$' "$tmp/trace"
grep -q 'DUMMY-OUTPUT-first' "$tmp/flaky-output"
grep -q '!! macOS gate: RERUN: failed in the parallel run, passed alone: dummy' "$tmp/flaky-output"
grep -qE 'dummy needed a rerun in 1 of the last [0-9]+ gate\(s\)' "$tmp/flaky-output"
grep -q 'macOS gate: tests passed after a rerun' "$tmp/flaky-output"
# Each test's result is printed as it comes (W25 review N4).
grep -q '^==> macOS gate: running tests, 3 at a time' "$tmp/flaky-output"
grep -q '^   1/1 Test #1: dummy .*Failed' "$tmp/flaky-output"
grep -q $'\tdummy$' "$repo/.git/nostrc-macos-gate-history/reruns"
ls "$repo/.git/nostrc-macos-gate-history/"*-first-run.log >/dev/null
assert_clean
# ... and a test that fails alone too blocks it.
if run_hook test > "$tmp/test-output" 2>&1; then
    echo 'a test failing in its rerun too did not block the push' >&2
    exit 1
fi
grep -q 'DUMMY-OUTPUT-rerun' "$tmp/test-output"
grep -q 'macOS gate: TESTS FAILED' "$tmp/test-output"
assert_clean

# A git older than 2.31 echoes --path-format=absolute back instead of
# applying it (W25 review N6): the history still lands in the common dir.
mkdir -p "$tmp/oldgit"
real_git="$(command -v git)"
cat > "$tmp/oldgit/git" <<MOCK
#!/bin/bash
args=()
for arg; do
    if [ "\$arg" = --path-format=absolute ]; then printf '%s\n' "\$arg"; else args+=("\$arg"); fi
done
exec "$real_git" "\${args[@]}"
MOCK
chmod +x "$tmp/oldgit/git"
before="$(grep -c . "$repo/.git/nostrc-macos-gate-history/reruns")"
PATH="$tmp/oldgit:$PATH" run_hook flaky > "$tmp/oldgit-output" 2>&1
[ "$(grep -c . "$repo/.git/nostrc-macos-gate-history/reruns")" -eq $((before + 1)) ]
absent 'path-format' "$tmp/oldgit-output"
[ ! -e "$branch/--path-format=absolute" ]
assert_clean

for stage in beads configure build test linux sanitizer; do
    if run_hook "$stage"; then
        echo "$stage failure did not block the push" >&2
        exit 1
    fi
    if [ "$stage" = beads ]; then
        absent '^CONFIGURE ' "$tmp/trace"
    fi
    assert_clean
done

# A checkout that rewrites the running hook mid-push must not change what runs.
cp "$branch/scripts/pre-push" "$tmp/pre-push.orig"
SPLICE="$branch/scripts/pre-push" run_hook
absent '^SPLICED$' "$tmp/trace"
grep -q 'SPLICED' "$branch/scripts/pre-push"
cp "$tmp/pre-push.orig" "$branch/scripts/pre-push"
assert_clean

# A Linux failure that is in by the end of the host build blocks the push
# before the host test run.
if BUILD_SLEEP=3 run_hook linux > "$tmp/linux-output" 2>&1; then
    echo 'Linux failure did not block the push' >&2
    exit 1
fi
grep -q 'Linux build failed' "$tmp/linux-output"
absent '^CTEST ' "$tmp/trace"
assert_clean

# So does a sanitizer failure (a leak report), named as one; its tests ran
# after the host build.
if BUILD_SLEEP=3 run_hook sanitizer > "$tmp/sanitizer-output" 2>&1; then
    echo 'Sanitizer failure did not block the push' >&2
    exit 1
fi
grep -q 'Sanitizer gate failed' "$tmp/sanitizer-output"
awk '/^BUILD /{b=NR} /^SANITIZER_TESTS$/{t=NR} END{exit !(b && t && b < t)}' "$tmp/trace"
assert_clean

# A host failure stops the Linux stage instead of waiting for it.
start=$SECONDS
if LINUX_SLEEP=60 run_hook build; then
    echo 'build failure did not block the push' >&2
    exit 1
fi
[ $((SECONDS - start)) -lt 30 ]
assert_clean

# Without Docker the push is blocked, unless the Linux stage is skipped, which
# is announced.
if DOCKER_DOWN=1 run_hook > "$tmp/no-docker-output" 2>&1; then
    echo 'A push without Docker was accepted' >&2
    exit 1
fi
grep -q 'needs Docker' "$tmp/no-docker-output"
grep -q 'NOSTRC_SKIP_LINUX_GATE=1' "$tmp/no-docker-output"
absent '^CONFIGURE ' "$tmp/trace"
assert_clean
DOCKER_DOWN=1 NOSTRC_SKIP_LINUX_GATE=1 run_hook > "$tmp/skip-output" 2>&1
grep -q 'Linux build SKIPPED (NOSTRC_SKIP_LINUX_GATE=1)' "$tmp/skip-output"
grep -q 'sanitizer stage SKIPPED too' "$tmp/skip-output"
grep -q '^CTEST ' "$tmp/trace"
absent '^DOCKER_RUN ' "$tmp/trace"
assert_clean

# NOSTRC_SKIP_SANITIZER_GATE=1 skips the sanitizer stage alone, loudly.
NOSTRC_SKIP_SANITIZER_GATE=1 run_hook > "$tmp/skip-san-output" 2>&1
grep -q 'SANITIZER STAGE SKIPPED (NOSTRC_SKIP_SANITIZER_GATE=1)' "$tmp/skip-san-output"
grep -q '^DOCKER_RUN linux ' "$tmp/trace"
absent '^DOCKER_RUN sanitizer ' "$tmp/trace"
grep -q '^CTEST ' "$tmp/trace"
assert_clean

# The candidate's own static checks run first and block the push.
mkdir -p "$branch/scripts"
cat > "$branch/scripts/check-linux-ci-packages.py" <<'CHECK'
import os, sys
open(os.environ["TRACE"], "a").write("CHECK %s\n" % " ".join(sys.argv[1:]))
sys.exit(1 if os.environ.get("FAIL_STAGE") == "check" else 0)
CHECK
git -C "$branch" add scripts/check-linux-ci-packages.py
git -C "$branch" commit -qm "static check"
base="$candidate"
candidate="$(git -C "$branch" rev-parse HEAD)"
run_hook
grep -q '^CHECK --root .*/checkout$' "$tmp/trace"
if run_hook check; then
    echo 'static check failure did not block the push' >&2
    exit 1
fi
absent '^CONFIGURE ' "$tmp/trace"
assert_clean

printf 'unrelated\n' > "$branch/unrelated"
git -C "$branch" add unrelated
git -C "$branch" commit -qm unrelated
base="$candidate"
candidate="$(git -C "$branch" rev-parse HEAD)"
run_hook > "$tmp/unrelated-output" 2>&1
grep -q 'BUILD_GROUNDHOG=OFF' "$tmp/trace"
# Nothing the sanitizer job tests changed: that stage is skipped, and says why.
grep -q 'sanitizer stage skipped for refs/heads/candidate: none of its 1 changed file(s) is under' "$tmp/unrelated-output"
grep -q '^DOCKER_RUN linux ' "$tmp/trace"
absent '^DOCKER_RUN sanitizer ' "$tmp/trace"
assert_clean

# Each new commit changes only a Groundhog dependency/config input, not
# Groundhog itself. Every one is also an input of the sanitizer build (or the
# job, or its image), so the sanitizer stage runs for each.
for path in CMakeLists.txt cmake/BuildConfig.cmake gnome/seahorse/secret_store.c \
    libnostr/src/nostr-event.c nostr-gobject/src/nostr_relay.c \
    nips/nip19/src/nip19.c nips/nip55l/dbus/org.nostr.Signer.xml \
    apps/gnostr/data/schemas/org.gnostr.gnostr.gschema.xml \
    .github/workflows/groundhog-ci.yml; do
    mkdir -p "$branch/$(dirname "$path")"
    # Appended: the workflow stays readable by the sanitizer stage.
    printf '# dependency change\n' >> "$branch/$path"
    git -C "$branch" add "$path"
    git -C "$branch" commit -qm "change $path"
    base="$candidate"
    candidate="$(git -C "$branch" rev-parse HEAD)"
    run_hook > "$tmp/dep-output" 2>&1
    grep -q 'BUILD_GROUNDHOG=ON' "$tmp/trace"
    grep -q '^DOCKER_RUN sanitizer ' "$tmp/trace"
    grep -q 'sanitizer stage runs: the range touches' "$tmp/dep-output"
    assert_clean
done
# Inputs of the sanitizer build that are not Groundhog's own inputs, and its
# image: the sanitizer stage runs for them too.
for path in libjson/src/json.c components/nostrdb/src/nostrdb_storage.c \
    third_party/nsync/internal/mu.c NipOptions.cmake scripts/linux-ci.Dockerfile; do
    mkdir -p "$branch/$(dirname "$path")"
    printf '# dependency change\n' >> "$branch/$path"
    git -C "$branch" add "$path"
    git -C "$branch" commit -qm "change $path"
    base="$candidate"
    candidate="$(git -C "$branch" rev-parse HEAD)"
    run_hook > "$tmp/dep-output" 2>&1
    grep -q '^DOCKER_RUN sanitizer ' "$tmp/trace"
    assert_clean
done

# A range git cannot compute (a force push over a remote tip never fetched)
# runs the stage: it must not read as "no files changed".
printf 'unrelated 2\n' > "$branch/unrelated"
git -C "$branch" add unrelated
git -C "$branch" commit -qm "unrelated 2"
candidate="$(git -C "$branch" rev-parse HEAD)"
base=1234567890123456789012345678901234567890
run_hook > "$tmp/unknown-output" 2>&1
grep -q 'sanitizer stage runs: git cannot list what refs/heads/candidate changes since 1234567890123456789012345678901234567890' "$tmp/unknown-output"
grep -q '^DOCKER_RUN sanitizer ' "$tmp/trace"
absent 'sanitizer stage skipped' "$tmp/unknown-output"
assert_clean
base="$(git -C "$branch" rev-parse HEAD~1)"

# A new branch (no remote oid) is filtered against its merge base with
# origin/master: a docs-only branch skips the sanitizer stage.
git -C "$repo" update-ref refs/remotes/origin/master "$candidate"
mkdir -p "$branch/docs"
printf 'notes\n' > "$branch/docs/notes.md"
git -C "$branch" add docs/notes.md
git -C "$branch" commit -qm docs
candidate="$(git -C "$branch" rev-parse HEAD)"
base=0000000000000000000000000000000000000000
run_hook > "$tmp/docs-output" 2>&1
grep -q 'sanitizer stage skipped for refs/heads/candidate: none of its 1 changed file(s)' "$tmp/docs-output"
absent '^DOCKER_RUN sanitizer ' "$tmp/trace"
assert_clean

echo 'pre-push tests passed'
