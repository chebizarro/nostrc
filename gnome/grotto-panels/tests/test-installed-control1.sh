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
# Match the SDK's compiler/instrumentation, but obtain its headers/libs only via pkg-config.
# Intentional splitting: compiler, linker and pkg-config flags are argument lists.
"${CC:-cc}" ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror "$(dirname "$0")/installed-control1.c" \
  ${LDFLAGS:-} $("${pc[@]}" --cflags --libs grotto-control-client-0) -o "$work/consumer"
mkdir -p "$work/config" "$work/data" "$work/cache" "$work/runtime"
chmod 700 "$work/runtime"
args=("$work/consumer" "$xml")
if [[ $# == 3 ]]; then args+=("$3"); fi
XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data" XDG_CACHE_HOME="$work/cache" XDG_RUNTIME_DIR="$work/runtime" \
  GSETTINGS_BACKEND=memory GTK_A11Y=test G_DEBUG=fatal-warnings \
  LD_LIBRARY_PATH="$installed_libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  dbus-run-session -- xvfb-run -a "${args[@]}"
