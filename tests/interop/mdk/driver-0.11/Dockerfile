# MDK 0.11 interop peer (nostrc-a5u5): builds the JSON-lines driver in
# src/main.rs against MDK v0.11.0 (946e0547, pinned in Cargo.toml and
# Cargo.lock; cgka-engine, the adopted Marmot profile) so the interop test
# needs no host Rust toolchain. Separate from the 0.8 image (../driver).
#
#   docker build -t nostrc-mdk-interop:0.11.0 tests/interop/mdk/driver-0.11
#   echo '{"id":1,"cmd":"hello"}' | docker run --rm -i nostrc-mdk-interop:0.11.0
#
# The test runs it with --network host on Linux, and with
# -e MDK_DRIVER_DIAL_HOST=host.docker.internal on Docker Desktop (macOS), so
# it reaches the test's relays on the host's 127.0.0.1; the redacted vectors
# go to MDK_DRIVER_ARTIFACT_DIR (a mounted directory).
#
# Rust 1.97.1 is MDK v0.11.0's own toolchain pin (rust-toolchain.toml).
# SQLCipher is built from source with vendored OpenSSL (rusqlite's
# bundled-sqlcipher-vendored-openssl), which needs perl and make.
FROM docker.io/library/rust:1.97.1-slim-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      git ca-certificates perl make pkg-config \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Cargo.toml Cargo.lock rust-toolchain.toml ./
# Dependencies first (their layer survives edits of the driver).
RUN mkdir src && echo 'fn main() {}' > src/main.rs \
 && cargo build --release --locked && rm -rf src
COPY src ./src
RUN touch src/main.rs && cargo build --release --locked

FROM docker.io/library/debian:bookworm-slim
COPY --from=build /src/target/release/mdk011-interop-driver /usr/local/bin/mdk011-interop-driver
ENTRYPOINT ["/usr/local/bin/mdk011-interop-driver"]
