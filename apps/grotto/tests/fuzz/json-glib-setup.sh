#!/usr/bin/env bash
# Pinned official JSON-GLib for the fuzz jobs (nostrc-0cfsg.29).
#
# Ubuntu 24.04 ships libjson-glib 1.8.0, whose parser leaks the nodes of every
# extra root statement (GNOME/json-glib#74, fixed by MR !82 = merge commit
# 05334ecc56a6ff17c473311ee590de595b252713, first released in 1.10.0). The strict
# LeakSanitizer fuzz run therefore fails in json_parser_load_from_data().
# This script builds the official GNOME release tarball, verified by SHA-256,
# into a private prefix; nothing is patched, suppressed or filtered.
#
#   json-glib-setup.sh install PREFIX [ENV_FILE]
#   json-glib-setup.sh verify  PREFIX [FUZZ_BINARY...]
set -euo pipefail

VERSION=1.10.8
SHA256=55c5c141a564245b8f8fbe7698663c87a45a7333c2a2c56f06f811ab73b212dd
URL="https://download.gnome.org/sources/json-glib/1.10/json-glib-${VERSION}.tar.xz"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

install_pinned() {
    local prefix=$1 env_file=${2:-}
    local work
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' RETURN
    curl -fL --retry 3 "$URL" -o "$work/json-glib.tar.xz"
    printf '%s  %s\n' "$SHA256" "$work/json-glib.tar.xz" | sha256sum -c
    mkdir "$work/source"
    tar -xJf "$work/json-glib.tar.xz" --strip-components=1 -C "$work/source"
    meson setup "$work/build" "$work/source" --prefix="$prefix" --libdir=lib \
        --buildtype=release -Dintrospection=disabled -Dgtk_doc=disabled \
        -Ddocumentation=disabled -Dman=false -Dtests=false -Dnls=disabled
    meson compile -C "$work/build"
    meson install -C "$work/build"
    PKG_CONFIG_PATH="$prefix/lib/pkgconfig" pkg-config --modversion json-glib-1.0 | grep -x "$VERSION"
    if [ -n "$env_file" ]; then
        {
            echo "JSON_GLIB_PREFIX=$prefix"
            echo "PKG_CONFIG_PATH=$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
            echo "LDFLAGS=-Wl,-rpath,$prefix/lib${LDFLAGS:+ $LDFLAGS}"
        } >> "$env_file"
    fi
}

verify_pinned() {
    local prefix=$1
    shift
    local work bin lib
    lib="$prefix/lib/libjson-glib-1.0.so.0"
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' RETURN
    export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    pkg-config --modversion json-glib-1.0 | grep -x "$VERSION"

    # Leak-negative API probe under strict ASan/LSan (no suppressions, no
    # detect_leaks override); must load the pinned library, not the system one.
    # shellcheck disable=SC2046
    clang -g -fsanitize=address -o "$work/probe" "$HERE/json_glib_leak_probe.c" \
        $(pkg-config --cflags --libs json-glib-1.0) -Wl,-rpath,"$prefix/lib"
    "$work/probe" | tee "$work/probe.out"
    grep -x "loaded_library=$(readlink -f "$lib")" "$work/probe.out"
    [ "$(grep -c '^loaded_library=' "$work/probe.out")" -eq 1 ]

    for bin in "$@"; do
        ldd "$bin" | tee "$work/ldd.out"
        grep -F "libjson-glib-1.0.so.0 => $lib" "$work/ldd.out"
    done
}

case "${1:-}" in
    install) shift; install_pinned "$@" ;;
    verify) shift; verify_pinned "$@" ;;
    *) echo "usage: $0 install PREFIX [ENV_FILE] | verify PREFIX [FUZZ_BINARY...]" >&2; exit 2 ;;
esac
