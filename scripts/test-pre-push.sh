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
cat > "$tmp/bin/ctest" <<'MOCK'
#!/bin/bash
set -eu
printf 'CTEST %s\n' "$*" >> "$TRACE"
if [ "${3:-}" = -N ]; then
    printf 'Total Tests: 1\n'
    exit 0
fi
[ "${FAIL_STAGE:-}" != test ]
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
        printf 'DOCKER_RUN %s\n' "$*" >> "$TRACE"
        src=""
        for arg; do case "$arg" in *:/src:ro) src="${arg%:/src:ro}" ;; esac; done
        # The container gets its own read-only checkout of the candidate.
        [ -n "$src" ] && [ "$src" != "$MAIN" ]
        case "$src" in */checkout) exit 1 ;; esac
        grep -qx candidate "$src/marker"
        sleep "${LINUX_SLEEP:-0}"
        [ "${FAIL_STAGE:-}" != linux ] ;;
    *) exit 1 ;;
esac
MOCK
chmod +x "$tmp/bin/cmake" "$tmp/bin/ctest" "$tmp/docker-bin/docker"

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
grep -q '^DOCKER_RUN .*/linux-src:/src:ro' "$tmp/trace"
! grep -q 'SKIPPED' "$tmp/output"
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
! grep -q '^CONFIGURE ' "$tmp/trace"
assert_clean

# Real CMake/CTest with no add_test calls returns success; the gate must reject it.
if run_hook "" 0 real > "$tmp/no-tests-output" 2>&1; then
    echo 'Zero registered CTest tests were accepted' >&2
    exit 1
fi
grep -q 'No registered CTest tests.*count=0' "$tmp/no-tests-output"
assert_clean

for stage in beads configure build test linux; do
    if run_hook "$stage"; then
        echo "$stage failure did not block the push" >&2
        exit 1
    fi
    if [ "$stage" = beads ]; then
        ! grep -q '^CONFIGURE ' "$tmp/trace"
    fi
    assert_clean
done

# A checkout that rewrites the running hook mid-push must not change what runs.
cp "$branch/scripts/pre-push" "$tmp/pre-push.orig"
SPLICE="$branch/scripts/pre-push" run_hook
! grep -q '^SPLICED$' "$tmp/trace"
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
! grep -q '^CTEST ' "$tmp/trace"
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
! grep -q '^CONFIGURE ' "$tmp/trace"
assert_clean
DOCKER_DOWN=1 NOSTRC_SKIP_LINUX_GATE=1 run_hook > "$tmp/skip-output" 2>&1
grep -q 'Linux build SKIPPED (NOSTRC_SKIP_LINUX_GATE=1)' "$tmp/skip-output"
grep -q '^CTEST ' "$tmp/trace"
! grep -q '^DOCKER_RUN ' "$tmp/trace"
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
! grep -q '^CONFIGURE ' "$tmp/trace"
assert_clean

printf 'unrelated\n' > "$branch/unrelated"
git -C "$branch" add unrelated
git -C "$branch" commit -qm unrelated
base="$candidate"
candidate="$(git -C "$branch" rev-parse HEAD)"
run_hook
grep -q 'BUILD_GROUNDHOG=OFF' "$tmp/trace"
assert_clean

# Each new commit changes only a Groundhog dependency/config input, not Groundhog itself.
for path in CMakeLists.txt cmake/BuildConfig.cmake gnome/seahorse/secret_store.c \
    libnostr/src/nostr-event.c nostr-gobject/src/nostr_relay.c \
    nips/nip19/src/nip19.c nips/nip55l/dbus/org.nostr.Signer.xml \
    apps/gnostr/data/schemas/org.gnostr.gnostr.gschema.xml \
    .github/workflows/groundhog-ci.yml; do
    mkdir -p "$branch/$(dirname "$path")"
    printf 'dependency change\n' > "$branch/$path"
    git -C "$branch" add "$path"
    git -C "$branch" commit -qm "change $path"
    base="$candidate"
    candidate="$(git -C "$branch" rev-parse HEAD)"
    run_hook
    grep -q 'BUILD_GROUNDHOG=ON' "$tmp/trace"
    assert_clean
done

echo 'pre-push tests passed'
