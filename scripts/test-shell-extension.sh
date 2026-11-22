#!/usr/bin/env bash
# Standalone affected check: no signer build or unrelated CTests.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
for tool in cmake ctest glib-compile-schemas node gjs dbus-run-session; do
  command -v "$tool" >/dev/null || { echo "Required extension test tool missing: $tool" >&2; exit 2; }
done
if [[ $(uname -s) == Linux ]]; then
  command -v dh_install >/dev/null || { echo 'Required extension test tool missing: dh_install' >&2; exit 2; }
fi
work=$(mktemp -d "$root/build-shell-extension.XXXXXX")
trap 'rm -rf "$work"' EXIT
cat > "$work/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.22)
project(NostrShellExtensionChecks LANGUAGES NONE)
enable_testing()
set(BUILD_TESTING ON)
add_subdirectory("${EXTENSION_SOURCE}" extension)
CMAKE
cmake -S "$work" -B "$work/build" -DEXTENSION_SOURCE="$root/gnome/nostr-shell-extension"
ctest --test-dir "$work/build" -R '^nostr-shell-extension-' --output-on-failure --no-tests=error
