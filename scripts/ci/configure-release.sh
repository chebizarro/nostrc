#!/bin/sh
# Configure a release build of the nostrc apps (gnostr, Grotto,
# grotto-daemon) in BUILD_DIR with the flags every release job shares.
# Extra arguments go to cmake (e.g. -DENABLE_TCP_IPC=ON).
#
#   scripts/ci/configure-release.sh BUILD_DIR [CMAKE_ARGS...]
#
# Needs the third_party submodules (checkout with submodules: recursive).
set -eu

build=${1:?usage: configure-release.sh BUILD_DIR [CMAKE_ARGS...]}
shift
root=$(cd "$(dirname "$0")/../.." && pwd)

for sub in third_party/nostrdb third_party/nsync; do
  [ -n "$(ls -A "$root/$sub" 2>/dev/null)" ] || {
    echo "configure-release: $sub is empty; check out submodules" >&2; exit 1; }
done

extra_cflags=
if [ "$(uname -s)" = Linux ]; then
  # GCC has no __has_feature; some vendored headers test it unguarded.
  mkdir -p "$root/$build"
  compat="$(cd "$root/$build" && pwd)/ci-gcc-compat.h"
  printf '#ifndef __has_feature\n#define __has_feature(x) 0\n#endif\n' > "$compat"
  extra_cflags="-include $compat"
  # Adw.Spinner needs libadwaita 1.6; Ubuntu 24.04 ships 1.5.
  if ! pkg-config --atleast-version=1.6 libadwaita-1; then
    sed -i 's/Adw\.Spinner/Gtk.Spinner/' "$root/apps/gnostr/data/ui/gnostr-main-window.blp"
  fi
fi

cmake -S "$root" -B "$root/$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_APPS=ON \
  -DBUILD_GROUNDHOG=OFF \
  -DSIGNET_ENABLE=OFF \
  -DBUILD_RELAYD=OFF \
  -DBUILD_LIBHANAMI=OFF \
  -DBUILD_TESTING=OFF \
  -DBUILD_SIGNER_TESTS=OFF \
  -DBUILD_TESTING_FRAMEWORK=OFF \
  ${extra_cflags:+"-DCMAKE_C_FLAGS=$extra_cflags"} \
  "$@"
