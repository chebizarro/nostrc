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
10% in local mode. It requires at least three complete runs,
all 39 micro workloads and all three loopback pairings, exact run counts,
durations/sample counts and Rust pin. Locally (the default comparer mode) the
gates are micro medians, delivered goodput, forward/reverse one-way and RTT
p50/p99 and both daemon CPU costs per delivered datagram. Loss, micro p95 and
lifetime CPU remain reported, not gates.

**Hosted vs local policy.** On shared GitHub runners the CI bench job runs
`perf_compare.py --hosted`: it compares only against the reference built and
measured on the same runner in the same job (never against this file's absolute
Apple numbers, which document the local baseline). Loopback goodput still fails
on a loss **strictly greater than 10%**, with no noise allowance. A non-cross-thread
micro median fails only when **both** its relative increase is strictly greater
than 10% **and** its absolute increase in ns is strictly greater than:

```text
candidate_spread = max(candidate run medians) - min(candidate run medians)
reference_spread = max(reference run medians) - min(reference run medians)
observed_spread = max(candidate_spread, reference_spread)
noise_floor_ns = max(2 ns, 2 × observed_spread)
```

Each spread uses all of the already-required ≥3 runs for that metric, separately
for candidate and same-runner reference. It is not the micro p95, a standard
error/confidence interval, or a pooled range across both builds (which would
mistake a real between-build regression for noise). The 2 ns absolute floor and
multiplier **k = 2** are empirical bounds derived below, not a statistical
significance claim. Equality at either gate boundary passes. The comparer computes
spreads from the raw artifacts; missing/invalid measurements still fail closed.
The job summary shows each gated micro's effective noise floor. No new CLI flags,
workflow changes, baseline refresh or additional benchmark runs are needed.

**Hosted calibration evidence.** Failed logs and downloadable
`fips-bench-results` artifacts from runs
[44082939871](https://github.com/chebizarro/nostrc/actions/runs/44082939871),
[44301687127](https://github.com/chebizarro/nostrc/actions/runs/44301687127) and
[44410974924](https://github.com/chebizarro/nostrc/actions/runs/44410974924)
contain three candidate and three reference measurements each. Representative
false positives (ns; spread is the larger within-build max−min):

| Run | Micro workload | Reference median | Candidate median | Delta | Spread | Delta / spread |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 44082939871 | `transport_addr_clone/shared/6` | 7.939 | 8.958 | 1.019 | 1.020 | 0.999 |
| 44301687127 | `transport_addr_clone/shared/6` | 9.697 | 10.938 | 1.241 | 0.602 | 2.061 |
| 44301687127 | `transport_addr_clone/shared/18` | 8.863 | 10.939 | 2.076 | 1.438 | 1.444 |
| 44301687127 | `transport_addr_clone/shared/58` | 9.704 | 10.938 | 1.234 | 2.819 | 0.438 |
| 44301687127 | `transport_addr_clone/former_vec/18` | 13.572 | 15.040 | 1.468 | 1.783 | 0.823 |
| 44301687127 | `transport_addr_clone/former_vec/58` | 12.899 | 14.462 | 1.563 | 2.590 | 0.603 |
| 44301687127 | `transport_addr_socket_and_clone/shared/scoped_ipv6` | 74.351 | 82.398 | 8.047 | 14.752 | 0.545 |
| 44410974924 | `transport_addr_clone/shared/6` | 7.941 | 8.959 | 1.018 | 1.255 | 0.811 |
| 44410974924 | `transport_addr_cross_thread/former_vec/6` | 345.635 | 401.160 | 55.525 | 52.233 | 1.063 |
| 44410974924 | `transport_addr_cross_thread/former_vec/18` | 343.923 | 407.777 | 63.854 | 63.671 | 1.003 |
| 44410974924 | `transport_addr_cross_thread/former_vec/58` | 382.783 | 437.104 | 54.321 | 43.488 | 1.249 |

For example, run 44082939871's shared/6 reference samples were
`[7.939, 8.958, 7.938]`, candidate `[7.939, 8.958, 8.958]`: a roughly 1 ns
step becomes +12.8% at this scale. Run 44301687127's shared/6 samples were
reference `[9.697, 9.722, 9.120]`, candidate `[10.939, 10.938, 10.936]`.
Its 1.241 ns shift exceeds 2× its 0.602 ns spread, so a spread-only rule is
insufficient. Rounding that observed absolute shift up to the next whole ns
selects **2 ns**. Among the remaining non-cross-thread false positives whose
absolute shifts exceed 2 ns, the largest delta/spread is **2.076 / 1.438 =
1.444**; rounding up to the next integer selects **k = 2**. The IPv6 workload's
reference `[74.351, 72.444, 87.196]`, candidate `[82.398, 72.576, 84.556]`
then gets a 29.504 ns floor, above its 8.047 ns shift. These constants cover
all observed non-cross-thread false positives in the three runs without
blanket-exempting small workloads: a stable 200 → 280 ns micro still fails.

All micro names containing `*_cross_thread*` are **report-only in hosted mode**:
they measure thread hand-off/scheduling on shared runners, not just operation
cost. The 14.2–18.6% shifts above, with 43.488–63.671 ns run spreads, are not a
reliable hosted regression gate. Loopback latency percentiles and CPU per
datagram also remain report-only, with changes shown in the job summary (p50
more than 50% worse adds a warning annotation). Neighbour noise dominates those:
the first hosted run (`44033430020`) measured the identical pinned Rust↔Rust pair
18% apart between reference and candidate, and one build's three C↔Rust reverse
p50 runs at 86–129 us. Hosted mode now gates 36 of 66 metrics (33 non-cross-thread
micros and 3 goodput metrics). Local mode is unchanged: all 66 metrics still
use only the 10% relative gate, including cross-thread micros.

Comparing the measured dataset against its updated baseline passes all 66
metrics; this checks aggregation/serialization, **not independent regression
performance evidence**. Comparer unit tests cover deliberate real regressions,
missing/invalid measurements, pin/workload mismatches, the historical false
positives, strict relative/absolute/spread boundaries and the hosted CLI path.
Absolute Apple timings must not gate Ubuntu: CI's same-runner
reference flow rebuilds the immutable C pin and measures it beside the candidate
with the same Rust binary, workload and harness.

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

## Tick profiler

`FIPSD_ENABLE_PROFILING` is **OFF by default**. This ports the profiler at
upstream FIPS `6aa80504baa0fa312b392009eea0e3e59fb6fe4c`: `instr/{mod,recorder,
writer,capture}.rs` and `perf_profile.rs`. `instr/recorder.{c,h}` owns the
fixed-size counters, capture-generation tokens and feature-erased span macros;
`instr/writer.{c,h}` owns TSV formatting, the bounded private capture file,
control commands and writer-thread lifecycle. `node/perf_profile.{c,h}` owns
pipeline histograms and its separate, opt-in reporter. All three `.c` files
are excluded from a default build. Only macros/private gated timestamp fields
were added to dispatch, crypto, transport and endpoint paths: no timer,
packet, queue, snapshot-publication or scheduling policy was changed.

### Build and capture

Use an out-of-tree build, with both daemon and client switches. For example:

```sh
ROOT=/Users/bizarro/Documents/Projects/nostrc-wt/fips-profiler
BUILD="$ROOT/build-prof"
git -C "$ROOT" submodule update --init --depth 1 third_party/nostrdb
timeout 300 cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DGO_WARNINGS_AS_ERRORS=ON \
  -DFIPSD_ENABLE_PROFILING=ON -DFIPSCTL_PROFILING=ON \
  -DBUILD_FIPS=ON -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF \
  -DBUILD_LIBHANAMI=OFF -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF \
  -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF -DSIGNET_ENABLE=OFF \
  -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF
timeout 900 cmake --build "$BUILD" --target fipsd_tests -j 10
# Run fipsd with your ordinary config in another foreground terminal.
"$BUILD/fipsd/fipsctl" --socket "$ROOT/p/node.sock" profile tick on --dir "$ROOT/p/captures"
"$BUILD/fipsd/fipsctl" --socket "$ROOT/p/node.sock" profile tick status
"$BUILD/fipsd/fipsctl" --socket "$ROOT/p/node.sock" profile tick off
```

Like upstream, the CLI prints the control JSON data, including the capture
`path`; the TSV is written to that file, **not embedded in the control reply**.
The already-parsed `fipsctl` commands now reach real off-dispatcher
`profile_tick_on`, `profile_tick_off`, and `profile_tick_status` handlers.
The default daemon still reports them as unknown commands. A client compiled
without `FIPSCTL_PROFILING` does not accept the `profile` command.

`on` returns `state`, `path`, `interval_secs`, `byte_cap`. `status` adds `bytes`
and reports `idle`, `running`, `stopped_by_cap`, or `stopped_by_error`.
`off` returns `state`, `stopped` and, when stopping a capture, `path`, `bytes`,
`stopped_by_cap`, `stopped_by_error`. Repeated `off` is successful and reports
`stopped:false`. The file is `profile-<UTC timestamp>[-collision suffix].tsv`,
created exclusively with mode 0600; directories are created with mode 0750.
An unprivileged daemon accepts a caller-selected directory. A root daemon
confines it to `/var/log/fips`, rejecting parent components and symlink escapes
through existing ancestors (the same trusted-root/check-then-act assumption as
upstream). Open/header/thread-start failures return an error and clean up the
new file. The default directory is `/var/log/fips`.

A lazily created OS thread drains every ten seconds, stops at 32 MiB instead
of rotating, and reports sink errors as `stopped_by_error`. `off` and daemon
teardown signal a condition variable, perform a final drain and join; they do
not sleep out the flush interval. Generation tokens reject a span that began
in an earlier capture, including across immediate `off`/`on`. Disarming retires
only synchronous atomic updates, never waits for a span body or a dispatcher
fiber to finish. The recording path is allocation-free and uses lock-free
64-bit atomics. The independent fields can fall in adjacent flush intervals
at a boundary, as in Rust; unsigned totals wrap modulo 2^64, not saturation.

### Fields and section coverage

The exact upstream header keys/notes and TSV column order are preserved:

```text
ts_unix  kind  domain  name  count  max  total  unit
```

`domain=tick`; steps precede gauges in upstream declaration order. Step
`max`/`total` are wall-clock microseconds, truncated from nanoseconds, including
any wait inside a span. They are **not CPU time**. Nested spans are inclusive:
do not sum them as disjoint costs. Gauges are `ticks` (unit `ticks`), `peers`
(unit `peers`), `tick_entry_gap` and `arm_starvation` (unit `us`). Gauges have
sample count, maximum sampled value and sum, not duration columns. The first
entry has no gap but does contribute deadline lateness. Starvation is actual
entry minus the stored scheduled tick deadline, not a formula based on gaps.

C has a deadline-driven timer wheel rather than Rust's monolithic tick arm.
The names are preserved, but callback frequency and nesting reflect **actual C
work**, not fabricated Rust-shaped calls:

| Upstream names | C instrumentation |
| --- | --- |
| `whole_tick` | Existing `hs_tick` body only; peer gauge sampled afterward |
| `check_tree_state`, `check_bloom_state`, `check_pending_lookups`, `poll_probes` | Existing tick calls to tree/bloom/lookup/probe maintenance |
| `check_mmp_reports`, `check_session_mmp_reports`, `record_stats_history` | Existing tick MMP/session-MMP/history calls |
| `poll_session_rekeys`, `purge_idle_sessions` | Session tick/path-facts sweep and deadline-driven session poll; idle predicate/close is a nested per-session span |
| `compute_mesh_size` | Actual mesh-estimation calls, including history/snapshot callers |
| `check_timeouts`, `poll_pending_connects`, `process_pending_retries` | Deadline-driven leg callback, its nested pending-leg sweep, and configured retry callback |
| `drive_peer_timers`, `poll_link_rekeys` | Dispatcher peer service and actual peer `POLL_LINK` effects |
| `check_link_heartbeats` | Dead-link detection inside tick MMP maintenance |

The following upstream rows remain in the schema with zero counts because
this C dispatcher has no corresponding **tick** section: `reload_peer_acl`,
`reload_host_map`, `poll_nostr_rendezvous`, `poll_lan_rendezvous`,
`purge_expired_path_mtu`, `poll_transport_discovery`,
`sample_transport_congestion`, `activate_connected_udp_sessions`,
`poll_worker_liveness`, `debug_assert_peer_maps_coherent`. This does not assert
that the whole subsystem is absent: C connected UDP activation, for example,
is event-driven at authentication, and worker exits are channel events.
No sibling Nostr/mDNS implementation was changed to invent tick duties.
OS/debug conditional row emission follows upstream: the connected-UDP row is
Linux/macOS, worker row Unix, invariant row Debug. All other rows, even zero
ones, are emitted every interval, preserving comparison-tool compatibility.

### Pipeline report and dispatcher snapshot cost

In a profiling-enabled daemon, `FIPS_PERF=1` or `FIPS_PIPELINE_TRACE=true`
enables the pipeline reporter; `FIPS_PERF_INTERVAL_SECS` defaults to 5 and is
at least 1. Values are cached at startup. It reports to stderr, independently
of whether a tick capture is armed. With both environment flags unset there
is no reporter thread. Durations use upstream's `ns`/`us`/`ms`/`s` thresholds
and one-decimal formatting above nanoseconds:

```text
[pipe 1s] fmp_encrypt=5/s avg=3.9us p50<=8.2us p95<=8.2us p99<=8.2us max<=8.2us allmax=6.2us udp_send_wildcard=6/s
```

Each stage uses cumulative count/total/exact lifetime maximum and 48
ceil-log2 nanosecond buckets, clamped to bucket 47. Reports use interval
histogram deltas; p50/p95/p99 and `max<=` are bucket upper bounds, `allmax` is
exact lifetime maximum, and `/s` is integer interval rate. Zero-count stages
and zero-count events are omitted. All sixteen upstream stage names/order
and six event names/order are retained. C measures FMP/FSP worker crypto,
FMP worker seal queue wait, packet classification/result dispatch, originated
endpoint send and queue wait, native delivery and consumer wait, transport
queue wait, TUN unpack/queue delivery, and UDP sendmsg. Connected/wildcard
sends and connected-socket installation/failure are counted. Unused stages
(such as `udp_recv`, which has no hook in the pinned upstream either) stay
silent; no readiness wait is falsely presented as a receive syscall cost.
C's result-dispatch span includes bookkeeping that Rust structures differently;
this is report-format parity, not equal-cost or equal-scheduling claims.

The final **C-only additive pipeline stages** follow the upstream sixteen:
`publish_snapshot` times the real control snapshot `build(h, now)`, and
`snapshot_header`, `snapshot_peers`, `snapshot_sessions`,
`snapshot_transports`, `snapshot_tree`, `snapshot_cache`, `snapshot_counters`
and `snapshot_history` time its consecutive sections (together they cover
all of `publish_snapshot`). They have the same
histogram/percentile layout but are not upstream tick names. This makes the
wave-16 finding testable: compare rate, average, tail and lifetime maximum to
packet/endpoint work under load. Snapshot construction outside the tick is not
falsely charged to `whole_tick` or `record_stats_history`.

### Incremental snapshot build (nostrc-oetfk)

Before, two sections rebuilt from scratch on every publish. `snapshot_history`
allocated and zeroed `peer_capacity` (256) retained-peer ring sets of ~283 KB
each, about 72 MB, then copied the live history. `snapshot_cache` ran
`fips_coord_cache_stats` and `fips_coord_cache_iter`, each sweeping all ~51k
slots of the default (~140 MB) coordinate cache. Now:

- Snapshots retain one of two shared, immutable, ref-counted history views
  (`src/node/stats_view.c`). The dispatcher logs each history update's inputs
  (eight deep) and replays them onto a view that no snapshot holds (atomic
  refcount: acquire load against the readers' acq_rel release), so an
  unchanged history costs a retain and a changed one one tick of ring pushes.
  A refused update, history re-initialization, or a reader that holds a view
  for more than eight updates falls back to a copy sized to `peer_count`.
  `hs_stats_tick` and replay share one deterministic update function.
- The core's `fips_coord_cache_scan` marks resident slots from the three
  eviction heaps into a bitmap and visits them in storage order in one
  O(len + slots/64) pass, with the same visits and stats as iter + stats
  (`FipsCoreCoordCacheTest` cross-checks them under churn).

The published snapshot layout, the control rendering and the goldens are
unchanged. Peer/session JSON rows stay on the dispatcher: measured at tens of
microseconds, they were not the cost.

Release `-DFIPSD_ENABLE_PROFILING=ON`, C→C `perf_loopback.run_pair`,
`FIPS_PERF=1`, both nodes, three runs on a loaded M4 Max (load average about
10). "1 s" rows aggregate 36 one-second reports of 5 s runs; "10 s" rows are
six ten-second reports of 12 s runs, so each p99 is computed over 30–100
builds:

| Stage | Before avg | Before p99 / allmax | After avg | After p99 / allmax |
| --- | --- | --- | --- | --- |
| `publish_snapshot` (1 s) | 2312.7 us | ≤67.1 ms worst window; 59.5 ms | 66.8 us | ≤1.0 ms in 35/36 windows; 1.1 ms |
| `publish_snapshot` (10 s) | 1582 us | ≤16.8 ms in 6/6 windows; 14.2 ms | 70.7 us | ≤524 us in 5/6, ≤1.0 ms in 1/6; 596 us |
| `snapshot_history` (1 s) | 1799.4 us | ≤67.1 ms; 49.3 ms | 16.4 us | ≤1.0 ms; 1.0 ms |
| `snapshot_cache` (1 s) | 464.0 us | ≤16.8 ms; 9.1 ms | 1.3 us | ≤16.4 us; 9 us |
| `snapshot_peers` (1 s) | 20.3 us | ≤262 us; 201 us | 20.2 us | ≤131 us; 77 us |
| other five sections (1 s) | ≤12 us each | ≤1.0 ms; 813 us | ≤12 us each | ≤262 us; 238 us |

The remaining `publish_snapshot` tail is wall time across preemption on the
loaded host, not a section's cost. Goodput in a plain Release build
(no profiling), with base and change runs interleaved six times each, gave
medians of 672.5 Mbit/s before and 659.3 Mbit/s after (ranges 656–684 and
635–689). Both are within the 10% tolerance of the 683.856 Mbit/s baseline
below, which is unchanged.

### Snapshot follow-ups: cache generation and incremental history views (nostrc-igg4w)

Two costs of the incremental build still scaled with table size:

- `snapshot_cache` ran the O(len) `fips_coord_cache_scan` (and rebuilt up to
  256 JSON rows) on **every** publish, although most publishes follow no
  cache change. libfips-core's cache now keeps a change counter
  (`fips_coord_cache_generation`) and reports the earliest live expiry
  (`fips_cache_stats.next_expiry_ms`). The dispatcher keeps the last scan as an
  immutable, ref-counted cache section (`cache_view` in `provider.c`) shared by
  every snapshot published while the generation is unchanged and no entry has
  newly expired; only `avg_age_ms` advances, by exactly the elapsed time. Any
  insert, touch, removal or expiry rescans, so a node whose cache is touched
  by every forwarded frame still pays the scan on each publish.
- A stats-history view the replay log cannot reach (older than the log's eight
  updates, behind a refused update, or never filled) was **copied whole**:
  ~283 KB per retained peer, about 72 MB at 256 peers. libfips-core gains
  `fips_stats_history_sync`, which brings an earlier copy of a history current
  by copying only the samples pushed since (per ring, at most two `memcpy`
  segments) and any peers that arrived or were evicted; each peer carries the
  `fast_pushes` it was `born` at, so a peer evicted and later re-created is
  copied whole rather than mixed. A whole copy remains for a never-filled view
  or a re-initialized history (`fipsd_stats_views_invalidate` starts a new
  lineage). The pool grows from two views to three, the third allocated only
  when two are read at once, so a slow control client pinning a view no longer
  makes every publish copy.

On this machine (macOS arm64, M4 Max) a warm 72 MB copy is ~1.2-1.3 ms and a
freshly allocated one 4-6 ms, while a sync after 2-30 missed ticks is 40-170
us, after 100 ticks ~0.26 ms and after a thousand ~1 ms (about a full copy
once a ring's worth is missing).

`fipsd_snapshot_bench` (always built, `tests/test_snapshot_bench.c`; the
Release build is the measurement) drives the real dispatcher handlers and times
every `hs_publish()` (the `publish_snapshot` stage) on the wall clock and on the
dispatcher thread's CPU clock: 256 retained peers (new peers arrive at random
sorted positions, then most are churned out), a coordinate cache of `--cache`
resident entries touched before `--cache-churn` percent of publishes, a
history tick every tenth publish, and `--slow-clients N` control clients that
keep a snapshot for several ticks. `FIPSD_SNAPSHOT_BENCH_TEST=ON` registers
`FipsdSnapshotPublishBench`, a `--max-p99-us 1000 --clock cpu` gate (Release
only; not run by ordinary ctest).

```text
cmake -DCMAKE_BUILD_TYPE=Release -DFIPSD_SNAPSHOT_BENCH_TEST=ON ... && ninja fipsd_snapshot_bench
FIPSD_WORKERS=2 fipsd/tests/fipsd_snapshot_bench --cache 50000 --cache-churn 0
```

Release, 2000 publishes per run, base and change binaries interleaved eight
times on a shared host (load average 17-40; one run's over-1 ms counts swing
with it, so medians and counts are shown). CPU-clock figures; `p99` is of all
publishes; "over 1 ms" is publishes of 2000 above 1 ms, per run:

| Scenario (256 retained peers) | p50 before | p50 after | p99 before | p99 after | over 1 ms before / after |
| --- | --- | --- | --- | --- | --- |
| 50k-entry cache, idle | 326 us | 3.6 us | 1028 us | 28 us | 3-78 / 1-2 |
| 50k-entry cache, touched before 25% of publishes | 312 us | 4.2 us | 907 us | 539 us | 1-57 / 1-15 |
| 4096-entry cache, touched before 25% | 116 us | 3.8 us | 175 us | 140 us | n/a |
| 4096-entry cache, two slow control clients | 117 us | 3.5 us | 1377 us | 49 us | 65-66 / 4 |

The history views while the slow clients pin them: 65 whole copies before,
one after (the third view, once), 270 replays before and 592 after. The
touched-cache row is bound by the O(len) rescan the touches force: its
rescanning publishes cost ~300 us before and ~330 us after at 50k entries (the
scan now also tracks the next expiry), and the publishes that need none cost
~4 us. A touch changes only a row's `last_used`, so a rescan-free touch path
(rows patched from the live entries) is possible and not done here. The
remaining tail of the history-tick publishes is the dispatcher touching view
storage that has been idle (page-ins under this host's memory pressure), which
the benchmark cannot separate from the replay itself.

### Reproduction and verification

`FipsdProfilingTest` joins `fipsd_tests` when profiling is enabled. It covers
nested spans and exactly-once evaluation, fixed-footprint/wrapping overflow,
reset, measured sustained starvation, retired capture generations, histogram
bounds/percentiles, exact pipeline output, concurrent drain/record, private
file modes, same-second restart, final drain, idle reset, startup descriptor
cleanup, and a writer that fails **before startup returns**. Fault injection
is in a separate test-only object, not production knobs. `FipsdProfilingE2eTest`
requires both profiling switches, builds its native load helper even without
`FIPS_ENABLE_BENCH`, and runs two real daemons through the existing
`perf_loopback.run_pair` IK/XK/native-API workload for three seconds. It calls
the real `fipsctl` on/status/off, requires live tick counts and compares TSV
structure and pipeline field layout against the captured Rust fixtures.
Root runs explicitly skip this unprivileged temporary-directory test.

`FipsdProfilingZeroCostTest` is registered instead in a default build. It
requires byte-identical feature-off/uninstrumented objects even at `-O0`,
uses deliberately undefined clock/gauge arguments to prove non-evaluation,
checks timestamp fields vanish from structures, and checks the actual
`fipsd_lib` archive contains no `fipsd_instr_*` or `fipsd_perf_*` symbols.

The reference fixtures in `tests/profile_fixtures/` are **real captured output**
from the locked upstream Release `--features profiling` build, not copied C
expectations. A separate Rust/Rust loopback run also passed the same shape
comparison. Comparison normalizes values only (not keys, names, units, notes
or row ordering), with the explicit Debug invariant-row difference.

Example from the local macOS arm64 Debug ASan/UBSan C/C loopback run (three
seconds, one peer, wall times include sanitizer/instrumentation overhead):

```text
# fipsctl profile tick on
{"byte_cap":33554432,"interval_secs":10,"path":".../profile-20261117T020317Z.tsv","state":"running"}
# fipsctl profile tick off
{"bytes":2454,"path":".../profile-20261117T020317Z.tsv","state":"idle","stopped":true,"stopped_by_cap":false,"stopped_by_error":false}
# selected actual TSV rows, tab-separated
1794881000  step   tick  record_stats_history  3  1081  1084  us
1794881000  step   tick  whole_tick            3  2684  5699  us
1794881000  gauge  tick  ticks                 3  1     3     ticks
1794881000  gauge  tick  peers                 3  1     3     peers
1794881000  gauge  tick  arm_starvation        3  147   301   us
```

These samples are diagnostic examples, not a new performance baseline or a
claim that tick time accounts for every dispatcher activity.

A later C/C ASan run also produced this actual snapshot-stage segment (the
rest of the same pipeline line contains the original stage fields):

```text
publish_snapshot=3/s avg=13.7ms p50<=16.8ms p95<=33.6ms p99<=33.6ms max<=33.6ms allmax=63.9ms
```

Final local verification used Debug/Werror builds, built `fipsd_tests` plus
**every** libfips-core test executable (including `fips_core_skeleton_test`),
and ran `ctest -R '^Fips' -E 'Soak|Perf' --no-tests=error --output-on-failure`:

| Configuration | Result |
| --- | --- |
| Default, ASan/UBSan | 192 counted: 183 passes, 9 skips, 0 failures; 190.69 s |
| Default, TSan | 192 counted: 183 passes, 9 skips, 0 failures; 333.16 s |
| Profiling, ASan/UBSan full selection | 193 counted: 184 passes, 9 skips, 0 failures; 190.43 s |
| Final profiling unit + e2e, ASan/UBSan | 2/2 pass; 5.28 s |
| Final profiling unit + e2e, TSan | 2/2 pass; 9.68 s |

ASan used `detect_leaks=0:strict_string_checks=1:halt_on_error=1` and UBSan
`print_stacktrace=1:halt_on_error=1`; this is not an LSan pass on macOS. TSan
used `halt_on_error=1`. The nine skips are the two privileged smoke tests and
seven optional Rust-interop gates, whose CMake binary bindings were not set;
they are not counted as passes. The separate real Rust capture and Rust/Rust
load/shape checks described above did run.

Earlier complete TSan runs (default and profiling) exposed the already-known
`FipsdNostrTraversal_guards` race in `relay.c:1064`, global
`write_operations.h_ws_write_ns` (`nostrc-xqwgo`). That code was not changed,
suppressed or excluded. The final default suite after the feature-off macro
fix did not trigger it; the final focused profiling TSan tests were clean.
An earlier feature-off include error and the strict `-O0` identity check's
finding of branch-emitting `do/while(0)` no-op macros were fixed, not worked
around: disabled macros now expand to nothing or the measured expression.

Direct static gates passed: `check-unsequenced-args.py --root <worktree>`,
`check-gui-test-gdk-frame.py` (55 sources), `check-linux-ci-packages.py`, and
`git diff --check`. No workflow files, tracker state or pre-push wrapper were
changed/run. The manifest keeps fipsd and fipsctl at 0.1.0 Unreleased.
