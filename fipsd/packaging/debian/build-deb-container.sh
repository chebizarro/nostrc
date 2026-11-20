#!/usr/bin/env bash
# Build the fipsd Debian packages in the distribution image they are for.
#
#   fipsd/packaging/debian/build-deb-container.sh [--image IMAGE] [--output-dir DIR]
#
# The image defaults to FIPSD_DEB_BUILD_IMAGE in fipsd/packaging/build-floor.env
# (the release the fips-packaging workflow builds and install-tests on); pass
# --image ubuntu:26.04 or debian:13 for another. The tree is mounted read-only
# at /src, the build dependencies are installed from debian/control with
# mk-build-deps, and build-deb.sh runs as it would on the host, so a package
# built here is built the way the shipped one is. Needs docker or podman.
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck disable=SC1091  # fipsd/packaging/build-floor.env
. "$root/fipsd/packaging/build-floor.env"
image=$FIPSD_DEB_BUILD_IMAGE
out=$root/build-deb
while [ $# -gt 0 ]; do
    case "$1" in
        --image) image=${2:?missing value for --image}; shift 2 ;;
        --output-dir) out=${2:?missing value for --output-dir}; shift 2 ;;
        -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "build-deb-container: unknown option $1" >&2; exit 2 ;;
    esac
done
mkdir -p "$out"
out=$(cd "$out" && pwd)

engine=""
for candidate in docker podman; do
    if command -v "$candidate" >/dev/null 2>&1; then engine=$candidate; break; fi
done
[ -n "$engine" ] || { echo "build-deb-container: neither docker nor podman is installed" >&2; exit 2; }

# The image's distribution names the package, as in the release workflow.
distro_tag=$(printf '%s' "$image" | tr -d ':' | tr -c 'a-z0-9.\n' '.')

# shellcheck disable=SC2016  # $DISTRO_TAG expands inside the container
"$engine" run --rm \
    -v "$root:/src:ro" \
    -v "$out:/out" \
    -e "DISTRO_TAG=$distro_tag" \
    "$image" bash -euxc '
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends devscripts equivs git ca-certificates >/dev/null
        git config --global --add safe.directory "*"
        mk-build-deps -i -r -t "apt-get -y -qq --no-install-recommends" /src/fipsd/packaging/debian/control
        /src/fipsd/packaging/debian/build-deb.sh --output-dir /out --distro-tag "$DISTRO_TAG"
    '
ls -1 "$out"/*.deb
