#!/bin/sh
# build.sh — package the NIP-07 bridge extension (nostrc-jjyp).
#
#   ./build.sh            -> dist/nostr-signer-bridge-firefox-<ver>.xpi  (unsigned)
#                            dist/nostr-signer-bridge-chromium-<ver>.zip
#   ./build.sh --check    also run tests/policy.test.js (needs node)
#
# The .xpi is unsigned: load it via about:debugging (temporary) or a
# Firefox Developer/Nightly/ESR build with xpinstall.signatures.required
# = false, or submit it to AMO for signing. See README.md.
set -eu
cd "$(dirname "$0")"

if [ "${1:-}" = "--check" ]; then
  node tests/policy.test.js
fi

version_of() { sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' "$1" | head -n1; }
VERSION=$(version_of manifest.firefox.json)
[ "$VERSION" = "$(version_of manifest.chromium.json)" ] || {
  echo "manifest.firefox.json and manifest.chromium.json versions differ" >&2; exit 1; }

command -v zip >/dev/null || { echo "zip(1) is required" >&2; exit 1; }
rm -rf dist
mkdir -p dist
for target in firefox chromium; do
  ext=zip; [ "$target" = firefox ] && ext=xpi
  stage=$(mktemp -d)
  cp -R src "$stage/src"
  rm -rf "$stage/src/providers"          # WebLN stub is not shipped yet (nostrc-yka8)
  cp "manifest.$target.json" "$stage/manifest.json"
  cp README.md "$stage/"
  out="$PWD/dist/nostr-signer-bridge-$target-$VERSION.$ext"
  (cd "$stage" && find . -type f | LC_ALL=C sort | sed 's|^\./||' | zip -q -X -@ "$out")
  rm -rf "$stage"
  echo "built $out"
done
