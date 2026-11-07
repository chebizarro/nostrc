# SPDX-License-Identifier: MIT
# Build from the repository root via signet/deploy/build-image.sh.
# All external selections are supplied from deploy/versions.env; no floating default.
ARG DEBIAN_IMAGE
FROM ${DEBIAN_IMAGE} AS builder
ARG CMAKE_VERSION
ARG MESON_VERSION
ARG SIGNET_PASSKEYS=OFF
RUN case "$SIGNET_PASSKEYS" in ON|OFF) ;; *) exit 1;; esac \
 && apt-get update && apt-get install -y --no-install-recommends \
    build-essential ninja-build pkg-config python3-pip python3-venv python3-jsonschema git \
    libglib2.0-dev libjson-glib-dev libjansson-dev libssl-dev \
    libsqlite3-dev libsqlcipher-dev libsodium-dev libmicrohttpd-dev \
    libsecp256k1-dev libcurl4-openssl-dev libwebsockets-dev libnsync-dev \
    libgit2-dev ca-certificates \
 && if [ "$SIGNET_PASSKEYS" = ON ]; then apt-get install -y --no-install-recommends libcbor-dev; fi \
 && rm -rf /var/lib/apt/lists/* \
 && pip3 install --no-cache-dir --break-system-packages "cmake==$CMAKE_VERSION" "meson==$MESON_VERSION"
WORKDIR /src
COPY . .
RUN cmake -B build-docker -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DSIGNET_ENABLE=ON -DSIGNET_USE_SQLCIPHER=ON \
    -DSIGNET_ENABLE_PASSKEYS=${SIGNET_PASSKEYS} -DSIGNET_ENABLE_TEST_HOOKS=OFF \
    -DBUILD_NOSTR_GTK=OFF -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF \
    -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF -DWITH_NOSTRDB=OFF \
 && cmake --build build-docker --target signetd signetctl -j2 \
 && cc -O2 -Wall -Wextra -Werror -I/usr/include/sqlcipher signet/deploy/store-inspect.c \
      -o /src/build-docker/signet-inspect-store -lsqlcipher -lsodium \
 && dpkg-query -W > /src/build-docker/package-inputs.txt

FROM builder AS test
# Keep testing out of the runtime builder; compile only the outbound TLS regression
# and its dependencies, without the unrelated GObject/test-framework trees.
RUN cmake -B build-docker -DBUILD_TESTING=ON \
      -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_TESTING_FRAMEWORK=OFF \
 && cmake --build build-docker --target test_connection_tls_no_resumption -j2 \
 && ctest --test-dir build-docker/tests --output-on-failure --no-tests=error \
      -R '^test_connection_tls_no_resumption$'
RUN if [ "$SIGNET_PASSKEYS" = ON ]; then passkeys=true; else passkeys=false; fi \
 && meson setup signet/build-meson-sqlite signet \
      -Dnostrc_build_dir=/src/build-docker -Dsignet_enable_tests=true \
      -Dsignet_enable_passkeys=$passkeys -Dsignet_enable_test_hooks=false \
 && meson setup signet/build-meson-sqlcipher signet \
      -Dnostrc_build_dir=/src/build-docker -Dsignet_enable_tests=true \
      -Dsignet_use_sqlcipher=true -Dsignet_enable_passkeys=$passkeys \
      -Dsignet_enable_test_hooks=false \
 && (meson test -C signet/build-meson-sqlite --print-errorlogs; sqlite_status=$?; \
     meson test -C signet/build-meson-sqlcipher --print-errorlogs; cipher_status=$?; \
     test "$sqlite_status" = 0 && test "$cipher_status" = 0)

FROM ${DEBIAN_IMAGE} AS runtime
ARG SIGNET_PASSKEYS=OFF
ARG SOURCE_URL=https://github.com/chebizarro/nostrc
ARG REVISION
ARG VERSION
ARG CREATED
LABEL org.opencontainers.image.source=$SOURCE_URL \
      org.opencontainers.image.revision=$REVISION \
      org.opencontainers.image.version=$VERSION \
      org.opencontainers.image.created=$CREATED \
      org.opencontainers.image.licenses=MIT
RUN case "$SIGNET_PASSKEYS" in ON|OFF) ;; *) exit 1;; esac \
 && apt-get update && apt-get install -y --no-install-recommends \
    libglib2.0-0 libjson-glib-1.0-0 libjansson4 libssl3 libsqlcipher0 \
    libsodium23 libmicrohttpd12 libsecp256k1-1 libnsync1 libwebsockets17 \
    libcurl4 ca-certificates curl sqlcipher \
 && if [ "$SIGNET_PASSKEYS" = ON ]; then apt-get install -y --no-install-recommends libcbor0.8; fi \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd -g 10001 signet \
 && useradd -u 10001 -g 10001 -d /data -s /usr/sbin/nologin signet \
 && install -d -o 10001 -g 10001 /data /etc/signet /usr/share/signet \
 && dpkg-query -W > /usr/share/signet/runtime-packages.txt
COPY --from=builder /src/build-docker/signet/signetd /usr/local/bin/signetd
COPY --from=builder /src/build-docker/signet/signetctl /usr/local/bin/signetctl
COPY --from=builder /src/build-docker/package-inputs.txt /usr/share/signet/build-packages.txt
COPY --from=builder /src/build-docker/libnostr/libnostr.so* /usr/local/lib/
COPY --from=builder /src/build-docker/libjson/libnostr-json.so* /usr/local/lib/
COPY --from=builder /src/build-docker/nips/nip04/libnip04.so* /usr/local/lib/
COPY --from=builder /src/build-docker/nips/nip19/libnip19.so* /usr/local/lib/
COPY --from=builder /src/build-docker/nips/nip44/libnip44.so* /usr/local/lib/
COPY --from=builder /src/build-docker/nips/nip46/libnip46.so* /usr/local/lib/
RUN ldconfig \
 && ldd /usr/local/bin/signetd > /usr/share/signet/ldd-signetd.txt \
 && ldd /usr/local/bin/signetctl > /usr/share/signet/ldd-signetctl.txt \
 && ! grep -q 'not found' /usr/share/signet/ldd-*.txt
COPY --from=builder /src/build-docker/signet-inspect-store /usr/local/bin/signet-inspect-store
USER 10001:10001
ENTRYPOINT ["signetd"]
CMD ["-c", "/etc/signet/signet.conf"]
