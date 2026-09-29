#!/usr/bin/env bash
#
# groundhog-linux-ci.sh — run Groundhog's hosted Linux CI job locally in Docker.
#
# Mirrors the `groundhog` job of .github/workflows/groundhog-ci.yml: an Ubuntu
# 24.04 image with (at least) the job's apt packages (scripts/linux-ci.Dockerfile),
# the job's CMake configure, and the tests run under
# `dbus-run-session -- xvfb-run` (a private session bus and an X server with no
# window manager). Use it to reproduce Linux/Xvfb-only GUI
# failures before pushing (nostrc-qp24.88).
#
# Usage (from the repository root, submodules initialised):
#   scripts/groundhog-linux-ci.sh                    # build, run the CI test set once
#   REPEAT=3 scripts/groundhog-linux-ci.sh           # ... three times
#   scripts/groundhog-linux-ci.sh -R groundhog-account-ui -V   # custom ctest args
#   scripts/groundhog-linux-ci.sh --shell            # interactive shell in the container
#
# Env: IMAGE (default nostrc-linux-ci), BUILD_VOLUME (default
# groundhog-linux-ci-build; a named volume keeps the Linux build out of the
# source tree), JOBS (default: container CPUs), REPEAT (default 1).
# The container runs as an unprivileged user, as the hosted runner does.
set -euo pipefail

IMAGE="${IMAGE:-nostrc-linux-ci}"
BUILD_VOLUME="${BUILD_VOLUME:-groundhog-linux-ci-build}"
REPEAT="${REPEAT:-1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# The image is shared with the pre-push gate's Linux stage (scripts/linux-gate.sh);
# its package list is a superset of this job's, checked against the workflows by
# scripts/check-linux-ci-packages.py.
docker build -q -t "$IMAGE" - < "$ROOT/scripts/linux-ci.Dockerfile" >/dev/null

if [ "${1:-}" = "--shell" ]; then
  set -- ; CMD='exec bash'
else
  [ $# -gt 0 ] || set -- -R '^(groundhog-|nostrc-test-bus-selftest$)'
  # "$@" (the ctest arguments) is expanded inside the container.
  CMD='set -euo pipefail
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DBUILD_GROUNDHOG=ON -DBUILD_NOSTR_GOBJECT=ON -DBUILD_NOSTR_GTK=OFF \
  -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF -DSIGNET_ENABLE=OFF >/dev/null
cmake --build /build --parallel "${JOBS:-$(nproc)}" >/build/build.log 2>&1 ||
  { tail -60 /build/build.log; exit 1; }
for i in $(seq 1 "$REPEAT"); do
  echo "==> Groundhog ctest run $i/$REPEAT"
  dbus-run-session -- xvfb-run -a -s "-screen 0 1280x800x24" \
    ctest --test-dir /build --output-on-failure "$@"
done'
fi

TTY=; if [ -t 0 ]; then TTY=-it; fi
# shellcheck disable=SC2086  # $TTY is empty or one flag
exec docker run --rm $TTY -e JOBS="${JOBS:-}" -e REPEAT="$REPEAT" \
  -v "$ROOT:/src" -v "$BUILD_VOLUME:/build" -w /src "$IMAGE" bash -c "$CMD" bash "$@"
