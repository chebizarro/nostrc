# FIPS performance baseline

## Dataplane correction: nostrc-pxwaj

Measured **2026-11-16** on the same Apple M4 Max workstation as the original
baseline. These are local macOS measurements, not hosted/Linux qualification.
The before columns are the original three-run medians at
`d51dd48ee3ba65c76d484cf50c1a954dcf3cd51c`; the after columns are new three-run
medians at `ffca1748e48b87267105189e515619a600a686fd`, which includes the
performance fix `59a309351` and the ownership/barrier review fixes. No best-of-N
selection: an initial measurement before the barrier follow-up was superseded
by a complete measurement of the final shipped source.

### Root cause and ownership review

The original sender profile recorded in `59a309351` attributed 94% of dispatcher
fiber time to `hs_poll -> hs_publish -> build`: originated sends, completions and
MMP reports rebuilt the complete control snapshot about once per millisecond.
Each build allocated/copied multi-megabyte stats history, swept coordinate
caches and generated control rows. The hot dispatcher could not drain its
64-slot command mailbox. The native reader gave up after 64 yields, dropping
accepted traffic, while crypto queue refusal burned already-reserved nonces.
This profile was recorded by the preceding investigation, not re-collected here.

The fix coalesces dirty-driven control snapshots over 50 ms, publishes packet
counters from the maintenance tick, batches at most 64 dispatcher events per
wake and sizes crypto queues to the 256-slab packet pool. Command admission
uses atomic in-flight accounting and leaves 64 slabs for inbound opens. The
native flow reader blocks on its mailbox with halt-channel cancellation;
shutdown closes that channel before joining and freeing readers. Native sink
and crypto workers drain bounded bursts rather than yielding per packet.

Review retained the **single dispatcher writer** for protocol/session state,
replay acceptance and nonce reservation. Workers receive owned slabs and cloned
cipher keys, not mutable session authority. Reserved nonces remain burned on
failure; they are never rolled back. Three defects were corrected:

1. `on_command` inspected `c->kind` after freeing its owned command. The dirty
   check now precedes free. The installed callback is exercised under ASan.
2. Packet recycle read `p->plane` after releasing `busy`, when the next take
   could already write it. Recycle now captures the plane before transferring
   ownership and never accesses slab fields afterward. A 10,000-cycle concurrent
   recycle regression and exact pool/admission-boundary assertions cover it.
3. Batching/coalescing broke control-command snapshot barriers: existing control
   cancellation and DNS tests observed stale connection/identity snapshots after
   a reply. A claimed control command now publishes **before resolving its
   ticket**. Canceled queued controls still neither execute nor build replies.

Dirty state is not cleared until a successful snapshot publication. The timer
wakes at the deferred deadline; state-change notices still synchronously publish
before becoming observable. An injected-clock regression checks coalescing,
exact wake deadlines, state-notice bypass and non-send dirty marking. A full
mailbox/cancellation regression checks refused-send ownership without sleeps.
The independent inline code review found no additional actionable issue; it did
not inspect a checkout or independently execute the tests.

### Follow-up: responder return-route lifetime under batching

A subsequent repeated real-UDP ring check exposed a route-lifetime bug that the
original one-shot full suite missed. Temporary tracing reproduced `missing
datagram 3 -> 0`: n3 learned n0's setup coordinates as an **unfunded probation
hint**, installed its responder session on successful XK msg3 with no application
delivery, then lost the hint to unrelated probation churn. At the failed route
lookup the live cache also lacked n0 (`live=0`, `needs_publish=0`); this was **not
control coalescing delaying a dataplane snapshot**. `fipsd_dp_route` already
publishes requested dataplane changes before every route lookup.

The missing operation was promotion after initial responder proof: msg3 has no
coordinate statement and need not deliver a CP-bearing application frame, so
neither existing `stated` intake path promoted the retained setup coordinates.
Faster batched dispatch exposed the probation-eviction ordering. The fix calls
`fipsd_dp_stated` for the retained return coordinates when the real executor first
installs a responder session, before SESSION_UP or pending-send release. Setup
intake before proof remains carrier-funded; after proof these are still hints,
not signed/verified coordinates. The existing dataplane dirty flag and route
publication barrier make the promotion visible before dependent routing. Control
snapshot coalescing, dispatcher batching and funding defaults are unchanged.

`FipsdNodeMultiReturnRouteTest` uses a three-node real-UDP line. A dispatcher-side
test action permanently exhausts the responder carrier's coordinate funding,
then the production probe completes a real indirect XK session. A second
single-writer action injects more unrelated unfunded hints than probation can
hold, verifies live-cache retention and snapshot next-hop selection, followed by
a real encrypted reverse datagram. It fails at the post-churn cache assertion
on the unfixed source (2.37 s), and passes with the promotion. No sleeps, send
retries, longer deadlines or enlarged queues were added; temporary tracing was
removed. The inline follow-up review reported no further actionable finding.

Follow-up verification (Debug/Werror, foreground bounded runs):

- `ctest -R FipsdNodeMulti --repeat until-fail:15`: Line, Star, Ring, FullMesh
  **and ReturnRoute each 15/15**, under ASan+UBSan (509.44 s) and TSan (633.28 s).
- `FipsdNativeBurstTest --repeat until-fail:10` under TSan: **10/10**, 24.31 s.
- Full `ctest -R '^Fips' -E 'Soak|Perf'`: **178 counted, 170 passes, 8 capability
  skips, 0 failures** under each sanitizer; ASan+UBSan 177.05 s, TSan 299.02 s.
  The same optional interop/privileged skips and sanitizer options documented
  below apply. The known unrelated `relay.c:1064` TSan race did not fire.
- Fresh Release/Werror `perf_loopback.py --runs 1 --seconds 5
  --latency-samples 1000`, pinned locked Rust daemon unchanged: C-to-C
  **686.556 Mbit/s**, **0.091% loss**, forward p50/p99 **78/133 us**, reverse
  **80/134 us**. Contemporaneous Rust-to-Rust **861.036 Mbit/s**, Rust/C **1.254x**;
  C-to-Rust **672.998 Mbit/s**. This is one smoke run, **not a new median
  baseline**; the three-run baseline below is unchanged.
- All three requested static checkers and `git diff --check` pass. Build and
  scratch directories and the separate Rust target are removed after checking;
  submodules are deinitialized. No merge or push.

## Machine and workload

- Apple M4 Max, arm64, 14 logical CPUs, 36 GiB RAM, macOS 27.0.1 (26A434),
  Darwin 27.0.0; no exclusive CPU reservation or affinity.
- Apple Clang 21.0.0, Release `-O3 -DNDEBUG`, `GO_WARNINGS_AS_ERRORS=ON`, no
  sanitizers; libsodium 1.0.22, secp256k1 0.7.1, OpenSSL 3.6.5.
- Rust `6aa80504baa0fa312b392009eea0e3e59fb6fe4c`, matching
  [UPSTREAM.md](../../libfips-core/UPSTREAM.md), built with
  `cargo build --locked --release --bin fips`, rustc/cargo 1.94.1. Its existing
  unused `IdentityCache::told` warning remains. No Rust source or lockfile edits.
- UDP `127.0.0.1`, `FIPSD_WORKERS=2`, `TOKIO_WORKER_THREADS=2`, TUN/DNS off,
  native API on, distinct benchmark keys, rekey after 3600 s.

`perf_run.py` invokes all four C microbenchmarks three times, then
`perf_loopback.py` rotates the three daemon pairings over three fresh runs.
Readiness observes IK/tree/bloom and established/proven XK sessions with matching
identities and complementary roles; no readiness sleep substitutes for events.
The traffic driver links real `libfipsnative`. Python is lifecycle orchestration,
not the hot loop. After 100 warm-up round trips, 1000 serial 1200-byte exchanges
measure each direction with the same monotonic clock: **not RTT/2**. Saturation
is a separate five seconds of unpaced offered load with a draining receiver.
Only locally unaccepted EAGAIN/ENOBUFS sends are retried; accepted datagrams are
not retransmitted. Payloads/duplicates are checked; reordering is allowed.

Goodput is `received * 1200 * 8 / 5 / 1e6`; loss includes packets still in flight
at the deadline, without a drain sleep. This is not maximum lossless capacity.
CPU gates use daemon traffic CPU divided by delivered datagrams; client and
`ps` CPU are not charged to the daemons. Each reported cell is independently
the median of three run values, including loss percentages and percentiles.

## Real-daemon before / after

| Pair | Before Mbit/s | After Mbit/s | Before loss % | After loss % | Before forward p50/p99 us | After forward p50/p99 us |
|---|---:|---:|---:|---:|---:|---:|
| c-c | 0.204 | 683.856 | 98.97 | 0.232 | 1584 / 2025 | 76 / 119 |
| c-rust | 0.207 | 664.343 | 98.97 | 0.284 | 621 / 792 | 69 / 120 |
| rust-rust | 817.661 | 1029.698 | 51.46 | 36.826 | 62 / 101 | 62 / 112 |

| Pair | After reverse p50/p99 us | After RTT p50/p99 us | Delivered Mbit/s, runs 1/2/3 | Loss %, runs 1/2/3 |
|---|---:|---:|---:|---:|
| c-c | 78 / 128 | 155 / 236 | 683.856 / 682.493 / 686.498 | 0.073 / 0.232 / 0.394 |
| c-rust | 72 / 122 | 142 / 220 | 664.022 / 667.824 / 664.343 | 0.284 / 0.119 / 2.718 |
| rust-rust | 62 / 108 | 125 / 201 | 1029.698 / 994.184 / 1093.884 | 36.826 / 39.370 / 32.914 |

**C-to-C acceptance is met:** 683.856 Mbit/s versus Rust-to-Rust 1029.698
Mbit/s (Rust/C = 1.506, within 2x), 0.232% median loss (every C-to-C run below
1%), and forward/reverse one-way p50 76/78 us (below 200 us). C-to-Rust median
loss is below 1%, but one run lost **2.718%**; it is not claimed lossless or
uniformly below 1%. Rust-to-Rust retains substantial unpaced overload loss.
The results and original profile support the dispatcher bottleneck, not a general
claim that C primitives now outperform Rust or that all queue drops are gone.

## All four C microbenchmarks: before / after

Targets `fips_bench_{noise_aead,routing_next_hop,coord_cache,transport_addr}`
require `FIPS_ENABLE_BENCH=ON`, are not installed and are not ctest tests. Each
process warms ten batches then measures 101 batches of 1000 operations with
`CLOCK_MONOTONIC`. CSV: `name,median_ns,p95_ns,ops_per_s`; p95 is nearest rank of
**batch-average** ns/op, not per-operation latency. Below are medians across
three process runs of each estimate; no Rust Criterion rerun is claimed.

Noise uses real XK setup outside timing, 64/1400-byte payloads, 16-byte AAD,
1024 presealed packets, replay reset on wrap, session/worker seal/open and cipher
handout. Routing uses 8/32/128/256 peers with eight-entry paths and compares the
borrowed production selector to the former allocating control. Cache has
1000/50000 entries and verified 4096 hits/misses. Addresses cover 6/18/58-byte
clones, IPv4/IPv6/scoped construction and producer/consumer transfer through a
bounded 256-item channel; cross-thread numbers include scheduler/channel costs.
Former controls are not shipped paths. No primitive algorithm changed here;
the largely unchanged timings support the dispatcher root cause.

| Workload | Before median ns | After median ns | After p95 ns |
|---|---:|---:|---:|
| `coord_cache/get_hit/1000` | 17 | 17 | 18 |
| `coord_cache/get_hit/50000` | 27 | 26 | 29 |
| `coord_cache/get_miss/1000` | 11 | 11 | 12 |
| `coord_cache/get_miss/50000` | 16 | 15 | 18 |
| `find_next_hop/current_borrowed/128` | 17621 | 17612 | 17905 |
| `find_next_hop/current_borrowed/256` | 35327 | 35124 | 35602 |
| `find_next_hop/current_borrowed/32` | 4390 | 4380 | 4480 |
| `find_next_hop/current_borrowed/8` | 1086 | 1095 | 1154 |
| `find_next_hop/former_allocating/128` | 18555 | 18557 | 18800 |
| `find_next_hop/former_allocating/256` | 37899 | 38038 | 38622 |
| `find_next_hop/former_allocating/32` | 4680 | 4646 | 4790 |
| `find_next_hop/former_allocating/8` | 1178 | 1175 | 1243 |
| `handout/send_cipher_clone` | 6 | 6 | 7 |
| `session/open/1400` | 2182 | 2173 | 2264 |
| `session/open/64` | 291 | 283 | 310 |
| `session/seal/1400` | 2092 | 2080 | 2160 |
| `session/seal/64` | 201 | 199 | 215 |
| `transport_addr_clone/former_vec/18` | 10 | 10 | 16 |
| `transport_addr_clone/former_vec/58` | 11 | 11 | 18 |
| `transport_addr_clone/former_vec/6` | 10 | 10 | 16 |
| `transport_addr_clone/shared/18` | 5 | 5 | 6 |
| `transport_addr_clone/shared/58` | 5 | 5 | 7 |
| `transport_addr_clone/shared/6` | 5 | 5 | 6 |
| `transport_addr_cross_thread/former_vec/18` | 68 | 68 | 82 |
| `transport_addr_cross_thread/former_vec/58` | 66 | 68 | 87 |
| `transport_addr_cross_thread/former_vec/6` | 60 | 58 | 73 |
| `transport_addr_cross_thread/shared/18` | 57 | 58 | 68 |
| `transport_addr_cross_thread/shared/58` | 56 | 56 | 77 |
| `transport_addr_cross_thread/shared/6` | 58 | 58 | 72 |
| `transport_addr_socket_and_clone/former_vec/ipv4` | 170 | 174 | 190 |
| `transport_addr_socket_and_clone/former_vec/ipv6` | 170 | 170 | 194 |
| `transport_addr_socket_and_clone/former_vec/scoped_ipv6` | 344 | 346 | 372 |
| `transport_addr_socket_and_clone/shared/ipv4` | 28 | 27 | 31 |
| `transport_addr_socket_and_clone/shared/ipv6` | 25 | 27 | 30 |
| `transport_addr_socket_and_clone/shared/scoped_ipv6` | 54 | 52 | 57 |
| `worker/open/1400` | 2196 | 2191 | 2286 |
| `worker/open/64` | 285 | 285 | 309 |
| `worker/seal/1400` | 2110 | 2098 | 2170 |
| `worker/seal/64` | 205 | 207 | 225 |

## Baseline/comparer policy

[`perf_baseline.json`](../tests/perf_baseline.json) now pins the measured shipped
C source `ffca1748e48b87267105189e515619a600a686fd` and unchanged Rust revision,
workload, machine/build metadata and 66 aggregated metrics. The tolerance stays
10%; the comparer was not weakened. It requires at least three complete runs,
all 39 micro workloads and all three loopback pairings, exact run counts,
durations/sample counts and Rust pin. Gates are micro medians, delivered
goodput, forward/reverse one-way and RTT p50/p99 and both daemon CPU costs per
delivered datagram. Loss, micro p95 and lifetime CPU remain reported, not gates.

Comparing the measured dataset against its updated baseline passes all 66
metrics; this checks aggregation/serialization, **not independent regression
performance evidence**. The 12 comparer unit tests pass, including deliberate
11% regressions, missing/invalid measurements and pin/workload mismatches.
Absolute Apple timings must not gate Ubuntu: CI's existing same-runner
reference flow rebuilds the immutable C pin and measures it beside the candidate
with the same Rust binary, workload and harness. No workflows were edited, and
no hosted execution is claimed. Shared-runner contention and p99 noise remain.

`FipsdPerfLoopbackTest` requires benchmarks and is disabled by default; invoke
the harness directly or opt in with `FIPSD_ENABLE_PERF_TEST=ON`. Missing Rust
exits 77, not success.

## Verification and limits

- Release/Werror: all four C microbenchmarks (39 workloads total) x3; all three
  real-daemon pairings x3 with the pinned locked Release Rust daemon.
- Out-of-tree Debug/Werror ASan+UBSan:
  `ctest -R '^Fips' -E 'Soak|Perf' --no-tests=error --output-on-failure`:
  **177 counted, 169 passes, 8 capability-gated skips, 0 failures**, 169.51 s.
  `ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:halt_on_error=1`,
  `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`. macOS leak detection off,
  not a leak-sanitizer pass. Initial suite had three snapshot-barrier assertion
  failures (control cancellation and both DNS end-to-end variants); the shipped
  barrier fix resolved them, followed by the complete green suite.
- Out-of-tree Debug/Werror TSan, `TSAN_OPTIONS=halt_on_error=1`, same selection:
  **177 counted, 169 passes, 8 skips, 0 failures**, 291.86 s. The known unrelated
  `relay.c:1064` race in `FipsdNostrTraversal_guards` (`nostrc-xqwgo`) did not fire.
- `ctest -R '^FipsdNativeBurstTest$' --repeat until-fail:10` under TSan:
  **10/10 passes**, 24.09 s. Real native APIs deliver 1024 datagrams exactly once
  with zero counted drops. No retry-to-green or sleep-based tests were added.
- Skips in each full sanitizer suite: optional control, UDP v4/v6, TCP, multinode
  and fipsctl Rust interop; privileged TUN and IPv6 smoke. Rust was used for the
  measured loopback pairings, not configured into these optional interop gates.
  Soak and performance tests were explicitly excluded, not counted as passes.
- Static checks passed: `check-unsequenced-args.py --root <worktree>`,
  `check-gui-test-gdk-frame.py`, `check-linux-ci-packages.py`, `git diff --check`.
- fipsd remains 0.1.0 Unreleased (PATCH-class corrections folded into the initial
  release); libfips-core/libfipsnative/libgo unchanged. No bd, hook wrapper,
  workflow edits, merge or push. Build/scratch directories and the Rust target
  are removed and submodules deinitialized after verification.

## Reproduction

All commands run in the assigned worktree; the upstream Rust checkout is
read-only except Cargo's explicitly separate target directory. Commands and
suites were run in the foreground with bounded timeouts and awaited. Example:

```sh
ROOT=/Users/bizarro/Documents/Projects/nostrc-wt/fips-perf-fix
RUST=/Users/bizarro/Documents/Dev/fips
BUILD="$ROOT/build-perf"
git -C "$ROOT" submodule update --init --depth 1 third_party/nostrdb
timeout 300 cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DGO_WARNINGS_AS_ERRORS=ON -DFIPS_ENABLE_BENCH=ON \
  -DBUILD_FIPS=ON -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF \
  -DBUILD_LIBHANAMI=OFF -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF \
  -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF -DSIGNET_ENABLE=OFF \
  -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF
timeout 900 cmake --build "$BUILD" --target fipsd fips_benches fipsd_perf_native -j 10
(cd "$RUST" && timeout 900 env CARGO_TARGET_DIR=/tmp/fips-rust-target-perffix2 \
  cargo build --manifest-path "$RUST/Cargo.toml" --locked --release --bin fips)
timeout 900 python3 "$ROOT/fipsd/tests/perf_run.py" --build "$BUILD" \
  --rust /tmp/fips-rust-target-perffix2/release/fips \
  --rust-commit "$(git -C "$RUST" rev-parse HEAD)" \
  --source-commit "$(git -C "$ROOT" rev-parse HEAD)" \
  --output "$ROOT/perf-results/final.json" --work-dir "$ROOT/p"
python3 "$ROOT/fipsd/tests/perf_compare.py" "$ROOT/perf-results/final.json" \
  --baseline "$ROOT/fipsd/tests/perf_baseline.json"
# Debug sanitizer builds use the same component switches, a fresh build directory,
# -DCMAKE_BUILD_TYPE=Debug and either -DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON
# or -DGO_ENABLE_TSAN=ON. Build fipsd_tests and every libfips-core test executable
# (including fips_core_skeleton_test) before running the ctest selections above.
rm -rf "$ROOT/build-perf" "$ROOT/build-perffix-asan" "$ROOT/build-perffix-tsan" \
  "$ROOT/p" /tmp/fips-rust-target-perffix2
git -C "$ROOT" submodule deinit -f --all
```
