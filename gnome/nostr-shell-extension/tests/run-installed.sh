#!/bin/sh
# SPDX-License-Identifier: MIT
# Exercise the production install rules without configuring the rest of nostrc.
set -eu
GJS=${1:-gjs}
CMAKE=${2:-cmake}
COMPILE_SCHEMAS=${3:-glib-compile-schemas}
DH_INSTALL=${4:-}
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/nostr-extension-install-XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
cat > "$tmp/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(nostr_extension_install_test LANGUAGES NONE)
set(CMAKE_INSTALL_LIBDIR lib CACHE PATH "Library directory (data-only fixture)")
set(BUILD_TESTING OFF)
add_subdirectory("${EXTENSION_SOURCE}" extension)
CMAKE
"$CMAKE" -S "$tmp" -B "$tmp/build" -DEXTENSION_SOURCE="$here/.." \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_DATADIR=share
DESTDIR="$tmp/root" "$CMAKE" --install "$tmp/build"
root="$tmp/root"
if [ -n "$DH_INSTALL" ]; then
    # Use the real Debian file-selection tool; CMake staging alone cannot catch
    # a stale .install list that drops new modules or icons (nostrc-u6v55.24).
    mkdir "$tmp/debian"
    repo=$(cd "$here/../../.." && pwd)
    cp "$repo/debian/control" "$repo/debian/changelog" \
        "$repo/debian/gnome-shell-extension-nostr.install" "$tmp/debian/"
    (cd "$tmp"; DH_COMPAT=13 "$DH_INSTALL" -pgnome-shell-extension-nostr \
        --sourcedir="$root")
    root="$tmp/debian/gnome-shell-extension-nostr"
fi
schemas="$root/usr/share/glib-2.0/schemas"
"$COMPILE_SCHEMAS" --strict "$schemas"
GIO_USE_VFS=local GSETTINGS_BACKEND=memory "$GJS" -m "$here/test-installed.js" \
    "$root/usr/share/gnome-shell/extensions/nostr@nostr.org" "$schemas"
