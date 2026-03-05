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
    git \
    libglib2.0-dev \
    libjson-glib-dev \
    libssl-dev \
    libsqlite3-dev \
    libsodium-dev \
    libmicrohttpd-dev \
    libsecp256k1-dev \
    libcurl4-openssl-dev \
    ca-certificates \
  && rm -rf /var/lib/apt/lists/*

# Copy the entire monorepo (needed for in-tree libs)
WORKDIR /src
COPY . .

# Build the monorepo with CMake to produce libnostr, nip libs, etc.
RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DSIGNET_ENABLE=ON \
  && cmake --build build --target signetd signetctl

# ============================================================================
# Stage 2: Runtime
# ============================================================================
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libglib2.0-0 \
    libjson-glib-1.0-0 \
    libssl3 \
    libsqlite3-0 \
    libsodium23 \
    libmicrohttpd12 \
    libsecp256k1-1 \
    ca-certificates \
    curl \
  && rm -rf /var/lib/apt/lists/*

# Create non-root user for the daemon
RUN groupadd -r signet && useradd -r -g signet -d /data -s /usr/sbin/nologin signet

# Copy binaries from build stage
COPY --from=builder /src/build/signet/signetd /usr/local/bin/signetd
COPY --from=builder /src/build/signet/signetctl /usr/local/bin/signetctl

# Copy shared libraries built by the monorepo
COPY --from=builder /src/build/libnostr/libnostr.so* /usr/local/lib/
COPY --from=builder /src/build/nips/nip04/libnip04.so* /usr/local/lib/
COPY --from=builder /src/build/nips/nip19/libnip19.so* /usr/local/lib/
COPY --from=builder /src/build/nips/nip46/libnip46.so* /usr/local/lib/
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
