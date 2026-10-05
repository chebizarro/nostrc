#!/usr/bin/env bash
# Write the source archive the desktop packages (Groundhog, Grotto) build
# from: every tracked file plus the nostrdb and nsync submodules, under one
# top-level directory.
#
#   scripts/make-desktop-source-archive.sh VERSION [OUTPUT_DIR]
#
# Produces OUTPUT_DIR/groundhog-VERSION.tar.gz and prints its SHA-256.
# GitHub's automatic tag archives leave submodules out, so recipes that
# download a release (Arch, RPM, Nix) use this one instead.
set -euo pipefail

version=${1:?usage: make-desktop-source-archive.sh VERSION [OUTPUT_DIR]}
root=$(cd "$(dirname "$0")/.." && pwd)
out=${2:-$root/build-dist}
name=groundhog-$version
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/$name" "$out"
if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git -C "$root" ls-files -z --recurse-submodules | (cd "$root" && tar --null -T - -cf -) | tar -xf - -C "$work/$name"
else
  tar -C "$root" --exclude=./build --exclude='./build-*' -cf - . | tar -xf - -C "$work/$name"
fi
for sub in third_party/nostrdb third_party/nsync; do
  [ -n "$(ls -A "$work/$name/$sub" 2>/dev/null)" ] || {
    echo "make-desktop-source-archive: $sub is empty; run git submodule update --init $sub" >&2; exit 1; }
done
# Stable order, owner and timestamps: the same tree gives the same archive.
# That needs GNU tar; with another tar (macOS) the archive is still correct,
# only not byte-for-byte reproducible.
epoch=$(git -C "$root" log -1 --format=%ct 2>/dev/null || echo 0)
tar=tar
command -v gtar >/dev/null 2>&1 && tar=gtar
if "$tar" --version 2>/dev/null | grep -q 'GNU tar'; then
  (cd "$work" && find "$name" -print0 | LC_ALL=C sort -z |
    "$tar" --no-recursion --owner=0 --group=0 --numeric-owner --mtime="@$epoch" --null -T - -cf - |
    gzip -n > "$out/$name.tar.gz")
else
  echo "make-desktop-source-archive: no GNU tar; archive will not be reproducible" >&2
  (cd "$work" && tar -cf - "$name" | gzip -n > "$out/$name.tar.gz")
fi
(cd "$out" && sha256sum "$name.tar.gz" 2>/dev/null || shasum -a 256 "$name.tar.gz")
