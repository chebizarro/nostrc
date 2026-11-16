#!/usr/bin/env bash
# Build the fipsd RPMs on a host with rpmbuild (Fedora, or a Fedora container
# -- see build-rpm-container.sh).
#
#   fipsd/packaging/rpm/build-rpm.sh [--output-dir DIR] [--version V]
#
# Writes the source archive with scripts/make-desktop-source-archive.sh (the
# fipsd name), points the spec at it and runs rpmbuild -ba. The spec's
# optional payload (fipsctl, the fips-dns helpers) is detected from the tree
# and passed as defines, since %files cannot be conditional on what the
# build produced. The tree's revision goes in through fipsd_git_rev.
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
out=$root/build-rpm
version=""
while [ $# -gt 0 ]; do
    case "$1" in
        --output-dir) out=${2:?missing value for --output-dir}; shift 2 ;;
        --version) version=${2:?missing value for --version}; shift 2 ;;
        -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "build-rpm: unknown option $1" >&2; exit 2 ;;
    esac
done
command -v rpmbuild >/dev/null 2>&1 || { echo "build-rpm: rpmbuild is not installed" >&2; exit 2; }
[ -n "$version" ] || version=$(sh "$root/fipsd/packaging/common/version.sh" "$root")
mkdir -p "$out"
out=$(cd "$out" && pwd)

topdir=$(mktemp -d)
trap 'rm -rf "$topdir"' EXIT
mkdir -p "$topdir"/{SOURCES,SPECS,RPMS,SRPMS,BUILD}
"$root/scripts/make-desktop-source-archive.sh" "$version" "$topdir/SOURCES" fipsd
sed "s/^%global upstream_version .*/%global upstream_version $version/" \
    "$root/fipsd/packaging/rpm/fipsd.spec" > "$topdir/SPECS/fipsd.spec"

defines=()
rev=""
if git -C "$root" rev-parse --verify --quiet HEAD >/dev/null 2>&1; then
    rev=$(git -C "$root" rev-parse --short=10 HEAD)
    git -C "$root" diff --quiet HEAD -- fipsd || rev="$rev-dirty"
    defines+=(--define "fipsd_git_rev $rev")
fi
[ -f "$root/fipsd/packaging/common/fips-dns-setup" ] && defines+=(--define "_fipsd_has_dns_helpers 1")
grep -q 'add_executable(fipsctl' "$root"/fipsd/CMakeLists.txt "$root"/fipsd/src/fipsctl/CMakeLists.txt 2>/dev/null \
    && defines+=(--define "_fipsd_has_fipsctl 1")

echo "build-rpm: version $version, revision ${rev:-none}"
rpmbuild -ba --define "_topdir $topdir" "${defines[@]}" "$topdir/SPECS/fipsd.spec"
cp "$topdir"/RPMS/*/*.rpm "$topdir"/SRPMS/*.rpm "$out"/
ls -1 "$out"/*.rpm
