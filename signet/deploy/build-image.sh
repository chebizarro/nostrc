#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
# shellcheck source=versions.env
source "$here/versions.env"
[[ $DEBIAN_IMAGE =~ @sha256:[0-9a-f]{64}$ && $CADDY_IMAGE =~ @sha256:[0-9a-f]{64}$ ]]
[[ $CMAKE_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ && $MESON_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
variant=${1:?usage: build-image.sh ON|OFF [docker buildx build options]}
shift
[[ $variant == ON || $variant == OFF ]]
exec docker buildx build --platform linux/amd64 -f "$root/signet/Dockerfile" \
  --build-arg "DEBIAN_IMAGE=$DEBIAN_IMAGE" --build-arg "CMAKE_VERSION=$CMAKE_VERSION" \
  --build-arg "MESON_VERSION=$MESON_VERSION" --build-arg "SIGNET_PASSKEYS=$variant" \
  --build-arg "REVISION=$(git -C "$root" rev-parse HEAD)" \
  --build-arg "VERSION=${SIGNET_BUILD_VERSION:-sha-$(git -C "$root" rev-parse HEAD)}" \
  --build-arg "SOURCE_URL=${SIGNET_SOURCE_URL:-https://github.com/chebizarro/nostrc}" \
  --build-arg "CREATED=$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$@" "$root"
