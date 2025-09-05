# NIP-77 (Negentropy) — Implementation Guide

This directory contains the core session implementation, message encoding/decoding, and an optional NostrDB-backed datasource for NIP-77.

- Core public API: `nips/nip77/include/nostr/nip77/negentropy.h`
- Backend (optional): `nips/nip77/backends/nostrdb/nostr-negentropy-ndb.c`
- Tests: `nips/nip77/tests/`

The NostrDB backend materializes an in-memory list of `(created_at, id)` items and sorts them by `created_at ASC, id ASC`. This keeps behavior simple and correct; a future iteration can introduce a streaming/ordered-cursor approach for large datasets.

## Build options

Enable the NostrDB target and the backend:

```sh
cmake -S . -B build \
  -DLIBNOSTR_WITH_NOSTRDB=ON \
  -DENABLE_NIP77_NOSTRDB=ON
cmake --build build -j
```

If you see a warning about `target 'nostrdb' not found`, ensure vendored sources exist in `third_party/nostrdb/` (see below) and that `flatccrt` is available on your system.

The top-level build wires a vendored NostrDB from `third_party/nostrdb/` and its dependencies (LMDB, CCAN, secp256k1). The build expects the `flatccrt` runtime library to match vendored flatcc headers. On macOS (Homebrew):

```sh
brew install flatcc
```

## Test suite

List of notable tests under `nips/nip77/tests/`:

- `test_ndb_integration`: NostrDB backend init + iteration smoke.
- `test_ndb_ordering`: Asserts `created_at` ordering and presence of seeded timestamps.
- `test_ndb_tie_break`: Duplicate timestamps sorted by `id ASC`.
- `test_ndb_empty`: Iteration on an empty DB.
- `test_ndb_session_e2e`: End-to-end negotiation (IdList and Split cases) using the NostrDB datasource.
- `test_ndb_perf`: Performance harness (disabled by default; see below).

Run all NIP-77+NDB tests:

```sh
ctest -R ndb -j4 --output-on-failure --test-dir build
```

## Performance tests (NostrDB datasource)

The performance harness measures two phases:

- Ingestion time: calls `ndb_process_event` for N synthetic events.
- Iteration time: constructs the datasource and iterates all items once.

The test is intentionally skipped unless explicitly enabled (to keep CI fast):

```sh
# from the build tree where tests are produced
cd build/nips/nip77/tests

# Run with defaults (N=100k)
NIP77_RUN_PERF=1 ./test_ndb_perf

# Specify dataset size
NIP77_RUN_PERF=1 NIP77_PERF_N=200000 ./test_ndb_perf
```

Example output:

```
perf: n=100000 ingest_sec=1.92 iter_sec=0.41 iter_throughput=243902 items/s
```

Interpretation:

- `ingest_sec` captures JSON parse + LMDB write overhead.
- `iter_sec` reflects the cost of materializing, sorting, and iterating.
- `iter_throughput` is a quick indicator of end-to-end iteration performance.

Notes:

- The harness uses a larger LMDB mapsize for big N (at least 64MB, scaled with N) to avoid immediate growth overhead.
- For stable comparisons, use a Release or RelWithDebInfo build.

## Troubleshooting NostrDB init

If `ndb_init` fails in your environment, consider the following:

- Ensure the temp directory is writable and not sandboxed. The tests default to `mkdtemp("/tmp/...")`.
- Verify `flatccrt` is installed and discoverable by the linker.
- On macOS, the Security framework is linked automatically; ensure Xcode command-line tools are present.
- Increase LMDB mapsize or reduce N for the perf test via `NIP77_PERF_N`.

The NostrDB-backed datasource (`nostr-negentropy-ndb.c`) logs failures with:

```
nostr_ndb_make_datasource: mkdir('<path>') failed: <errno>
nostr_ndb_make_datasource: ndb_init('<path>') failed (flags=0x..., mapsize=...)
```

While `test_ndb_perf` itself prints a simple skip message on failure, you can temporarily add similar logging if you need to diagnose environment issues specifically for perf runs.

## Future work

- Streaming/ordered-cursor iteration to avoid full-materialization costs for very large datasets.
- Additional large-corpus benchmarks and memory profiling.
