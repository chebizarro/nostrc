#!/usr/bin/env bash
# Build the fipsd Debian packages (fipsd, fips-gateway, libfipsnative0, libfipsnative-dev)
# from this checkout, then check them.
#
#   fipsd/packaging/debian/build-deb.sh [--output-dir DIR] [--version DEBVERSION]
#                                      [--distro-tag TAG] [--no-check]
#
# dpkg-buildpackage wants debian/ at the source root, where the headless login
# stack's recipe already lives, so this copies the tree (tracked files plus
# the submodules) to a scratch directory and puts fipsd/packaging/debian
# there as debian/ -- the shape of scripts/build-desktop-deb.sh. The copy has
# no .git, so the tree's revision is passed in through FIPSD_GIT_REV (see
# debian/rules) and the packaged `fipsd -V` still names the commit.
#
# The Debian version comes from fipsd/packaging/common/version.sh --deb:
# <declared>-1 at a fipsd-v<declared> tag, otherwise <declared>~git<date>.<sha>-1.
# --distro-tag appends ~<TAG> (one deb per distribution release, as the
# Groundhog release names them: fipsd_0.1.0-1~ubuntu24.04_arm64.deb);
# --version replaces the whole thing.
#
# After the build, unless --no-check: fipsd/packaging/check-deb-depends.sh,
# check-glibc-floor.sh and check-deb-revision.sh run over the packages. Needs
# the Build-Depends from fipsd/packaging/debian/control
# (`apt build-dep ./` inside the copy, or mk-build-deps).
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
out=$root/build-deb
version=""
distro_tag=""
check=1
while [ $# -gt 0 ]; do
    case "$1" in
        --output-dir) out=${2:?missing value for --output-dir}; shift 2 ;;
        --version) version=${2:?missing value for --version}; shift 2 ;;
        --distro-tag) distro_tag=${2:?missing value for --distro-tag}; shift 2 ;;
        --no-check) check=0; shift ;;
        -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "build-deb: unknown option $1" >&2; exit 2 ;;
    esac
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
src=$work/fipsd
mkdir -p "$src" "$out"
out=$(cd "$out" && pwd)

# The tree's revision, the way fipsd/cmake/BuildInfo.cmake reads it: the
# 10-character HEAD hash, -dirty when tracked files under fipsd/ differ.
head=""
state=none
rev=""
if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "$root" ls-files -z --recurse-submodules | (cd "$root" && tar --null -T - -cf -) | tar -xf - -C "$src"
    head=$(git -C "$root" rev-parse HEAD)
    rev=$(git -C "$root" rev-parse --short=10 HEAD)
    if git -C "$root" diff --quiet HEAD -- fipsd; then
        state=clean
    else
        state=dirty
        rev="$rev-dirty"
    fi
else
    # A source archive: no git, take the directory as it is.
    tar -C "$root" --exclude=./build --exclude='./build-*' --exclude=./debian -cf - . | tar -xf - -C "$src"
fi
[ -n "$(ls -A "$src/third_party/nostrdb" 2>/dev/null)" ] || {
    echo "build-deb: third_party/nostrdb is empty; run git submodule update --init third_party/nostrdb" >&2; exit 1; }

[ -n "$version" ] || version=$(sh "$root/fipsd/packaging/common/version.sh" --deb "$root")
[ -z "$distro_tag" ] || version="$version~$distro_tag"

rm -rf "$src/debian"
cp -r "$src/fipsd/packaging/debian" "$src/debian"
# The changelog's top entry carries the declared release version; this build
# names what it is actually building.
sed -i "1s/(\([^)]*\))/($version)/" "$src/debian/changelog"
echo "build-deb: version $version, revision ${rev:-none}"

(cd "$src" && FIPSD_GIT_REV="$rev" dpkg-buildpackage -b -us -uc)
cp "$work"/*.deb "$out"/
cp "$work"/*.buildinfo "$work"/*.changes "$out"/ 2>/dev/null || true
ls -1 "$out"/*.deb

if [ "$check" -eq 1 ]; then
    checks=$root/fipsd/packaging
    fipsd_deb=$(ls "$out"/fipsd_"${version}"_*.deb)
    "$checks/check-deb-depends.sh" "$out"/*_"${version}"_*.deb
    "$checks/check-glibc-floor.sh" "$out"/*_"${version}"_*.deb
    (cd "$root" && "$checks/check-deb-revision.sh" "$fipsd_deb" "$head" "$state")
fi
