#!/usr/bin/env bash
# Build the fipsd RPMs in the Fedora image the fips-packaging workflow uses.
#
#   fipsd/packaging/rpm/build-rpm-container.sh [--image IMAGE] [--output-dir DIR]
#
# The image defaults to FIPSD_RPM_BUILD_IMAGE in fipsd/packaging/build-floor.env.
# The tree is mounted read-only at /src, `dnf builddep` installs the spec's
# BuildRequires, and build-rpm.sh runs as it would on a Fedora host. rpmbuild
# and systemd-rpm-macros come from the image: without the macros the spec's
# %systemd_post would not expand and the package would ship scriptlets that
# quietly do nothing. Needs docker or podman.
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck disable=SC1091  # fipsd/packaging/build-floor.env
. "$root/fipsd/packaging/build-floor.env"
image=$FIPSD_RPM_BUILD_IMAGE
out=$root/build-rpm
while [ $# -gt 0 ]; do
    case "$1" in
        --image) image=${2:?missing value for --image}; shift 2 ;;
        --output-dir) out=${2:?missing value for --output-dir}; shift 2 ;;
        -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "build-rpm-container: unknown option $1" >&2; exit 2 ;;
    esac
done
mkdir -p "$out"
out=$(cd "$out" && pwd)

engine=""
for candidate in docker podman; do
    if command -v "$candidate" >/dev/null 2>&1; then engine=$candidate; break; fi
done
[ -n "$engine" ] || { echo "build-rpm-container: neither docker nor podman is installed" >&2; exit 2; }

"$engine" run --rm -v "$root:/src:ro" -v "$out:/out" "$image" bash -euxc '
    dnf install -y -q rpm-build rpmdevtools dnf-plugins-core git
    git config --global --add safe.directory "*"
    dnf builddep -y -q /src/fipsd/packaging/rpm/fipsd.spec
    /src/fipsd/packaging/rpm/build-rpm.sh --output-dir /out
'
ls -1 "$out"/*.rpm
