#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Stage only this module; never install into or connect to the user's session.
set -euo pipefail
build=$1
libdir=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
DESTDIR="$work" cmake --install "$build"
export PKG_CONFIG_PATH="$work$libdir/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pc=(pkg-config)
installed_libdir=$("${pc[@]}" --variable=libdir grotto-control-client-0)
xml=$("${pc[@]}" --variable=interfacesdir grotto-control-client-0)/org.nostr.Grotto.Control1.xml
[[ $installed_libdir == "$work"/* && $xml == "$work"/* && -f $xml ]]
# Intentional splitting: these are pkg-config's compiler and linker flags.
${CC:-cc} -std=c11 -Wall -Wextra -Werror "$(dirname "$0")/installed-control1.c" \
  $("${pc[@]}" --cflags --libs grotto-control-client-0) -o "$work/consumer"
mkdir -p "$work/config" "$work/data" "$work/cache"
XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data" XDG_CACHE_HOME="$work/cache" \
  GSETTINGS_BACKEND=memory GTK_A11Y=test G_DEBUG=fatal-warnings \
  LD_LIBRARY_PATH="$installed_libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  dbus-run-session -- xvfb-run -a "$work/consumer" "$xml"
