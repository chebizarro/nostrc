# Upstream profiler fixtures

Captured from read-only `/Users/bizarro/Documents/Dev/fips` at
`6aa80504baa0fa312b392009eea0e3e59fb6fe4c` on macOS arm64, using:

```sh
CARGO_TARGET_DIR=/tmp/fips-rust-target-prof cargo build \
  --manifest-path /Users/bizarro/Documents/Dev/fips/Cargo.toml \
  --locked --release --features profiling --bin fips --bin fipsctl
```

`rust-tick-release.tsv` is an actual idle capture, armed with upstream
`fipsctl --socket <socket> profile tick on --dir <private worktree directory>`,
then stopped after the pipeline reporter's first one-second interval. The
identity is the public key of the harness's explicit, disposable test secret
(`11` repeated 32 times), not a developer identity. `rust-control.json` records
the actual upstream CLI's JSON data objects (wire responses wrap them in
`status`/`data`). `rust-pipeline.txt` contains real report lines from a subsequent
three-second Rust/Rust run of `perf_loopback.run_pair`, using the same native
traffic driver as C.

Tests normalize timestamps, counters and metadata **values**, not column names,
header keys, notes, row names, units, or order. The only conditional difference
removed is `debug_assert_peer_maps_coherent`: Cargo's requested release build
omits it, while the C Debug build emits it. Linux/macOS have identical remaining
rows; other platforms must account for upstream's platform-conditional rows.
Pipeline tests compare field layout and unit syntax against the captured lines;
the C unit test additionally checks exact histogram/percentile arithmetic.
The C-only final pipeline stage `publish_snapshot` uses the same field layout;
it is not presented as an upstream stage or inserted into the tick TSV.
