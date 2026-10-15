#!/usr/bin/env bash
# G12 TSan dependency runtime only (nostrc-u6v55.14.4). No system install or patch.
# Distro GLib's uninstrumented futex/atomic handoffs are invisible to TSan.
# Native/ASAN jobs continue using stock Ubuntu libraries.
set -euo pipefail
prefix="${1:?usage: bash scripts/grotto-tsan-runtime.sh PRIVATE_PREFIX}"
work="${prefix}-build"
mkdir -p "$work/source"
curl --fail --location --retry 3 \
  https://download.gnome.org/sources/glib/2.80/glib-2.80.0.tar.xz \
  --output "$work/glib.tar.xz"
echo "8228a92f92a412160b139ae68b6345bd28f24434a7b5af150ebe21ff587a561d  $work/glib.tar.xz" | sha256sum --check
tar -xJf "$work/glib.tar.xz" --strip-components=1 -C "$work/source"
meson setup "$work/build" "$work/source" --prefix="$prefix" --libdir=lib \
  --buildtype=debugoptimized --wrap-mode=nodownload -Db_sanitize=thread \
  -Dtests=false -Dinstalled_tests=false -Dintrospection=disabled \
  -Ddocumentation=false -Dman-pages=disabled -Dnls=disabled -Dsysprof=disabled
meson compile -C "$work/build" -j "${JOBS:-2}"
meson install -C "$work/build" --no-rebuild
PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
  pkg-config --modversion glib-2.0 gio-2.0 gobject-2.0
