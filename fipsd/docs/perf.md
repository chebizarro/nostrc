# FIPS performance baseline

Task `nostrc-f66sr.4`, measured **2026-11-16**. No shipped code changed.
Daemon tick profiling (`iq1n9.4`) is out of scope. This is local macOS evidence,
not hosted Ubuntu validation, wire-only latency, or a language shootout.

## Machine and methodology

- Apple M4 Max, arm64, 14 logical CPUs, 36 GiB RAM, macOS 27.0.1 (26A434),
  Darwin 27.0.0. Developer workstation, no exclusive CPU reservation/affinity.
- C shipped source `d51dd48ee3ba65c76d484cf50c1a954dcf3cd51c` plus test-only
  harness; Apple Clang 21.0.0, Release `-O3 -DNDEBUG`, Werror, no sanitizers.
  libsodium 1.0.22, secp256k1 0.7.1, OpenSSL 3.6.5.
- Rust `6aa80504baa0fa312b392009eea0e3e59fb6fe4c`, exactly
  [the pin](../../libfips-core/UPSTREAM.md), rustc/cargo 1.94.1, locked dependencies,
  default upstream release/bench profiles. Upstream emits its existing unused
  `IdentityCache::told` warning; C builds with warnings-as-errors.
- Both daemons: UDP `127.0.0.1`, `FIPSD_WORKERS=2`, `TOKIO_WORKER_THREADS=2`,
  TUN/DNS off, native API on, distinct fixed benchmark keys, rekey after 3600 s.

All four upstream files (684 lines) were read: **39 workloads**, not just four
aggregate numbers. C targets `fips_bench_{noise_aead,routing_next_hop,coord_cache,
transport_addr}` require `FIPS_ENABLE_BENCH=ON`; they are neither installed nor
registered with ctest. Each warms ten batches then times 101 batches of **1000
iterations**, using `CLOCK_MONOTONIC`. `FIPS_BENCH_ITERS` changes N. Stdout is CSV,
without a header: `name,median_ns,p95_ns,ops_per_s`. p95 is nearest rank over
**batch-average ns/op**, not individual-operation latency. Compiler barriers and
live checks retain work under optimization. `ops_per_s = 1e9 / median_ns`.

The table reports medians of **three C process runs**, including the three p95
estimates. Rust used the unmodified `cargo bench --locked --bench NAME` defaults:
3 s warm-up, ~5 s measurement, 100 samples for noise/cache and 50 for
routing/address. Rust columns are median/empirical p95 of Criterion
`sample.json` time/iterations, **not** fitted slopes or confidence bounds.
There was one Criterion invocation per file; the CI C/loopback gate separately
requires at least three runs. Never select the fastest run.

### Workload fidelity and representation differences

- **Noise:** real XK pair, setup excluded; 64/1400-byte payload, 16-byte AAD,
  1024 presealed packets, replay reset on wrap. Separate session seal/open,
  in-place worker seal/open, and cipher handout. C session output is caller-owned;
  Rust allocates a Vec. C key copies are destroyed/zeroized inside timed work.
- **Routing:** 8/32/128/256 peers; eight-entry paths; four unrelated addresses
  per bloom; half advertise the destination; cost `1 + i*0.01`; identical
  destination/interior/root and tie-break inputs. C calls the real borrowed
  selector plus an allocating snapshot control. Rust's external bench mirrors
  its production HashMaps; C's production view is an array. Former controls
  are not identical container/allocator implementations.
- **Cache:** 1000/50000 entries, six-entry paths, upstream xorshift seed,
  4096 hits at `(i*7919)%n` and 4096 misses, insert at time 1/get at time 2,
  TTL `UINT64_MAX/2`. Hits and misses are verified before timing.
- **Address:** 6/18/58 bytes; IPv4/IPv6/max-scoped-IPv6 formatting+clone;
  producer clone/consumer drop via a bounded **256-item** channel. Startup and
  readiness are untimed; completion/join is timed. C `/shared` names the
  corresponding **current POD copy**, not a fabricated Arc. Its mutex/condition
  channel differs from Rust `sync_channel`; cross-thread numbers include
  channel/scheduler costs, not isolated atomics. Former socket construction
  reserves 56 bytes and grows for the scoped form; C uses libc formatting.

## Full microbenchmark table

Nanoseconds/op; **C/Rust >1 means C is slower**. Backend, representation and
channel differences above are included, not controlled away.

| Workload | C median | C p95 | Rust median | Rust p95 | C/Rust |
|---|---:|---:|---:|---:|---:|
| `coord_cache/get_hit/1000` | 17.0 | 20.0 | 11.9 | 12.4 | 1.43 |
| `coord_cache/get_hit/50000` | 27.0 | 30.0 | 15.0 | 15.6 | 1.81 |
| `coord_cache/get_miss/1000` | 11.0 | 12.0 | 9.6 | 10.0 | 1.14 |
| `coord_cache/get_miss/50000` | 16.0 | 18.0 | 11.3 | 11.8 | 1.42 |
| `find_next_hop/current_borrowed/128` | 17621.0 | 18170.0 | 5888.2 | 6195.8 | 2.99 |
| `find_next_hop/current_borrowed/256` | 35327.0 | 35996.0 | 12676.8 | 12788.4 | 2.79 |
| `find_next_hop/current_borrowed/32` | 4390.0 | 4503.0 | 1193.1 | 1234.6 | 3.68 |
| `find_next_hop/current_borrowed/8` | 1086.0 | 1155.0 | 306.0 | 379.9 | 3.55 |
| `find_next_hop/former_allocating/128` | 18555.0 | 18857.0 | 11637.4 | 12225.5 | 1.59 |
| `find_next_hop/former_allocating/256` | 37899.0 | 38825.0 | 24063.1 | 24441.9 | 1.57 |
| `find_next_hop/former_allocating/32` | 4680.0 | 4854.0 | 2675.8 | 2702.0 | 1.75 |
| `find_next_hop/former_allocating/8` | 1178.0 | 1237.0 | 710.6 | 904.2 | 1.66 |
| `handout/send_cipher_clone` | 6.0 | 6.0 | 17.7 | 17.9 | 0.34 |
| `session/open/1400` | 2182.0 | 2270.0 | 987.9 | 1000.2 | 2.21 |
| `session/open/64` | 291.0 | 314.0 | 204.0 | 215.1 | 1.43 |
| `session/seal/1400` | 2092.0 | 2173.0 | 830.1 | 839.2 | 2.52 |
| `session/seal/64` | 201.0 | 216.0 | 182.2 | 192.4 | 1.10 |
| `transport_addr_clone/former_vec/18` | 10.0 | 17.0 | 10.4 | 10.6 | 0.97 |
| `transport_addr_clone/former_vec/58` | 11.0 | 20.0 | 12.4 | 12.6 | 0.89 |
| `transport_addr_clone/former_vec/6` | 10.0 | 16.0 | 10.9 | 11.3 | 0.92 |
| `transport_addr_clone/shared/18` | 5.0 | 6.0 | 3.8 | 3.9 | 1.32 |
| `transport_addr_clone/shared/58` | 5.0 | 6.0 | 3.8 | 3.9 | 1.32 |
| `transport_addr_clone/shared/6` | 5.0 | 6.0 | 3.8 | 3.9 | 1.31 |
| `transport_addr_cross_thread/former_vec/18` | 68.0 | 75.0 | 34.1 | 37.7 | 1.99 |
| `transport_addr_cross_thread/former_vec/58` | 66.0 | 80.0 | 29.2 | 33.0 | 2.26 |
| `transport_addr_cross_thread/former_vec/6` | 60.0 | 80.0 | 15.9 | 18.3 | 3.78 |
| `transport_addr_cross_thread/shared/18` | 57.0 | 67.0 | 42.4 | 47.4 | 1.35 |
| `transport_addr_cross_thread/shared/58` | 56.0 | 68.0 | 43.3 | 46.1 | 1.29 |
| `transport_addr_cross_thread/shared/6` | 58.0 | 68.0 | 56.0 | 80.7 | 1.04 |
| `transport_addr_socket_and_clone/former_vec/ipv4` | 170.0 | 187.0 | 71.7 | 76.7 | 2.37 |
| `transport_addr_socket_and_clone/former_vec/ipv6` | 170.0 | 194.0 | 72.3 | 74.6 | 2.35 |
| `transport_addr_socket_and_clone/former_vec/scoped_ipv6` | 344.0 | 374.0 | 160.9 | 165.5 | 2.14 |
| `transport_addr_socket_and_clone/shared/ipv4` | 28.0 | 28.0 | 65.3 | 67.2 | 0.43 |
| `transport_addr_socket_and_clone/shared/ipv6` | 25.0 | 29.0 | 63.3 | 64.5 | 0.39 |
| `transport_addr_socket_and_clone/shared/scoped_ipv6` | 54.0 | 59.0 | 132.3 | 135.9 | 0.41 |
| `worker/open/1400` | 2196.0 | 2287.0 | 980.7 | 1035.4 | 2.24 |
| `worker/open/64` | 285.0 | 313.0 | 195.1 | 197.5 | 1.46 |
| `worker/seal/1400` | 2110.0 | 2182.0 | 843.3 | 852.3 | 2.50 |
| `worker/seal/64` | 205.0 | 219.0 | 199.7 | 203.0 | 1.03 |

## Real-daemon UDP/native-API results

The harness observes IK/tree/bloom readiness, opens XK via a native setup flow,
and requires both peers to report an established, proven session with matching
identities and complementary roles. No fixed sleep substitutes for readiness.
The C traffic driver links **libfipsnative**, using both real daemon APIs, not an
in-process test transport. Python manages lifecycle, not the hot traffic loop.

After 100 warm-up round trips, measure **1000 serial 1200-byte exchanges**.
Both endpoints are timestamped by one process/monotonic clock: one-way is directly
measured, **not RTT/2**. These application-to-application times include native
APIs, both daemons, UDP and client syscall overhead; they are measured separately
from saturation. Throughput is then **five seconds of unpaced offered load**,
with a receiver thread continuously draining the far endpoint. Only an
unaccepted local EAGAIN/ENOBUFS send may be tried again; accepted datagrams are
never retransmitted. Contents/duplicates are checked and reordering is allowed.

Goodput = `received*1200*8 / seconds / 1e6`. `sent` counts local native acceptance,
not wire delivery. Loss includes packets in flight at the deadline; no drain
sleep inflates goodput. The producer consumes a client core and can expose
scheduler/queue overload. This is **not a search for maximum lossless capacity**.
Throughput follows the arrow; latency exercises both directions. Three fresh
runs per pairing, rotating order; each cell is the median of three run values.

| Pair/direction | Delivered Mbit/s | Native-accepted Mbit/s | Loss % | Forward p50/p99 us | Reverse p50/p99 us | RTT p50/p99 us |
|---|---:|---:|---:|---:|---:|---:|
| c → c | 0.204 | 20.035 | 98.97 | 1584.0 / 2025.0 | 1592.0 / 1987.0 | 3174.0 / 4005.0 |
| rust → rust | 817.661 | 1667.685 | 51.46 | 62.0 / 101.0 | 62.0 / 100.0 | 124.0 / 195.0 |
| c → rust | 0.207 | 20.141 | 98.97 | 621.0 / 792.0 | 1098.0 / 1272.0 | 1728.0 / 2012.0 |

CPU snapshots span the complete traffic-driver interval (warm-up, latency,
throughput): `/proc/PID/stat` on Linux, `ps -o time=` on macOS. Percentages are
relative to **one logical CPU**, so may exceed 100%. Child `getrusage` deltas
around each individually reaped daemon also record lifetime user/system time,
including startup/shutdown. Neither generator nor `ps` CPU is charged to a daemon.

| Pair | Left/right traffic CPU seconds | Left/right CPU % | Left lifetime user/system seconds | Right lifetime user/system seconds |
|---|---:|---:|---:|---:|
| c-c | 6.810 / 1.960 | 78.9 / 22.9 | 6.694 / 0.258 | 1.902 / 0.187 |
| rust-rust | 7.730 / 8.230 | 150.0 / 159.9 | 3.037 / 4.762 | 6.058 / 2.258 |
| c-rust | 6.790 / 0.110 | 97.8 / 1.6 | 6.698 / 0.241 | 0.067 / 0.114 |

## Reading the gaps (not profiler attribution)

- **1400-byte AEAD ~2.2–2.5x slower:** C calls libsodium ChaCha20-Poly1305;
  Rust uses ring. The increasing payload-size gap suggests bulk crypto backend
  cost, not merely allocation. Opens also check/mark replay; C advances by
  shifting 128 bitmap words (`libfips-core/src/noise/replay.c`).
- **Borrowed routing ~3x slower:** each peer's C bloom check hashes the same
  destination with SHA-256 (`src/proto/bloom/core.c`). Repeated hashing and
  coordinate-distance work are candidates, not individually measured costs.
- **Cache 1.1–1.8x slower:** keyed open-address probing/expiry checks and inline
  capacity for 80 coordinate entries per slot differ from Rust's layout.
  The larger 50000-entry hit gap suggests memory-layout/cache pressure; no
  hardware cache-miss profile was collected.
- **Address:** current socket construction+POD clone is faster; raw current-copy
  and cross-thread differences include the storage/channel implementations.
  Former libc-formatting controls are not shipped hot paths.
- **Daemon overload dominates:** C goodput is ~0.2 Mbit/s with ~99% loss of
  native-accepted traffic, versus Rust ~818 Mbit/s with ~51% loss. C/C serial
  RTT ~3.2 ms versus Rust/Rust ~0.12 ms is far beyond the AEAD microsecond delta.
  `fipsd/src/native/api.c` yields up to 64 times on a full dispatcher mailbox,
  then drops; its feeder also drops on native-socket ENOBUFS. Queueing/scheduling
  and native delivery are the first follow-up candidates. Source inspection
  **does not locate every loss or prove causality**. No shipped-code tuning or
  out-of-scope tick profiler was added to make the result look better.

## CI policy

[`perf_baseline.json`](../tests/perf_baseline.json) records this host, workload,
66 aggregated metrics, immutable C/Rust pins, and **10% tolerance**.
[`perf_compare.py`](../tests/perf_compare.py) requires medians over **at least
three runs** and fails closed on missing/invalid results, mismatched actual
run counts/durations/latency sample counts, or a different Rust revision. Throughput decrease
>10%, or cost/latency increase >10%, fails. Gates: each micro median; delivered
throughput; both one-way p50/p99; RTT p50/p99; each daemon's traffic CPU normalized
by delivered datagrams. Micro p95, loss and lifetime CPU are reported, not gates.

**Apple absolute timings must not gate an Ubuntu VM.** `bench` runs only for
`workflow_dispatch`/`schedule`, on `ubuntu-24.04`:

1. Honors and verifies a reusable caller's requested C revision. Reuses
   `interop-udp`'s pinned Rust checkout/cache/build steps verbatim, checks that
   `UPSTREAM.md` matches the baseline's Rust pin, and records that verified
   revision in each result artifact. Local callers must supply the verified
   Rust build revision via `--rust-commit`.
2. Checks out the C reference commit from the JSON. Copies only the current
   test harness into it (the base predates the harness), never shipped candidate
   source. API incompatibility fails rather than silently rebaselining.
3. Builds both Release/Werror and measures three sequential runs each on the
   **same runner**, with the same Rust binary. The comparer uses the JSON's
   source pin, workload/metric manifest and tolerance, but fresh runner-local
   reference timings instead of the Mac's absolute measurements.
4. Fails on regression; always uploads raw candidate/reference JSON and the
   baseline. No best-of-N, retry-to-green or automatic baseline rewrite.

Shared runners are noisy: three medians mitigate, not eliminate, contention,
p99 instability and sequential-run drift. Inspect artifacts on failure. Baseline
changes require review; no hosted execution is claimed here.

`FipsdPerfLoopbackTest` requires `FIPS_ENABLE_BENCH=ON`, is labeled
`fips;perf;benchmark` and **DISABLED by default**. Enable explicitly with
`FIPSD_ENABLE_PERF_TEST=ON`, or invoke Python directly as CI does. Missing
`--rust`/`FIPSD_UDP_INTEROP_RUST_BINARY` exits **77**, not success.

## Reproduction

Use absolute paths; keep the socket scratch path short (macOS limit: 104 bytes).
No TUN, root or relays required. Example:

```sh
ROOT=/absolute/nostrc-worktree
RUST=/absolute/pinned-fips-checkout
git -C "$ROOT" submodule update --init --depth 1 third_party/nostrdb
cmake -S "$ROOT" -B "$ROOT/build-perf" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DGO_WARNINGS_AS_ERRORS=ON -DFIPS_ENABLE_BENCH=ON \
  -DBUILD_FIPS=ON -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF \
  -DBUILD_LIBHANAMI=OFF -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF \
  -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF -DSIGNET_ENABLE=OFF \
  -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF
cmake --build "$ROOT/build-perf" --target fipsd fips_benches fipsd_perf_native
(cd "$RUST" && CARGO_TARGET_DIR=/tmp/fips-rust-target-perf cargo build --locked --release --bin fips)
python3 "$ROOT/fipsd/tests/perf_run.py" --build "$ROOT/build-perf" \
  --rust /tmp/fips-rust-target-perf/release/fips --rust-commit "$(git -C "$RUST" rev-parse HEAD)" \
  --source-commit "$(git -C "$ROOT" rev-parse HEAD)" \
  --output "$ROOT/perf-results/c.json" --work-dir "$ROOT/p"
for bench in noise_aead routing_next_hop coord_cache transport_addr_clone; do
  (cd "$RUST" && CARGO_TARGET_DIR=/tmp/fips-rust-target-bench cargo bench --locked --bench "$bench")
done
python3 "$ROOT/fipsd/tests/perf_compare.py" "$ROOT/perf-results/c.json" \
  --baseline "$ROOT/fipsd/tests/perf_baseline.json"
# On another host, also pass --reference <same-runner-reference.json>, collected
# from the manifest's immutable C source using the identical current harness.
# Preserve desired results, then remove only your task-owned artifacts:
rm -rf "$ROOT/build-perf" /tmp/fips-rust-target-perf /tmp/fips-rust-target-bench
git -C "$ROOT" submodule deinit -f --all
```

## Validation

- Release/Werror; all 39 C workloads ×3; four unmodified Rust Criterion benches;
  all three real-daemon pairings ×3 (five seconds, 1000 latency exchanges).
- Debug/Werror ASan+UBSan, then
  `ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:halt_on_error=1
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
  ctest --test-dir "$ROOT/build-perf-san" -R '^Fips' --no-tests=error --output-on-failure`:
  **167 counted, 0 failed**: 160 passes, 7 skips; two additional disabled tests.
  Skips: optional control/Rust UDP/v6/TCP/multinode interop and privileged TUN/IPv6
  smoke. Disabled: soak/performance. Leak detection is off on macOS, as in CI.
- All new micro executables also ran under ASan/UBSan. An extra **one-second
  sanitizer-daemon saturation experiment failed** C/C's nonzero-delivery
  assertion, with no sanitizer diagnostic. It is not a baseline or a pass and
  was not retried to hide the failure.
- Twelve comparer tests: 10% boundary; injected 11% regressions in both directions;
  medians, missing runs/rows, invalid measurements, declared/actual workload
  mismatches and missing/wrong Rust pins. Local baseline comparison:
  66 metrics, zero regressions (serialization/aggregation check, not independent
  performance-regression evidence).
- Requested static checkers, `git diff --check`, `actionlint`: passed. Selector
  operational suite: **483 passed** after synchronizing only FIPS workflow
  accounting (existing interop/chaos job entries were absent at the base).
- No `bd`, pre-push wrapper, merge, push or shipped-target/manifest edit.
