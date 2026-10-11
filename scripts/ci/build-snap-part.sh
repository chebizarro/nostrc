#!/bin/sh
# override-build of the snaps under snap/ (gnostr, grotto, grotto-daemon):
# configure with the release flags (scripts/ci/configure-release.sh), the
# vendored nsync, and install into the part.
#
#   scripts/ci/build-snap-part.sh all|daemon
#
# all: build and install the whole tree (the GUI snaps prime what they
# ship); daemon: build grotto-daemon alone.
set -eux

mode=${1:?all|daemon}
install=${CRAFT_PART_INSTALL:?run from snapcraft}

# Ubuntu 24.04's blueprint-compiler is older than the .blp files need.
python3 -m pip install --break-system-packages -q blueprint-compiler==0.22.2

scripts/ci/configure-release.sh build \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DNOSTR_USE_SYSTEM_NSYNC=OFF
if [ "$mode" = daemon ]; then
  cmake --build build -j"$(nproc)" --target grotto-daemon
  install -D build/apps/grotto/grotto-daemon "$install/usr/bin/grotto-daemon"
else
  cmake --build build -j"$(nproc)"
  DESTDIR="$install" cmake --install build
fi
