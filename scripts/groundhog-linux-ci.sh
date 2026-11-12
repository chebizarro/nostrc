#!/usr/bin/env bash
# Run Groundhog Linux CI locally in the shared Ubuntu 24.04 image.
# SANITIZERS=1 reads the hosted sanitizer recipe (targets, configure, environment
# and default CTest selection) rather than maintaining another allowlist.
# Usage: scripts/groundhog-linux-ci.sh [ctest arguments | --shell]
#        SANITIZERS=1 scripts/groundhog-linux-ci.sh
#        SANITIZERS=1 scripts/groundhog-linux-ci.sh -R '^groundhog-preferences$' -V
# Env: IMAGE (nostrc-linux-ci), BUILD_VOLUME (groundhog-linux-ci-build), JOBS,
# REPEAT (1), SANITIZERS (0), optional BUILDER (a private buildx builder),
# CONTAINER_NAME and RESOURCE_LABEL (org.nostrc.groundhog-ci=1).
set -euo pipefail
IMAGE="${IMAGE:-nostrc-linux-ci}"
BUILD_VOLUME="${BUILD_VOLUME:-groundhog-linux-ci-build}"
REPEAT="${REPEAT:-1}"
SANITIZERS="${SANITIZERS:-0}"
RESOURCE_LABEL="${RESOURCE_LABEL:-org.nostrc.groundhog-ci=1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[[ "$SANITIZERS" = 0 || "$SANITIZERS" = 1 ]] || { echo 'SANITIZERS must be 0 or 1' >&2; exit 2; }

build=(docker build)
if [[ -n "${BUILDER:-}" ]]; then
  build=(docker buildx build --builder "$BUILDER" --load)
fi
"${build[@]}" -q --label "$RESOURCE_LABEL" -t "$IMAGE" \
  - < "$ROOT/scripts/linux-ci.Dockerfile" >/dev/null
docker volume create --label "$RESOURCE_LABEL" "$BUILD_VOLUME" >/dev/null

if [[ "${1:-}" = --shell ]]; then
  set --; CMD='exec bash'
else
  CMD='set -euo pipefail
if [[ "$SANITIZERS" = 1 ]]; then
  eval "$(python3 /src/scripts/sanitizer-gate-ci.py config --source-root /src)"
  env "${GH_SAN_CONFIGURE_ENV[@]}" cmake -S /src -B /build "${GH_SAN_CONFIGURE[@]}"
  env "${GH_SAN_BUILD_ENV[@]}" cmake --build /build --parallel "${JOBS:-$(nproc)}" \
    --target "${GH_SAN_TARGETS[@]}"
  export "${GH_SAN_ENV[@]}"
  if [[ $# = 0 ]]; then
    regex="^($(IFS="|"; echo "${GH_SAN_TESTS[*]}"))$"
    expected=$(printf "%s\n" "${GH_SAN_TESTS[@]}" | sort)
    registered=$(ctest --test-dir /build -N -R "$regex" | sed -n "s/^ *Test *#[0-9]*: *//p" | sort)
    missing=$(comm -23 <(echo "$expected") <(echo "$registered"))
    [[ -z "$missing" ]] || { echo "Missing sanitizer tests: $missing" >&2; exit 1; }
    set -- -R "$regex" --parallel "$GH_SAN_TEST_JOBS"
  fi
else
  cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON -DBUILD_GROUNDHOG=ON -DBUILD_NOSTR_GOBJECT=ON -DBUILD_NOSTR_GTK=OFF \
    -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF -DSIGNET_ENABLE=OFF \
    -DGNOSTR_ENABLE_ASAN=OFF -DGNOSTR_ENABLE_UBSAN=OFF
  cmake --build /build --parallel "${JOBS:-$(nproc)}"
  [[ $# != 0 ]] || set -- -R "^(groundhog-|nostrc-test-bus-selftest$)"
fi
for i in $(seq 1 "$REPEAT"); do
  echo "==> Groundhog ctest run $i/$REPEAT (sanitizers=$SANITIZERS)"
  dbus-run-session -- xvfb-run -a -s "-screen 0 1280x800x24" \
    ctest --test-dir /build --output-on-failure --no-tests=error "$@" | tee /build/groundhog-local-ctest.log
  if [[ "$SANITIZERS" = 1 ]] && grep -Eq "$GH_SAN_SKIP_PATTERN" /build/groundhog-local-ctest.log; then
    echo "A selected sanitizer test was skipped or did not run" >&2
    exit 1
  fi
done'
fi

args=(--rm --label "$RESOURCE_LABEL")
[[ -z "${CONTAINER_NAME:-}" ]] || args+=(--name "$CONTAINER_NAME")
[[ ! -t 0 ]] || args+=(-it)
exec docker run "${args[@]}" -e JOBS="${JOBS:-}" -e REPEAT="$REPEAT" -e SANITIZERS="$SANITIZERS" \
  -v "$ROOT:/src:ro" -v "$BUILD_VOLUME:/build" -w /src "$IMAGE" bash -c "$CMD" bash "$@"
