#!/usr/bin/env bash
# Build the desktop .deb packages (groundhog, grotto) from this checkout.
#
#   scripts/build-desktop-deb.sh [OUTPUT_DIR]
#
# dpkg-buildpackage wants debian/ at the source root, where the headless
# login stack's recipe already lives, so this copies the tree (tracked files
# plus the nostrdb and nsync submodules) to a scratch directory and puts
# packaging/debian-desktop there as debian/. Needs the Build-Depends from
# packaging/debian-desktop/control (apt build-dep ./ inside the copy).
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/build-deb}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
src=$work/groundhog

mkdir -p "$src" "$out"
if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git -C "$root" ls-files -z --recurse-submodules | (cd "$root" && tar --null -T - -cf -) | tar -xf - -C "$src"
else
  # A source archive: no git, take the directory as it is.
  tar -C "$root" --exclude=./build --exclude='./build-*' --exclude=./debian -cf - . | tar -xf - -C "$src"
fi
for sub in third_party/nostrdb third_party/nsync; do
  [ -n "$(ls -A "$src/$sub" 2>/dev/null)" ] || {
    echo "build-desktop-deb: $sub is empty; run git submodule update --init $sub" >&2; exit 1; }
done
rm -rf "$src/debian"
cp -r "$src/packaging/debian-desktop" "$src/debian"

(cd "$src" && dpkg-buildpackage -b -us -uc)
cp "$work"/*.deb "$out"/
cp "$work"/*.buildinfo "$work"/*.changes "$out"/ 2>/dev/null || true
ls -1 "$out"/*.deb
