# SPDX-License-Identifier: MIT
#
# Signet - NIP-46 Nostr bunker service
#
# Multi-stage build: compile with full toolchain, run with minimal runtime.
# Uses CMake monorepo build to resolve in-tree dependencies.

# ============================================================================
# Stage 1: Build
# ============================================================================
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    meson \
    python3-pip \
    git \
    libglib2.0-dev \
    libjson-glib-dev \
    libjansson-dev \
    libssl-dev \
    libsqlite3-dev \
    libsodium-dev \
    libmicrohttpd-dev \
    libsecp256k1-dev \
    libcurl4-openssl-dev \
    libwebsockets-dev \
    libnsync-dev \
    ca-certificates \
  && rm -rf /var/lib/apt/lists/* \
  && pip3 install --no-cache-dir --break-system-packages "cmake>=3.30,<3.31"

# Copy the entire monorepo (needed for in-tree libs)
WORKDIR /src
COPY . .

# Build the monorepo with CMake to produce libnostr, nip libs, etc.
RUN cmake -B build-docker -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DSIGNET_ENABLE=ON \
    -DBUILD_NOSTR_GTK=OFF \
    -DBUILD_APPS=OFF \
    -DBUILD_LIBMARMOT=OFF \
    -DBUILD_MARMOT_GOBJECT=OFF \
  && cmake --build build-docker --target signetd signetctl

# ============================================================================
# Stage 2: Runtime
# ============================================================================
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libglib2.0-0 \
    libjson-glib-1.0-0 \
    libjansson4 \
    libssl3 \
    libsqlite3-0 \
    libsodium23 \
    libmicrohttpd12 \
    libsecp256k1-1 \
    libnsync1 \
    libwebsockets17 \
    libcap2 \
    ca-certificates \
    curl \
  && rm -rf /var/lib/apt/lists/*

# Create non-root user for the daemon
RUN groupadd -r signet && useradd -r -g signet -d /data -s /usr/sbin/nologin signet

# Copy binaries from build stage
COPY --from=builder /src/build-docker/signet/signetd /usr/local/bin/signetd
COPY --from=builder /src/build-docker/signet/signetctl /usr/local/bin/signetctl

# Grant CAP_IPC_LOCK so mlockall() works for non-root deployments without
# Docker cap_add (e.g., bare metal with systemd).  Requires libcap2-bin.
RUN apt-get update && apt-get install -y --no-install-recommends libcap2-bin \
  && setcap cap_ipc_lock+ep /usr/local/bin/signetd \
  && apt-get purge -y libcap2-bin && apt-get autoremove -y \
  && rm -rf /var/lib/apt/lists/*

# Copy shared libraries built by the monorepo
COPY --from=builder /src/build-docker/libnostr/libnostr.so* /usr/local/lib/
COPY --from=builder /src/build-docker/libjson/libnostr_json.so* /usr/local/lib/
COPY --from=builder /src/build-docker/nips/nip04/libnip04.so* /usr/local/lib/
COPY --from=builder /src/build-docker/nips/nip19/libnip19.so* /usr/local/lib/
COPY --from=builder /src/build-docker/nips/nip44/libnip44.so* /usr/local/lib/
COPY --from=builder /src/build-docker/nips/nip46/libnip46.so* /usr/local/lib/
RUN ldconfig

# Data directory for SQLCipher database
RUN mkdir -p /data && chown signet:signet /data
VOLUME /data

# Health check endpoint
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD curl -sf http://127.0.0.1:8080/health || exit 1

# Process hardening: run as non-root
USER signet

# Required environment variables:
#   SIGNET_DB_KEY       - Master key for SQLCipher
#   SIGNET_BUNKER_NSEC  - Bunker identity nsec
#   SIGNET_RELAYS       - Comma-separated relay URLs
ENV SIGNET_DB_KEY="" \
    SIGNET_BUNKER_NSEC="" \
    SIGNET_RELAYS=""

ENTRYPOINT ["signetd"]
CMD ["-c", "/data/signet.toml"]
