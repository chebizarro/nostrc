#!/usr/bin/env bash
# Build only the consumer's C closure, install it, then use installed headers/libs.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/nostrc-rust.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
prefix="$scratch/prefix"
build="$scratch/build"

cmake -S "$root" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_INSTALL_RPATH="$prefix/lib" \
  -DBUILD_TESTING=OFF -DBUILD_TESTING_FRAMEWORK=OFF \
  -DNOSTR_WITH_GLIB=OFF -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF \
  -DBUILD_LIBBLOSSOM=ON -DBUILD_LIBPORTHOME=ON -DBUILD_LIBVAULT=ON \
  -DBUILD_LIBHANAMI=OFF -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF \
  -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF -DBUILD_APPS=OFF \
  -DBUILD_RELAYD=OFF -DSIGNET_ENABLE=OFF -DENABLE_NIP55L=OFF \
  -DENABLE_NIP04=OFF -DENABLE_NIP05=OFF -DENABLE_NIP06=OFF -DENABLE_NIP11=OFF \
  -DENABLE_NIP17=OFF -DENABLE_NIP14=OFF -DENABLE_NIP19=OFF -DENABLE_NIP21=OFF \
  -DENABLE_NIP27=OFF -DENABLE_NIP23=OFF -DENABLE_NIP22=OFF -DENABLE_NIP28=OFF \
  -DENABLE_NIP24=OFF -DENABLE_NIP25=OFF -DENABLE_NIP30=OFF -DENABLE_NIP39=OFF \
  -DENABLE_NIP40=OFF -DENABLE_NIP45=OFF -DENABLE_NIP46=OFF -DENABLE_NIP47=OFF \
  -DENABLE_NIP49=OFF -DENABLE_NIP86=OFF -DENABLE_NIP57=OFF -DENABLE_NIP58=OFF \
  -DENABLE_NIP60=OFF -DENABLE_NIP61=OFF -DENABLE_NIP70=OFF -DENABLE_NIP75=OFF \
  -DENABLE_NIP73=OFF -DENABLE_NIP5F=OFF -DENABLE_NIP99=OFF -DENABLE_NIP92=OFF \
  -DWITH_NIP77=OFF -DENABLE_NIP50=OFF -DENABLE_NIP51=OFF -DENABLE_SCHEMA=OFF \
  -DENABLE_NIP98=OFF -DENABLE_NIPB0=OFF -DENABLE_NIP_CAS0010=OFF \
  -DENABLE_NIP44=ON -DENABLE_NIP59=ON -DENABLE_BUD02=ON
cmake --build "$build" --target vault nostr_json --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
for package in libgo libjson libnostr nips/nip44 nips/nip59 nips/bud02 libblossom libporthome libvault; do
  cmake --install "$build/$package"
done

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DYLD_LIBRARY_PATH="$prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
export CARGO_TARGET_DIR="$scratch/target"
# Run the original C reference test against the same installed library.
# pkg-config intentionally supplies separate compiler/linker arguments here.
# shellcheck disable=SC2046
"${CC:-cc}" "$root/libporthome/tests/test_seal_v2.c" -o "$scratch/c-vectors" \
  $(pkg-config --cflags --libs porthome)
"$scratch/c-vectors"
cargo build --locked --manifest-path "$root/bindings/rust/Cargo.toml" --workspace
cargo test --locked --manifest-path "$root/bindings/rust/Cargo.toml" --workspace
