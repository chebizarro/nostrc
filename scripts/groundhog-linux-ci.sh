#!/usr/bin/env bash
#
# groundhog-linux-ci.sh — run Groundhog's hosted Linux CI job locally in Docker.
#
# Mirrors the `groundhog` job of .github/workflows/groundhog-ci.yml: an Ubuntu
# 24.04 image with the job's apt packages, the job's CMake configure, and the
# tests run under `dbus-run-session -- xvfb-run` (a private session bus and an
# X server with no window manager). Use it to reproduce Linux/Xvfb-only GUI
# failures before pushing (nostrc-qp24.88).
#
# Usage (from the repository root, submodules initialised):
#   scripts/groundhog-linux-ci.sh                    # build, run the CI test set once
#   REPEAT=3 scripts/groundhog-linux-ci.sh           # ... three times
#   scripts/groundhog-linux-ci.sh -R groundhog-account-ui -V   # custom ctest args
#   scripts/groundhog-linux-ci.sh --shell            # interactive shell in the container
#
# Env: IMAGE (default groundhog-linux-ci), BUILD_VOLUME (default
# groundhog-linux-ci-build; a named volume keeps the Linux build out of the
# source tree), JOBS (default: container CPUs), REPEAT (default 1).
# The container runs as an unprivileged user, as the hosted runner does.
set -euo pipefail

IMAGE="${IMAGE:-groundhog-linux-ci}"
BUILD_VOLUME="${BUILD_VOLUME:-groundhog-linux-ci-build}"
REPEAT="${REPEAT:-1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

docker build -q -t "$IMAGE" - <<'DOCKERFILE' >/dev/null
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
# Keep this list in step with groundhog-ci.yml's "Install build and display dependencies".
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    libgtk-4-dev libadwaita-1-dev libglib2.0-dev libsecret-1-dev \
    libjansson-dev libsecp256k1-dev libwebsockets-dev libsodium-dev \
    libssl-dev libcurl4-openssl-dev libsoup-3.0-dev libjson-glib-dev glib-networking \
    libgit2-dev libsqlite3-dev libnsync-dev libsqlcipher-dev \
    desktop-file-utils appstream xvfb xauth dbus-bin at-spi2-core \
    blueprint-compiler gnome-keyring adwaita-icon-theme librsvg2-common \
    git python3 ca-certificates gdb \
 && rm -rf /var/lib/apt/lists/*
RUN useradd -m -u 1001 ci && install -d -o ci -g ci /build
USER ci
DOCKERFILE

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
