# MDK 0.9.0 expected-incompatible probe (nostrc-a5u5): makes KeyPackages with
# MDK v0.9.0 (a102b196, the 0xF2F1 v1 account proof), pinned in Cargo.toml and
# Cargo.lock; see src/main.rs and ../README.md.
#
#   docker build -t nostrc-mdk-interop:0.9.0-probe tests/interop/mdk/driver-0.9-probe
#
# Rust 1.90.0 is MDK v0.9.0's own toolchain pin.
FROM docker.io/library/rust:1.90.0-slim-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      git ca-certificates perl make pkg-config \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Cargo.toml Cargo.lock rust-toolchain.toml ./
RUN mkdir src && echo 'fn main() {}' > src/main.rs \
 && cargo build --release --locked && rm -rf src
COPY src ./src
RUN touch src/main.rs && cargo build --release --locked

FROM docker.io/library/debian:bookworm-slim
COPY --from=build /src/target/release/mdk09-probe-driver /usr/local/bin/mdk09-probe-driver
ENTRYPOINT ["/usr/local/bin/mdk09-probe-driver"]
