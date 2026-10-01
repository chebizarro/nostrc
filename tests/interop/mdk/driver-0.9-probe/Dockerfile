# MDK 0.9.0 expected-incompatible probe (nostrc-a5u5): makes KeyPackages with
# MDK v0.9.0 (a102b196, the 0xF2F1 v1 account proof), pinned in Cargo.toml and
# Cargo.lock; see src/main.rs and ../README.md.
#
#   docker build -t nostrc-mdk-interop:0.9.0-probe tests/interop/mdk/driver-0.9-probe
#
# Rust 1.90.0 is MDK v0.9.0's own toolchain pin.
# Base images pinned by multi-arch index digest (tag kept for reading).
FROM docker.io/library/rust:1.90.0-slim-bookworm@sha256:64232e656c058f4468e8d024e990acff04f0fd5a5c0a88a574dc37773d7325c9 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      git ca-certificates perl make pkg-config \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Cargo.toml Cargo.lock rust-toolchain.toml ./
RUN mkdir src && echo 'fn main() {}' > src/main.rs \
 && cargo build --release --locked && rm -rf src
COPY src ./src
RUN touch src/main.rs && cargo build --release --locked
# The parity tests: the probe's feature registry, component set and session
# configuration are MDK 0.9 marmot-app's (read from Cargo's git checkout).
RUN cargo test --release --locked

FROM docker.io/library/debian:bookworm-slim@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251
COPY --from=build /src/target/release/mdk09-probe-driver /usr/local/bin/mdk09-probe-driver
ENTRYPOINT ["/usr/local/bin/mdk09-probe-driver"]
