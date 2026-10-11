#!/usr/bin/env bash
# Run only the fake Secret Service contract against the pinned libsecret.
set -euo pipefail

version="$(pkg-config --modversion libsecret-1)"
if ! dpkg --compare-versions "$version" ge 0.21.8; then
    echo "ERROR: libsecret $version is below required 0.21.8" >&2
    exit 1
fi
libdir="$(pkg-config --variable=libdir libsecret-1)"
if [[ "$libdir" != /opt/libsecret-0.21.8/lib ]]; then
    echo "ERROR: libsecret resolves outside the pinned prefix: $libdir" >&2
    exit 1
fi
echo "NIP-55L refusing-session contract: libsecret $version from $libdir"

# A runtime test reaches only the paths it drives; reject every sync
# session-open call in the signer sources as well as exercising the live read
# path below.
if grep -rnE '(^|[^[:alnum:]_])(secret_service_ensure_session_sync|_secret_session_open_sync)[[:space:]]*\(' \
        /src/nips/nip55l/src; then
    echo "ERROR: synchronous libsecret session opening reintroduced" >&2
    exit 1
fi

build=/build/nostrc
cmake -S /src -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DNIP55L_SECRET_BACKEND=libsecret \
    -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF \
    -DBUILD_RELAYD=OFF -DSIGNET_ENABLE=OFF \
    -DBUILD_LIBHANAMI=OFF -DBUILD_TESTING_FRAMEWORK=OFF \
    -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF \
    -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF -DBUILD_APPS=OFF
cmake --build "$build" --target test_nip55l_dbus_contract -j 2

# Check the runtime resolution too: the pkg-config guard alone cannot catch
# an unintended dynamic-loader fallback to Noble's system libsecret.
for binary in "$build/nips/nip55l/test_nip55l_dbus_contract" \
              "$build/nips/nip55l/nostr-signer-daemon"; do
    if ! ldd "$binary" | grep -F "libsecret-1.so.0 => $libdir/"; then
        echo "ERROR: $binary is not linked to pinned libsecret in $libdir" >&2
        exit 1
    fi
done

ctest --test-dir "$build" --no-tests=error --output-on-failure -V \
    -R '^nip55l_fake_secret_service_contract$'
