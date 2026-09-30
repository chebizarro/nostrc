# MDK 0.8 interop peer (nostrc-7gx7, nostrc-77pa): builds the JSON-lines
# driver in src/main.rs against MDK v0.8.0 (575ae29d, pinned in Cargo.toml and
# Cargo.lock) so the interop test needs no host Rust toolchain.
#
#   docker build -t nostrc-mdk-interop:0.8.0 tests/interop/mdk/driver
#   echo '{"id":1,"cmd":"hello"}' | docker run --rm -i nostrc-mdk-interop:0.8.0
#
# The test runs it with --network host on Linux, and with
# -e MDK_DRIVER_DIAL_HOST=host.docker.internal on Docker Desktop (macOS), so
# it reaches the test's relays on the host's 127.0.0.1.
FROM docker.io/library/rust:1.93-slim-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Cargo.toml Cargo.lock ./
# Dependencies first (their layer survives edits of the driver).
RUN mkdir src && echo 'fn main() {}' > src/main.rs \
 && cargo build --release --locked && rm -rf src
COPY src ./src
RUN touch src/main.rs && cargo build --release --locked

FROM docker.io/library/debian:bookworm-slim
COPY --from=build /src/target/release/mdk-interop-driver /usr/local/bin/mdk-interop-driver
ENTRYPOINT ["/usr/local/bin/mdk-interop-driver"]
