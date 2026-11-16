#!/bin/sh
# Print fipsd's declared version, the single source every package reads.
#
#   fipsd/packaging/common/version.sh [--deb] [ROOT]
#
# The source is declare_component_version(FIPSD M m p) in fipsd/CMakeLists.txt,
# the entry VERSION_MANIFEST.md names as authoritative and the value
# scripts/tag_release.py reads from a configured build to create the
# fipsd-v<M.m.p> tag. --deb prints the Debian version for the current tree:
# <M.m.p>-1 at a fipsd-v<M.m.p> tag, otherwise <M.m.p>~git<YYYYMMDD>.<sha>-1,
# which sorts before the release the tree is heading for, and ".dirty" is
# added when tracked files under fipsd/ differ from HEAD. Without git (an
# exported tree) the Debian version is <M.m.p>-1.
set -eu

deb=0
if [ "${1:-}" = "--deb" ]; then deb=1; shift; fi
root=${1:-$(cd "$(dirname "$0")/../../.." && pwd)}
cmake_file="$root/fipsd/CMakeLists.txt"

version=$(sed -n 's/^declare_component_version(FIPSD \([0-9][0-9]*\) \([0-9][0-9]*\) \([0-9][0-9]*\))$/\1.\2.\3/p' "$cmake_file")
if [ -z "$version" ]; then
    echo "version.sh: no declare_component_version(FIPSD M m p) in $cmake_file" >&2
    exit 1
fi
if [ "$deb" -eq 0 ]; then
    printf '%s\n' "$version"
    exit 0
fi

if ! git -C "$root" rev-parse --verify --quiet HEAD >/dev/null 2>&1; then
    printf '%s-1\n' "$version"
    exit 0
fi
if git -C "$root" describe --tags --exact-match HEAD 2>/dev/null | grep -qx "fipsd-v$version"; then
    printf '%s-1\n' "$version"
    exit 0
fi
date=$(git -C "$root" log -1 --format=%cs | tr -d -)
sha=$(git -C "$root" rev-parse --short=10 HEAD)
dirty=""
git -C "$root" diff --quiet HEAD -- fipsd 2>/dev/null || dirty=.dirty
printf '%s~git%s.%s%s-1\n' "$version" "$date" "$sha" "$dirty"
