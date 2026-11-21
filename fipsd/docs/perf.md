# FIPS performance baseline

## Deep optimization pass: nostrc-u19cz (summary)

This pass ran from 2026-11-19 to 2026-11-20, before fipsd 0.1.0. It started
from a profile showing that in each daemon one OS thread did all packet work:
the crypto worker fibers shared the dispatcher's scheduler thread, and libsodium
ran its portable reference ChaCha20. The fixes, each detailed in its own
section below:

| Change | Where |
|---|---|
| Paced sustainable-rate search and traffic-window CPU µs/datagram | [Paced sustainable rate](#paced-sustainable-rate) (u19cz.1) |
| OpenSSL EVP AEAD for payloads ≥ 256 B | `libfips-core/README.md`, Noise audit addendum B (u19cz.2) |
| Crypto on OS threads, worker-side UDP send behind a per-peer send gate, fused inbound open | [Dataplane threads](#dataplane-threads-nostrc-u19cz3) (u19cz.3) |
| Batched native delivery with direct client writes | [Native delivery](#native-delivery-nostrc-rpqvh) (rpqvh) |
| Cached per-subsystem deadlines | [Dispatcher timer scans](#dispatcher-timer-scans-nostrc-w3tfz) (w3tfz) |
| Count-bounded rekey-output wipes; cached non-blocking check in libgo | Noise audit addendum R-1, libgo README (02p6s, bmsti) |
| Linux `recvmmsg`, batched `sendmmsg`/UDP GSO | [UDP syscall batching on Linux](#udp-syscall-batching-on-linux-nostrc-u19cz4) (u19cz.4) |
| Inline crypto under a short backlog; libgo skips redundant idle wakes | u19cz.14 section below (u19cz.14) |

The final head-to-head against the pre-pass baseline `5b3c987c8`, on macOS and
on the Linux VM with Rust↔Rust as the control in every run, is tabulated in
[`release-notes-0.1.0.md`](release-notes-0.1.0.md#performance). On an M4 Max:
- C↔C unpaced goodput went from 594–680 to 819–1,324 Mbit/s.
- The sustainable rate (<1% loss) went from 244–650 to 506–1,200 Mbit/s.
- CPU per datagram at 100 Mbit/s fell from about 60 to about 41 µs.

On the Linux VM:
- Unpaced goodput went from 494–540 to 1,009–1,163 Mbit/s.
- The sustainable rate went from 394 to 800 Mbit/s.
- CPU per datagram at 100 Mbit/s went from 85/70 to 60/53 µs (tx/rx). Rust is
  54–68/70–99 µs in the same runs.

Follow-ups are filed under the epic.

## Paced sustainable rate

Use a Release build (the build command below deliberately disables unrelated apps):

```sh
cd /Users/bizarro/Documents/Projects/nostrc-wt/u19cz-1-paced-harness
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DFIPS_ENABLE_BENCH=ON -DBUILD_FIPS=ON -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF -DBUILD_LIBHANAMI=OFF -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF -DSIGNET_ENABLE=OFF -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF
cmake --build build-bench --parallel 6 --target fipsd fipsd_perf_native
caffeinate -i python3 fipsd/tests/perf_loopback.py \
  --fipsd "$PWD/build-bench/fipsd/fipsd" \
  --rust /tmp/fips-rust-target-perffix/release/fips \
  --native-tool "$PWD/build-bench/fipsd/tests/fipsd_perf_native" \
  --runs 1 --paced --paced-seconds 3 --loss-threshold .01 \
  --paced-resolution .02 --latency-samples 300 \
  --work-dir /tmp/u19cz1-perf --output /tmp/u19cz1-paced.json
```

For a fixed-load efficiency comparison, use `--paced-rate 100` **instead of**
`--paced`. This runs exactly one 100 payload Mbit/s probe per pairing, without
bracketing or bisection, using `--paced-seconds` for its traffic window:

```sh
cd /Users/bizarro/Documents/Projects/nostrc-wt/u19cz-1-paced-harness
caffeinate -i python3 fipsd/tests/perf_loopback.py \
  --fipsd "$PWD/build-bench/fipsd/fipsd" \
  --rust /tmp/fips-rust-target-perffix/release/fips \
  --native-tool "$PWD/build-bench/fipsd/tests/fipsd_perf_native" \
  --runs 1 --paced-rate 100 --paced-seconds 3 --latency-samples 300 \
  --work-dir /tmp/u19cz1-fixed --output /tmp/u19cz1-fixed.json
```

Fixed-rate rows report `paced_rate_mbps`, `target_mbps`, `offered_mbps`,
`throughput_mbps`, `loss_fraction`, latency and CPU, but do not claim a
`sustainable_mbps`. Loss is reported even if it exceeds the search threshold;
check actual offered rate and delivery before comparing CPU efficiency.

`caffeinate -i` is macOS-specific; omit it on Linux. The pinned Rust binary is
an input, not rebuilt by this harness. Pair order rotates as in unpaced mode.
Each pairing keeps **one pair of daemons** throughout its search; each probe
opens fresh native flows on a distinct port, warms with 100 bidirectional
exchanges, measures for `--paced-seconds` (default 3), then drains for 200 ms.
The drain is outside the offered interval but its deliveries count. Packets
arriving later count as lost; distinct flows prevent contamination of the next
probe. Flow/listener teardown is asynchronous, hence distinct probe ports.

The search starts at 100 payload Mbit/s, doubles to a failing upper bracket
(or `--paced-max-mbps`, default 3200), halves if necessary down to 1 Mbit/s,
then bisects until bracket width / passing rate <= `--paced-resolution`
(default .02). A pass requires loss **strictly below** `--loss-threshold`
(default .01, a fraction) and actual offered rate within 2% of target. These
are finite-window observations, assuming approximately monotonic loss, not
statistical confidence bounds or a guarantee of indefinitely sustainable load.
A noisy early failure can underestimate capacity; use several rotated runs on
an otherwise idle machine for comparisons, not the best probe from many runs.

Read each pairing's JSON as follows:

- `sustainable_mbps`: highest passing **actual offered** payload rate; the
  selected row's `throughput_mbps` is its delivered goodput. `sustainable_loss`
  is its loss fraction. Null means no passing probe, not zero loss.
  `search_ceiling_reached` means only a lower bound was established; raise the
  ceiling to obtain a failing bracket.
- `probes`: every target, actual `offered_mbps`, delivered `throughput_mbps`,
  `sent`, `accepted`, `received`, `loss_fraction`, local backpressure, CPU,
  and pass/fail. In paced mode `sent` counts **attempted offers**, including
  native API refusals; `accepted` counts successful API sends. Loss includes
  both local refusals and downstream loss. A sender that cannot produce the
  requested load must not get credit for that target. Unpaced `sent` and the
  driver's default JSON layout are unchanged.
- `loaded_one_way`: same-clock p50/p99 microseconds sampled during paced load,
  with `loaded_latency_samples` showing the actual count. Missing samples give
  null. Existing `one_way`, `reverse_one_way`, and `rtt` remain serial **pre-load**
  latency observations, not latency under the sustainable load.
- `cpu[0/1].traffic_cpu_s`: daemon CPU seconds between traffic boundary
  markers. `us_per_delivered_datagram` is this delta × 1e6 / received datagrams,
  in both paced and unpaced modes. Setup, warm-up, serial latency, process
  startup/exit and the 200 ms drain are outside the marked send window. The
  denominator still includes datagrams received during the drain; its CPU is
  deliberately excluded from the headline traffic-window cost.
- `native_counters`: per daemon (sender first), the non-zero changes of its
  `show_metrics` `native.*` counters across the probe, e.g. the receiver's
  `native.drop_flow_queue_full`; null where a daemon does not answer
  `show_metrics`. The control snapshot is read after the drain, so a late
  datagram can land on either side of it.
- `us_per_delivered_datagram_run` retains the former whole-driver CPU cost for
  comparison. Existing `traffic_s`, `traffic_percent_one_core` and
  `traffic_wall_s` retain their whole-run meanings, so older gate metrics are
  unchanged. No received datagrams gives null normalized CPU values.

The harness enables `FIPSD_PERF_CPU_MARKERS=1`, reads flushed START/END lines
from the driver's stderr using `Popen`, and samples both daemon processes on
those events. The driver waits for a newline acknowledgement on stdin at each
marker; the first send cannot precede the start CPU samples, and the end marker
is emitted after the send deadline, before waiting for the receiver's drain.
Direct driver callers do not enable this handshake and stdout remains unchanged.
There are no sleeps or polling loops in CPU sampling. The subprocess retains a
bounded timeout and is killed/reaped on timeout or sampling failure. Boundary
observations still have IPC/scheduling and sequential `ps` sampling skew; macOS
CPU accounting is quantized. This is event-bounded process accounting, not an
atomic cross-process CPU-clock snapshot. Short probes magnify those errors.
CPU excludes the traffic driver and kernel work not charged to the daemons.

`--pairs` (default `c-c,rust-rust,c-rust`) restricts the pairings, which
still rotate across runs; the default output is unchanged.

The native driver's optional arguments are
`LEFT_API RIGHT_API RIGHT_NPUB SECONDS LATENCY_SAMPLES [OFFERED_MBPS [PORT]]`.
Pacing uses monotonic **absolute** deadlines in batches covering at most 50 us
(up to 64 datagrams), with busy waiting between batches. No per-packet sleep:
macOS timer wake-up granularity would otherwise cap the offered rate. The
sender consumes a core, has microbursts within a batch, and may catch up after
scheduler delays; it cannot promise wire-level uniform spacing or real-time
accuracy under contention. Actual offers, API acceptance and delivery are
reported separately. Rate units always exclude transport/crypto overhead.

`perf_run.py --paced` (search) or `--paced-rate MBPS` (fixed load) accepts the
same paced configuration flags and
collects an **additional** `paced_loopback` artifact alongside the unchanged
unpaced gate workload. `perf_compare.py` prints sustainable rate/loss, paced
CPU and unpaced CPU microseconds as **report-only diagnostics** (also in its
markdown summary). No new failure metric or hosted-gate policy is introduced;
raw per-probe and latency details remain in the JSON. No component version
bump: these are test/harness/documentation changes, not shipped daemon APIs.

## Inline crypto at moderate rates: nostrc-u19cz.14

Measured **2026-11-20** on the shared Linux VM of nostrc-u19cz.4 (below:
Ubuntu 24.04 aarch64 under QEMU/HVF, 10 vCPU, gcc 13.3 Release,
`FIPSD_WORKERS=2`). The current tip `9e78793e9` and this branch were built side
by side and run alternating, with the pre-pass baseline `4ff8ca4f2` and the
Rust daemon (every invocation's Rust-to-Rust pair is the noise control). Every
run is reported. macOS (M4 Max, load average 6-18) was checked for
regressions. At a fixed 100 Mbit/s the Linux C sender had become dearer than
before the pass (95-100 us per delivered datagram against 85-88 for
`4ff8ca4f2` and 55-57 for Rust). It is now 59-63.

### What the instrumentation showed

A temporary counter build of the current tip (not committed) counted every
`fipsd_dp_submit` outcome per lane, worker condvar sleeps and signals, and the
dispatcher selects that found every queue empty. `perf stat --per-thread`
(`context-switches`, `cpu-migrations`, `syscalls:sys_enter_futex`,
`raw_syscalls:sys_enter`, `sched:sched_waking`) covered 6 s inside an 8 s c-c
probe at 100 Mbit/s and `perf record -g` 4 s. Per-thread CPU came from `/proc`
over the same sub-window. On this VM `perf`'s task-clock reads about 30% below
`/proc` utime+stime, which the harness uses, so every CPU figure here is the
harness's. c-c at 100 Mbit/s, current tip, out of 83.5k datagrams:

| Job | Inline | To a worker | Why not inline |
|---|---:|---:|---|
| sender session seal | 867 | 82,621 | next native datagram already in the mailbox 58,740; worker busy 20,568; completions queued 3,313 |
| sender link seal + UDP send | 449 | 83,067 | completions queued 66,532; worker busy 12,500; mailbox 4,032 |
| receiver link open (+ fused FSP open) | 456 | 83,060 | rest of the receive batch queued 67,485; worker busy 14,869 |

So about 99% of the jobs went to workers although the dispatcher had time to
spare. The sender woke a sleeping worker 35.6k times and parked 41k times, and
worker batches averaged 4 jobs. The bead's suspects were right for the
receiver (the rest of a receive batch) and partly for the sender (queued
completions), but the largest single reason on the sender was the native
mailbox: the next datagram had usually arrived by the time the dispatcher woke
for the previous one. In
`perf record` 48% of the sender's samples were in `try_to_wake_up`; under HVF
a wake of another vCPU is an exit to the host. The dispatcher waking a worker
took 12%, worker completions waking the dispatcher 14%, libgo's netpoller
waking a scheduler thread for the native reader 9%, the dispatcher's timed
select waking an idle scheduler thread (`sleepers_add`) 6.4% plus that
thread's condvar exit. The loopback send waking the receiver took 5.6%, which
is inherent (Rust pays it too).

The counter build took the inline threshold from an environment knob
(diagnostic only): a job ran inline while at most K events were queued for the
dispatcher. c-c, 6 s at 100 Mbit/s, CPU us per delivered datagram, and
unpaced c-c Mbit/s:

| K | 0 (current) | 2 | 4 | 8 | 16 |
|---|---:|---:|---:|---:|---:|
| sender | 97.0, 95.4 | 74.2, 78.2 | 67.2, 69.1 | 71.8, 68.5 | 67.2, 67.4 |
| receiver | 68.0, 67.5 | 61.8, 60.6 | 59.4, 59.2 | 61.0, 57.3 | 58.7, 59.8 |
| unpaced c-c | 990.6, 851.7 | | 970.2, 947.7 | | 920.0, 999.6 |

At K=4, 97% of the sender's seals ran inline and worker wakes fell from
26,800 to 402 per 62.8k datagrams; at K=16 all ran inline. Under saturation
even K=16 inlined only 3% of the jobs, because the queues sit at 32 and more.

### Changes

- **fipsd** (`node/context.c`). `fipsd_node_dispatch_backlog` sums the
  queued completions, transport events, peer commands and command mailbox,
  and reports a backlog only above `FIPSD_NODE_INLINE_BACKLOG` (8). Up to
  that, a job runs inline as before, delaying those few events by one AEAD
  instead of costing two thread wakes. Unchanged: shard order (inline only
  while the shard's worker has nothing in flight, so a shard's first handoff
  keeps the rest of its burst there), the results-slot reservation, the send
  gate, and replay marks and counters on the dispatcher. `FipsdDataplaneTest`
  (`inline_backlog`) pins the rule and fails with the old one.
- **libgo** (`fiber/sched/sched.c`). `sleepers_add` signalled `S.cv` on every
  timed park (nostrc-e08s7), so each dispatcher park woke an idle scheduler
  thread that found nothing and slept again. Idle workers now record, under
  `S.mu`, when they will wake by themselves. The signal is skipped only when
  every idle worker is already due by the new deadline on a monotonic
  condvar: each then re-derives its wait from the heap, which holds the new
  sleeper, as the signalled worker would. A worker waiting untimed (the e08s7
  case) or past the deadline still gets the same single signal. macOS has no
  monotonic condvar (REALTIME fallback, which can oversleep after a clock
  step), so it always signals and is unchanged (nostrc-u19cz.21).

Not needed after this: completing each job right after its own send,
signalling once per submitted run and spinning workers all make handoffs
cheaper, and at these rates there are hardly any left; the dispatcher already
sends the frames it seals inline itself, so a low-rate send rule adds nothing.

### Results (Linux VM)

100 Mbit/s, 4 s windows, CPU us per delivered datagram, attributing the two
steps: three rounds of current, fipsd step, both steps. The last column ran
the first form of the libgo change ("any idle worker due by then"), which
behaves identically with the harness's two scheduler threads. `*` lost
12-15%, mostly local refusals at the sender's native API (see loss below).

| | Current | fipsd step | fipsd + libgo |
|---|---:|---:|---:|
| c-c sender | 96.5, 97.7, 95.8 | 70.5, 84.8\*, 66.7 | 60.0, 60.5, 63.4 |
| c-c receiver | 67.0, 68.4, 67.4 | 58.6, 58.0\*, 59.8 | 52.8, 51.6, 52.6 |
| C sender to Rust | 98.7, 99.8, 97.5 | 85.8\*, 68.7, 69.1 | 66.1, 68.8, 83.1\* |
| Rust sender (Rust-Rust) | 55.5, 62.2, 55.4 | 55.2, 57.8, 54.2 | 55.0, 59.3, 58.5 |

An earlier session with the pre-pass baseline, three rounds of current,
fipsd step, `4ff8ca4f2`: c-c sender 77.6, 96.2, 96.3 / 67.9, 67.7, 67.4 /
75.7, 86.4, 85.2; receiver 53.3, 66.5, 67.5 / 58.1, 56.9, 59.3 / 61.5, 72.2,
70.3; C sender to Rust 98.0, 98.4, 96.8 / 69.7, 69.0, 68.9 / 96.6, 86.9,
87.1.

Final code, 8 s windows, five rounds alternating with the current tip:

| | Current `9e78793e9` | u19cz.14 |
|---|---:|---:|
| c-c sender | 96.6, 97.8, 94.6, 96.3, 96.5 | 60.0, 63.4, 60.7, 58.9, 59.5 |
| c-c receiver | 67.3, 66.4, 66.1, 67.5, 67.4 | 51.7, 52.4, 52.7, 55.8, 52.6 |
| C sender to Rust | 98.9, 97.7, 100.7, 97.8, 98.6 | 64.5, 65.6, 65.1, 60.8, 64.9 |
| Rust receiver behind it | 52.3, 50.9, 54.2, 49.2, 50.9 | 65.8, 66.4, 66.3, 64.8, 64.3 |
| c-c loss % | 0, 3.09, 0.20, 0.21, 0 | 0, 0, 0, 3.45, 0 |
| C-to-Rust loss % | 0.64, 0.35, 4.82, 0.01, 0.52 | 0.45, 0.64, 0.06, 0, 2.96 |
| c-c loaded one-way p50 / p99 us | 1348/2182, 1178/5002, 1339/4082, 1238/2169, 1236/2279 | 314/874, 409/1215, 356/1057, 305/3193, 335/647 |
| C-to-Rust loaded one-way p50 us | 1187, 1128, 1068, 997, 1185 | 495, 487, 535, 412, 477 |
| c-c serial one-way p50 us (pre-load) | 294, 265, 561, 291, 280 | 227, 308, 271, 250, 256 |

Loss. Both builds show occasional runs losing 3-5% (12-15% in 4 s windows),
almost all of it local refusals: the native reader fell more than a client
socket buffer (about 16 ms at this rate) behind, i.e. a host-level stall. Over
the 4 s sessions such runs were 0 of 12 (current), 3 of 18 (this branch's two
steps) and 1 of 6 (`4ff8ca4f2`); over the 8 s session above, 2 of 10 for each
build. That is not a distinguishable difference.

The Rust receiver costs more behind the new C sender (50 -> 65 us): the
current tip's worker sent about 4 frames per `sendmmsg` (GSO), so the Rust
receiver woke once per burst, while an inline send at this rate is one
datagram per call. The pair as a whole is still cheaper (C sender plus Rust
receiver about 150 -> 130 us per datagram). Batching inline sends per
dispatcher burst is a follow-up (nostrc-u19cz.20).

Wakes and system calls per datagram, one session, 100 Mbit/s c-c (the Rust
row is its Rust-to-Rust pair in the same session):

| | CPU us | Context switches | futex | All syscalls | Wakeups (`sched_waking`) | Migrations |
|---|---:|---:|---:|---:|---:|---:|
| `4ff8ca4f2` sender / receiver | 87.5 / 72.8 | 2.09 / 1.63 | 4.60 / 3.70 | 10.05 / 8.79 | 1.97 / 1.62 | .001 / .002 |
| current sender / receiver | 100.0 / 70.1 | 1.89 / 1.23 | 4.38 / 2.86 | 6.55 / 4.70 | 1.82 / 1.22 | .112 / .110 |
| u19cz.14 sender / receiver | 62.5 / 54.1 | 1.57 / 1.30 | 2.73 / 2.26 | 7.22 / 5.36 | 1.49 / 1.30 | .004 / .003 |
| Rust sender / receiver | 56.3 / 71.7 | 1.45 / 2.04 | 1.35 / 1.16 | 10.28 / 7.72 | 1.29 / 1.66 | .000 / .001 |

The current tip makes only about 20% more wakes per datagram than this
branch. What it pays for is where they land: worker threads woken on idle
vCPUs (IPIs, one thread migration per nine datagrams). The two crypto threads
are now idle at this rate.

Unpaced (`--seconds 5`), delivered Mbit/s, receiver overload as in u19cz.4:

| | Current | u19cz.14 |
|---|---:|---:|
| c-c (seven rounds) | 1041.7, 1090.6, 971.8, 770.1, 1068.9, 1014.8, 1001.8 | 863.8, 948.4, 1039.6, 1059.7, 1178.0, 1212.7, 1096.7 |
| C to Rust | 236.4, 197.9, 188.9 | 177.4, 169.7, 156.9 |
| Rust to Rust (unchanged) | 81.2, 88.0, 84.6 | 88.8, 79.0, 82.3 |

C to Rust delivers less because the C sender offers more (676-695k datagrams
in 5 s against 533-597k) and the overloaded Rust receiver sheds more of it
(594-603k `native.drop_flow_queue_full` against 435-474k), as the u19cz.4
section describes.

Sustainable (<1% loss) Mbit/s, three searches each, `--paced-seconds 3`:

| | Current | u19cz.14 |
|---|---:|---:|
| c-c | 450.0, 481.2, 587.5 | 518.7, 387.5, 518.7 |
| C to Rust | 162.5, 125.0, 150.0 | 156.2, 200.0, 221.9 |

The 387.5 search failed at 400 Mbit/s on one stall (8.0k local refusals in
3 s). These are host noise, not a gain or a regression.

What is left to Rust (sender 59-63 against 55-57): in `perf record` of the
final sender, 23% of the samples are libgo's poller thread waking the
scheduler thread that runs the native reader. Each readiness costs two thread
wakes (kernel to poller, poller to worker; tokio's worker polls epoll
itself), and a targeted push broadcasts `S.cv` to every idle scheduler thread
(nostrc-y1y8n), which by default is one per CPU. Both are libgo design
changes, tracked as nostrc-u19cz.18 and nostrc-u19cz.19. The loopback send
waking the receiver (14%) is inherent.

### macOS

Release builds of `9e78793e9` and this branch, alternating, the second
session in reverse order. macOS keeps per-datagram sends and receives, and its
libgo condvar always signals, so only the fipsd step applies.

| macOS | Current | u19cz.14 |
|---|---:|---:|
| 100 Mbit/s CPU us/datagram, c-c sender | 32.6, 30.9, 31.0, 32.2, 28.1, 33.7 | 28.6, 36.2, 34.1, 34.1, 32.6, 31.0 |
| c-c receiver | 32.6, 31.3, 31.7, 31.0, 27.1, 33.2 | 28.3, 37.9, 34.8, 35.0, 32.4, 31.4 |
| C sender to Rust | 33.8, 36.3, 30.7, 40.1, 31.7, 31.9 | 31.2, 40.6, 40.6, 30.7, 31.5, 32.0 |
| Rust sender, same invocations | 31.0, 20.4, 20.7, 18.2, 20.6, 20.4 | 19.7, 25.5, 30.3, 22.6, 20.9, 27.6 |
| unpaced c-c Mbit/s | 1419.2, 1426.6, 1383.7 | 1418.6, 1331.8, 1463.9 |
| unpaced C to Rust Mbit/s | 765.4, 1069.2, 1152.7 | 1044.9, 841.6, 1052.0 |
| c-c serial one-way p50 fwd/rev us | 65/63, 64/63, 57/54 | 65/62, 56/55, 58/58 |

The means differ by less than the unchanged Rust pair moved (the branch's
slots happened to be the busier ones), so neither a regression nor a gain is
claimed on macOS.

Verification, Debug `-DGO_WARNINGS_AS_ERRORS=ON`, `ctest -R '^Fips' -E
'Soak|Perf'` with both Rust interop binaries set (the unrelated `nips/nip57`
target, missing `bolt11/bolt11.h`, does not build in either tree):

- macOS, Apple clang ASan+UBSan: 222/222 (8 capability skips). `^Go` 46/46,
  and the 15 multi-worker, timeout, idle and lifecycle libgo tests 10/10
  each.
- macOS TSan (vendored nsync, `-E '^FipsCore'`): 183/183 (8 skips), `^Go`
  46/46, no ThreadSanitizer report. `--repeat until-fail:5` over workers,
  dataplane, node/UDP v4/v6/native two-node, native burst, restart replay,
  the five multi-node topologies and both TCP rekey-saturated tests, `-j4`:
  every test 5/5 except one deadline miss of `FipsdTcpRekeySaturatedTest`
  beside the multi-node tests (nostrc-4im43), which then passed 5/5 alone
  (11-13 s of its deadline).
- Linux VM, clang 18 ASan+UBSan: 224/224 (11 capability skips). `^Go` 46/46,
  and the same 15 libgo tests 10/10 each; the sleeper skip is active only on
  Linux.
- Linux VM, clang 18 TSan (vendored nsync), libgo: `^Go` 46/46 and the 15
  tests 5/5 each, no report.
- `FipsdDataplaneTest` (`inline_backlog`) aborts with the threshold set back
  to 0. The libgo change was reviewed for lost wakes (a covering worker taken
  by a runnable, arbitrary signal recipients, the heap-read window, worker
  exit, the REALTIME fallback); its first form ("any idle worker due") was
  narrowed to "every idle worker due" and monotonic-only as a result.
- Static pre-push checks (whitespace, unsequenced arguments, errno after
  park, GDK frame, CI packages, Groundhog privacy) pass.
- Skips: privileged TUN/IPv6/Ethernet/gateway smoke, BlueZ, Tor, the control
  and fipsctl interop, and the kqueue-only native probe on Linux.

Deferred, with beads: libgo's poller hop, two thread wakes per readiness
(nostrc-u19cz.18); a targeted push waking every idle scheduler thread
(nostrc-u19cz.19); batching inline sends per dispatcher burst
(nostrc-u19cz.20); the Darwin condvar keeping the per-park signal
(nostrc-u19cz.21).

## UDP syscall batching on Linux: nostrc-u19cz.4

Linux only: macOS and Windows keep the per-datagram paths, and the C client,
harness and Rust daemon are unchanged. Measured **2026-11-19/20** on the
shared Linux VM below, base `ff3fe88e1` (perf/fips-deep-opt) and this branch
built side by side and run **back to back, alternating**, every run reported.
The VM runs on the same shared M4 Max host as the other agents' VMs and builds,
and its numbers are dominated by that: see "Noise" below before reading any
rate.

- Ubuntu 24.04 aarch64 under QEMU/HVF, 10 vCPU, ~3.9 GB RAM, kernel
  7.0.0-42-generic; `net.core.{r,w}mem_max` 4 MiB, so the 2 MiB UDP buffers are
  granted. Release `-O3 -DNDEBUG` (cc = gcc 13.3), `FIPSD_WORKERS=2` (harness).
- `perf_loopback.py --runs 2 --latency-samples 300`, with `--seconds 5`
  (unpaced), `--paced-rate 100 --paced-seconds 4` (CPU per delivered datagram)
  and `--paced --paced-seconds 3` (sustainable, <1% loss), each mode as four
  alternating invocations (base, new, base, new): 4 rows per pairing and build.
- Syscalls: `perf stat -e raw_syscalls:sys_enter,syscalls:sys_enter_<name>`
  attached to each C daemon of the c-c pairing for 5 s inside a long traffic
  window (16 s at 100 Mbit/s; 14 s unpaced); CPU profiles with `perf record -g`.

### Design

**libgo.** `gof_recvmmsg`/`gof_sendmmsg` (Linux only, appended to
`fiber/io/io.c` beside `gof_recvmsg`): `recvmmsg(2)`/`sendmmsg(2)` with
`gof_recvmsg`'s readiness contract per call. The fiber parks only while the
syscall would move no message; a short count is not an error; a zero timeout
never parks, so a crypto worker thread may call `gof_sendmmsg` as it calls
`gof_sendmsg`.

**Receive** (`transport/udp/connected.c`). The receive fiber takes up to 32
datagrams per `recvmmsg`, each with its own source address and ancillary data
(pktinfo destination, TOS/TCLASS ECN, `SO_RXQ_OVFL`), each through the same
decode, classifier, framing and route-hint step as before. Each slot receives
straight into a `fipsd_transport_event` the node will own and free, so a frame
reaches the dispatcher without `fipsd_transport_emit`'s per-packet
`calloc` + full-frame copy; the slot gets a fresh event (one `malloc`, no
zeroing). A datagram longer than the slot (the configured MTU) spills into a
per-slot tail that only such datagrams touch, and is copied into an event of
its size: every datagram the old 64 KiB buffer took is still taken. The
cumulative kernel-drop counter is recorded once per batch (as upstream), and a
burst from one source updates its route hint without the 64-entry scan.
`fipsd_transport_emit_event` tries a plain channel send before building the
cancellation select (all emitters).

Not done: one channel hop per batch, and an event pool returned by the
dispatcher. Both need the dispatcher loop (`node/context.c`), which frees each
event itself and was being changed concurrently (nostrc-u19cz.12).

**Send** (`transport/udp/udp.c`, `node/worker_set.c`,
`node/dataplane/encrypted.c`). A crypto worker drains up to 32 jobs. For
consecutive jobs sharing an `after_batch` step it runs their AEADs, then the
step once over them, then queues their completions in order. The data-lane
link send is that step on a transport with the new optional `send_batch` op
(Linux UDP); other transports keep the per-frame `after` send. Per gate run
(consecutive frames of one peer slot and transport) the worker takes the gate
lock once and decides each frame in queue order exactly as a single send
would: sent only at the generation captured at submit with nothing `held`, a
held frame holding every later one. The frames it may send leave in one
`send_batch`, in that order, before the lock is released, so a bump still
waits for an in-progress send, and bounced frames still follow in completion
order. Each frame keeps its own result (`send_rc`) and all accounting stays on
the completion.

UDP `send_batch` groups consecutive frames to one destination (one socket
lease, one resolution) and issues one `sendmmsg` per group. A run of
equal-size datagrams, of which only the last may be shorter, rides one
message with a `UDP_SEGMENT` control message (GSO, at most 64 segments and
65000 bytes); listener sends carry the pktinfo source beside it.
Partial results: `sendmmsg` returning k < n resubmits from message k, so its
error is learned; a failed plain message fails only its frame and the rest
follow in order. A failed GSO message is resent, with the rest of the run, as
plain messages, so its frames meet what single sends would (backpressure takes
what fits; a pending socket error fails one send). If the kernel refused GSO
itself (EIO: no checksum offload on the route; EINVAL, EOPNOTSUPP,
ENOPROTOOPT, EMSGSIZE), that socket stops using GSO, with one warning. GSO is
probed per socket at open with `getsockopt(UDP_SEGMENT)`, as quinn-udp does,
because a kernel without it would ignore the control message and send one
oversize datagram. `FIPSD_UDP_GSO=0` turns it off (diagnostics). Backpressure
(EAGAIN) is still a send failure, never a wait, as with `sendmsg`.

### Syscalls per datagram

c-c, 5 s windows, every syscall of each daemon (including the native API and
scheduler):

| Paced 100 Mbit/s (~52k datagrams) | Base sender | New sender | Base receiver | New receiver |
|---|---:|---:|---:|---:|
| all syscalls | 411.5k | 385.9k | 334.1k | 286.8k |
| UDP send (`sendmsg` / `sendmmsg`) | 52.5k | 11.7k | — | — |
| UDP receive (`recvmsg` / `recvmmsg`) | — | — | 60.1k | 17.4k |
| `fcntl` (libgo `set_nonblock`, nostrc-bmsti) | 104.9k | 63.9k | 99.8k | 59.4k |
| `futex` | 172.2k | 221.2k | 111.3k | 141.8k |

| Unpaced (~95k datagrams/s sent) | Base sender | New sender | Base receiver | New receiver |
|---|---:|---:|---:|---:|
| all syscalls | 2.07M | 1.01M | 1.38M | 0.33M |
| UDP send / receive calls | 474.5k `sendmsg` | 25.8k `sendmmsg` | 484.6k `recvmsg` | 21.1k `recvmmsg` |
| `fcntl` | 949.1k | 435.3k | 611.7k | 111.8k |
| `futex` | 155.7k | 120.9k | 132.1k | 88.2k |

At 100 Mbit/s a batch averages ~4.5 frames on the send side and ~3 datagrams
per receive call (including the receive that finds the socket empty and
parks). Unpaced: ~18 datagrams per `sendmmsg` and ~23 per `recvmmsg`.
`futex` rises at the paced rate. The likely cause is the u19cz.3 inline rule,
but it is not instrumented: a receive batch leaves events queued, and a worker
holds a run's completions until its batched send, so fewer jobs run inline on
the dispatcher (measured and fixed by nostrc-u19cz.14, above).

### Results

Paced 100 Mbit/s, CPU microseconds per delivered datagram (traffic window).
`*` marks runs in which the pair was already slow before load: serial one-way
p50 of 0.58-1.9 ms against ~0.3 ms otherwise, with low CPU and 28-69% loss.
Both builds show such runs (nostrc-u19cz.13).

| | Base `ff3fe88e1` | u19cz.4 |
|---|---:|---:|
| c-c sender | 102.1, 102.7, 156.4*, 100.4 | 102.8, 168.0*, 96.2, 92.2* |
| c-c receiver | 71.3, 70.6, 107.2*, 70.9 | 69.8, 116.2*, 67.4, 64.8* |
| C sender to Rust | 103.3, 112.0, 102.5, 103.6 | 100.8, 97.6, 98.8, 85.4* |
| c-c loss % | 1.0, 2.8, 59.5*, 0.1 | 4.4, 69.4*, 3.4, 28.1* |

Means without the marked runs: c-c sender 101.7 -> 99.5 us, receiver
70.9 -> 68.6 us, C sender to Rust 105.4 -> 99.1 us. That is 2-6% lower, within
this VM's noise. In the same probes the Rust
daemon costs 53-58 us (sender) and 63-73 us (receiver). The profile explains
why the effect is small. On this VM, thread wake-ups (`futex` ->
`try_to_wake_up`, an IPI under HVF) take ~30-35% of every daemon's samples.
The UDP send path itself was 14.4% of the sender's samples before and 10.7%
after (`udp_sendmsg` and loopback xmit, which on loopback includes the
receiver's softirq and its wake), and the receive fiber 5.8% -> 4.9% of the
receiver's (`fipsd_transport_emit` 1.3% -> `_emit_event` 0.3%).

Unpaced delivered Mbit/s. Every row is receiver overload: C pairs lose
65-73% (base) and 72-81% (new, whose sender offers more), Rust to Rust 97%.

| | Base | u19cz.4 |
|---|---:|---:|
| c-c | 232.0, 248.6, 270.3, 284.5 | 207.3, 282.0, 274.6, 283.1 |
| C to Rust | 254.1, 258.5, 185.1, 337.0 | 145.4, 214.5, 196.4, 257.0 |
| Rust to Rust (unchanged) | 71.3, 77.0, 73.4, 76.8 | 81.0, 76.0, 83.9, 80.6 |

Unpaced rows measure how a receiver sheds an overload, not capacity. The
faster C sender (more sent: 1.55M vs 1.34M in one 14 s C-to-Rust window) lets
the Rust receiver deliver less. On the C receiver, every drop in both builds is
native delivery (`native.drop_flow_queue_full`). There are no kernel receive
drops and no UDP send errors (`show_metrics`/`show_transports` after fixed
100/200 Mbit/s probes, 2 each). Batched receive did not raise those drops:
base 2794, 3628 (100) and 17455, 36706 (200), new 521, 6100 and 13771, 22888.

Sustainable (<1% loss) payload Mbit/s, 4 runs each. Noise again: the unchanged
Rust-to-Rust pair ranged 36-200 Mbit/s.

| | Base | u19cz.4 |
|---|---:|---:|
| c-c | 107.8, 47.7, 112.5, 60.2 | 93.7, 100.0, 46.9, 51.6 |
| C to Rust | 49.2, 81.2, 85.9, 35.2 | 63.3, 106.2, 87.5, 74.2 |
| Rust to Rust (unchanged) | 75.8, 35.9, 151.6, 95.3 | 47.7, 103.1, 125.0, 200.0 |

**Noise.** At a fixed 100 Mbit/s the VM's loaded one-way p50 is 1.0-1.4 ms.
Failures in the sustainable search look like sporadic stalls rather than a
rate limit: one base C-to-Rust search lost 5.6% at 50 Mbit/s, then passed every
probe up to 49.2. These rates are recorded, not claimed as a gain or a
regression; the syscall counts and the profile shares are the reliable
evidence.

Verification: Debug `-DGO_WARNINGS_AS_ERRORS=ON`, `ctest -R
'^Fips|^GoFiberIoDatagram' -E 'Soak|Perf'`, with both Rust interop binaries
set.

- Linux VM, clang 18 ASan+UBSan: 226 tests, 0 failures, 11 capability skips.
  The Linux-only cases ran: one-call `recvmmsg` burst, batched sends over
  connected and listener sockets with GSO probed, and the libgo mmsg tests.
- Linux VM, clang 18 TSan (vendored nsync, `-E '^FipsCore'`): 187 tests,
  2 failures, both pre-existing. `FipsdNodeMultiRingTest` and
  `FipsdNodeMultiFullMeshTest` hit a report on OpenSSL's per-thread
  `EVP_CIPHER_CTX` between two in-process nodes' dispatcher fibers; the
  unmodified base reproduced it 5/5 (nostrc-u19cz.16). Nothing else was
  reported.
- Linux VM, gcc 13.3 Release `-O3 -Werror`: everything builds except
  `libfips-core/tests/test_aead_residue.c`, pre-existing (nostrc-u19cz.17).
  226 tests, 0 failures, 2 not built, 11 skips.
- macOS, Apple clang ASan+UBSan: 224 tests, 0 failures, 8 skips. TSan
  (vendored nsync, `-E '^FipsCore'`): 185 tests, 0 failures, 8 skips, no
  reports. `FipsdDataplaneTest` repeated 30/30 under ASan.
- Stage 1 alone (`780ef2975`) builds with gcc `-Werror` on Linux, and its
  UDP, dataplane, libgo and UDP two-node tests pass.
- Skips are capability skips: privileged TUN/IPv6/Ethernet/gateway smoke,
  BlueZ, Tor, the control and fipsctl interop (upstream checkout and cargo),
  and the kqueue-only native probe on Linux.

Stage 3 (outbound fusion, nostrc-ov6k4) was not attempted. On the VM the
sender is bound by its dispatcher thread and by thread wakes, not by the link
worker; on macOS the link worker is still the single-flow bottleneck (u19cz.3)
and keeps per-datagram sends, so fusion would cost throughput there.

macOS (M4 Max, shared host, load average ~20) keeps every per-datagram path:
the worker's per-frame send (no `send_batch` op), `recvmsg` into one buffer
and the copying emit. Only the emit's channel fast path and the receive
route-hint cache are shared with Linux. Release builds of base and final code,
alternating as on the VM:

| macOS | Base `ff3fe88e1` | u19cz.4 |
|---|---:|---:|
| 100 Mbit/s CPU us/datagram, c-c sender | 49.9, 53.6, 55.1, 51.9 | 45.4, 51.1, 49.2, 43.5 |
| 100 Mbit/s CPU us/datagram, c-c receiver | 43.1, 48.8, 48.3, 47.5 | 44.7, 43.9, 44.6, 42.3 |
| 100 Mbit/s CPU us/datagram, C sender to Rust | 53.8, 63.7, 50.9, 59.2 | 45.0, 54.0, 60.9, 37.4 |
| unpaced c-c Mbit/s | 776.3, 354.7, 530.7, 647.8 | 559.9, 609.7, 415.1, 593.0 |
| unpaced Rust to Rust (unchanged) | 438.2, 386.4, 544.9, 241.9 | 681.0, 655.0, 72.5, 319.7 |

No regression is visible beyond the host noise shown by the unchanged pair.

Deferred, with beads:

- One dispatcher channel hop per receive batch, and a dispatcher-returned
  event pool (nostrc-u19cz.12).
- Intermittent slow C-C pair on the VM, in both builds (nostrc-u19cz.13).
- Fewer inline crypto runs at moderate rates with batched I/O, measured as
  the `futex` rise (nostrc-u19cz.14, done above).
- Darwin `recvmsg_x`/`sendmsg_x` batching, as upstream does on macOS
  (nostrc-u19cz.15).
- Pre-existing gate failures found here: Linux TSan on the OpenSSL
  per-thread context (nostrc-u19cz.16), and gcc `-Werror` in
  `test_aead_residue.c` (nostrc-u19cz.17).
- Outbound fusion stays deferred (nostrc-ov6k4).

## Dataplane threads: nostrc-u19cz.3

Measured **2026-11-19** on the M4 Max below (Release `-O3 -g`, local macOS, not
hosted/Linux qualification). The machine was shared with other benchmarking
agents and two VMs (load average 10-17), so absolute numbers are noisy: the
baseline (`5b3c987c8`) and each stage were measured **back to back in one
session**, every run is reported (no best-of selection), and run 1's c-c pair
carried a 4 s `sample` of both daemons, which depresses its numbers.
Saturation is `perf_loopback.py --runs 2 --seconds 5 --latency-samples 300`.
CPU per delivered datagram comes from the paced harness of
`perf/u19cz-1-paced-harness` (`4ff8ca4f2`, its CPU-window client built from
that commit) at a fixed 100 Mbit/s: `--paced --paced-seconds 5
--paced-max-mbps 100`, about 10.4k datagrams/s.

### Result: interleaved baseline vs stage 3

Final round: baseline and stage 3 alternated, no `sample`. One saturated
`--runs 1` run and one 100 Mbit/s probe per binary per round, two rounds.

| | Baseline `5b3c987c8` | Stage 3 `ea2031f60` |
|---|---:|---:|
| c-c Mbit/s (loss %) | 721.6 (0.1), 708.2 (0.1) | 891.5 (25.5), 725.7 (39.1) |
| c-rust Mbit/s (loss %) | 717.9 (0.8), 726.9 (0.6) | 1000.4 (18.5), 1217.6 (3.7) |
| c-c one-way p50 fwd/rev us | 98/103, 91/93 | 72/72, 80/81 |
| c-rust one-way p50 fwd/rev us | 64/66, 92/92 | 55/61, 61/64 |
| daemon CPU % one core, c-c, tx / rx | 102.6/111.4, 101.4/110.4 | 251.0/180.0, 243.7/172.3 |
| CPU us/datagram at 100 Mbit/s, c-c tx / rx | 48.7/48.5, 53.6/53.4 | 33.2/35.3, 36.5/38.6 |
| CPU us/datagram at 100 Mbit/s, c-rust C sender | 55.2, 53.4 | 37.8, 38.6 |
| Rust in the same probes (sender / receiver) | 29.1/44.9, 23.8/39.8 | 21.3/34.4, 26.2/43.1 |

Trade-off at moderate paced rates. Direct c-c probes, 3 s, two each,
interleaved. Losses out of 93750 / 156250 / 218750 datagrams:

| Offered | Baseline | Stage 3 |
|---|---:|---:|
| 300 Mbit/s | 9, 1865 | 62, 181 |
| 500 Mbit/s | 8, 332 | 783, 1706 |
| 700 Mbit/s | 9340, 3623 | 5577, 1674 |

Every stage 3 loss is `native.drop_flow_queue_full` at the receiver. The
faster dispatcher delivers a batch faster than the flow feeder drains a
per-flow queue of `node.native_api.pending_per_flow` (default 16). The
baseline lost mostly local send refusals at the sender instead. Native
delivery is outside this change and is tracked as nostrc-rpqvh (fixed below,
"Native delivery"). Unpaced C-to-C overload loss is the same mechanism.

Idle cost is unchanged. Two linked idle daemons (`top`, 5 s intervals) saw
+239 to +298 context switches per interval for stage 1 against +213 to +383 for
the baseline, at 0.0-0.3% CPU. The two crypto threads block on their
condvars.

Verification, per stage, Debug `-DGO_WARNINGS_AS_ERRORS=ON`, with
`FIPSD_UDP_INTEROP_RUST_BINARY` set, and `FIPSD_TCP_INTEROP_RUST_BINARY` from
stage 2 on. Stage 1 ran the three TCP interop tests separately. ASan: 3/3.
TSan: the C-to-Rust rekey-saturated scenario missed once while running beside
the other two, then passed 6/6 alone, alternating with the baseline, with
identical K flips (C 34.1-34.4 s, Rust 35.9-37.0 s).

- ASan+UBSan `ctest -R '^Fips' -E 'Soak|Perf'`: 221/221.
- TSan, same selection: 221/221, no ThreadSanitizer report.
- TSan `--repeat until-fail:5` over two-node (node, UDP v4/v6, native), ring,
  presence/rebind, restart-replay, TCP rekey saturated, native burst, workers
  and dataplane: stages 2 and 3 55/55. Stage 1 54/55: the miss was
  `FipsdTcpRekeySaturatedTest`'s deadline, a pre-existing tail (nostrc-4im43).
  The unmodified baseline TSan binary also took 21.6 and 36.2 s of a 37 s
  deadline. Later runs did not separate the builds: across inline on/off and
  sharding on/off variants, slow runs were 0/10 vs 2/10, 2/10 vs 0/10, 1/15 vs
  2/15.
- Capability skips: privileged TUN/Ethernet/IPv6 smoke, Tor, and the control
  and fipsctl interop tests, which need an upstream control checkout and cargo.

Deferred, with beads:

- Outbound fusion (nostrc-ov6k4).
- Windows worker-side send (nostrc-akn6b).
- libfips-core's 58 KB output clear per executed step (nostrc-02p6s).
- Per-wake timer scans (nostrc-w3tfz).
- libgo's per-send `fcntl` (nostrc-bmsti).

### Stage 1: crypto on OS threads

Before, the "crypto workers" were scheduler fibers. libgo wakes a fiber on its
last worker and steals only from deep queues, so in each daemon one OS thread
did all packet work (sender `sample`: 2104 of 2136 samples active on one
thread, the second scheduler worker idle).

- `worker_set.c` runs each worker on its own pthread (winpthreads on Windows,
  as libgo's blocking executor). Each has a bounded FIFO ring under a
  mutex/condvar. A producer signals only when it makes a waiting worker's empty
  queue non-empty (upstream `MacWorkerSender`), and an idle worker blocks: no
  timer, no spin. Completions use the existing bounded results channel. A
  blocking `go_channel_send` from a plain thread waits on OS primitives and
  wakes the dispatcher's select through `gof_hook_make_runnable`, as the
  blocking executor does. Workers drain batches of 32; the dispatcher keeps its
  64-event batch.
- Jobs carry a shard. A peer's link jobs stay on one worker and its session
  jobs on the next, so a flow completes in order and a session datagram's two
  AEADs (or an arrival's two opens) pipeline over two workers.
- A handoff buys parallelism only while other input waits. When the results
  channel, transport events, peer inbox and command mailbox are all empty, and
  the shard's worker has nothing in flight, the dispatcher runs the job itself
  (since nostrc-u19cz.14: while at most 8 events are queued in them).
  Its completion still goes through the results channel, behind that worker's.
  The run first reserves a results slot that no accepted job can still claim,
  or the job goes to its worker instead (review fix), so an inline completion
  is never dropped and the dispatcher never waits on its own queue.
  Without this, CPU per datagram at 100 Mbit/s rose from ~56 to ~88 us: two
  thread wakes per job cost several times the AEAD.
- Wipes. `recycle` used to wipe all three 2 KB slabs after
  `fipsd_crypto_job_destroy` had already wiped them. It now wipes once, only
  the bytes a tenant can have written: the header in `ad`, the copied input, and
  in `output` the header plus at most input + tag. The dataplane also called
  `fips_rekey_outputs_clear` on already-empty output lists, which wipes all 48
  items (58 KB) whatever `count` says, and executed steps that produced no
  output. fipsd now skips both; outputs only append at `count`, and compaction
  wipes the tail it drops.

| Stage 1, back to back | Baseline `5b3c987c8` (two sessions) | Stage 1 |
|---|---:|---:|
| c-c Mbit/s, run 1 (sampled) / run 2 | 517.2 / 714.4; 474.7 / 707.1 | 323.3 / 877.0 |
| c-c loss %, run 1 / run 2 | 0.6 / 0.1; 1.1 / 0.1 | 46.4 / 15.1 |
| c-c one-way p50 fwd/rev us, run 2 | 98 / 99; 85 / 88 | 64 / 66 |
| c-rust Mbit/s, run 1 / run 2 | 660.0 / 709.9; 683.7 / 718.7 | 1011.5 / 1000.5 |
| c-rust loss %, run 1 / run 2 | 5.7 / 1.2; 2.0 / 0.5 | 1.8 / 4.4 |
| daemon CPU % one core, c-c run 2, tx / rx | 102.5 / 112.9; 102.8 / 116.8 | 167.6 / 183.4 |
| CPU us/datagram at 100 Mbit/s, c-c tx / rx | 56.6/55.2, 62.0/55.0, 56.5/55.7, 71.6/66.4, 56.6/56.2, 61.1/56.6 | 38.4/39.7, 39.0/40.3, 36.3/37.5 |
| CPU us/datagram at 100 Mbit/s, c-rust C sender | 55.3, 64.6, 62.8, 66.3, 52.8, 68.8 | 36.5, 36.5, 35.3 |

Rust at 100 Mbit/s in the same probes: 20.7-29.1 us (sender) and 34.6-56.0 us
(receiver) per datagram. Thread split (`sample`, c-c run 1):

- Baseline sender: one thread 2104/2136 active (`__sendmsg` 719, ChaCha20 403,
  `__bzero` 157, Poly1305 110, `memset` 109). Receiver: one thread 1697/1864
  active.
- Stage 1 sender: dispatcher thread 1567/1614 active, `fipsd-crypto-0` and
  `fipsd-crypto-1` 412 each. Receiver: dispatcher 916/1467, crypto 286 + 269.
- At 100 Mbit/s the Stage 1 workers are nearly idle (inline runs). The
  dispatcher threads hold `__sendmsg`, the per-iteration timer scans and clock
  reads.

The sender now offers more than one C receiver absorbs unpaced. c-c loss is
receiver overload: run 2 sent 538k and delivered 457k. Rust-rust drops 39-86%
the same way. It is not a send-side regression, and c-rust stays near 2-4%.
The sender's dispatcher thread is saturated with `__sendmsg`; stage 2 moves
that send.

### Stage 2: data-lane sends from the worker

The worker that sealed a data-lane link frame now sends it, as upstream's
encrypt workers do. This applies to transports whose new
`send_concurrent(to)` op allows it: UDP to a numeric endpoint, POSIX only.
On Windows the fiber socket layer uses overlapped I/O, and upstream spawns no
encrypt workers there. The UDP instance and DNS-cache nsync locks are
published to TSan (`udp/lock.h`), as libgo's `NLOCK` does. TCP, Tor, Nym,
Ethernet, BLE and the loopback test transport keep the completion-time send,
as do farewells (which outlive the table) and addressed, control-lane path
messages.

- Submit resolves the active pair, as the completion would, and checks that
  pair's MTU (the completion's own check). Counters stay reserved by the
  dispatcher, and all accounting stays on the completion: `frames_sent`, link
  statistics, MMP `record_sent`, the machine's `DataSent` and send-failure
  counts. It runs only for frames the worker reports sent.
- 9keoz.12 guard. Each peer slot has a gate (mutex + generation). The worker
  sends only while holding it at the generation captured at submit. The
  dispatcher bumps it, under the mutex, before a path switch closes the old
  pair, on rebind, when an epoch can leave the link's set
  (retired/reverted/restarted/teardown; every removal is a `Retired` output),
  when removal starts, and on slot release. A plain install does not bump: the
  old current epoch drains, still accepted, to the same destination. A bump
  waits for an in-progress send. After it, a frame comes back unsent and the
  completion resolves it as before: re-resolve, MTU, stale check, dispatcher
  send.
- Wire order (review fix). Without a barrier, a frame submitted after a bump
  could leave from the worker before the earlier frames the bump sent back to
  the dispatcher; across a rekey, a new-epoch frame would then overtake
  old-epoch data (nostrc-3vl7v). The gate also counts `held`: the slot's
  data-lane frames the dispatcher still has to send or drop. Frames left to the
  completion count from submit, bounced ones from their bounce, and recycle
  releases them after the completion. The worker sends only at `held == 0`;
  otherwise it bounces the frame and holds it too. A peer's jobs are FIFO on
  one worker, inline runs happen only while that worker is idle, and
  completions are handled in order, so the wire sees frames in submit order.
  `FipsdDataplaneTest` (`gate_wire_order`) bumps with frames queued on a
  blocked real worker and checks the order; it fails without the barrier.

| Stage 2, back to back | Baseline `5b3c987c8` | Stage 2 |
|---|---:|---:|
| c-c Mbit/s, run 1 (sampled) / run 2 | 509.1 / 709.8 | 271.9 / 613.4 |
| c-c loss %, run 1 / run 2 | 1.3 / 0.1 | 56.7 / 47.4 |
| c-c one-way p50 fwd/rev us, run 2 | 78 / 84 | 62 / 63 |
| c-rust Mbit/s, run 1 (sampled) / run 2 | 712.7 / 676.3 | 878.6 / 1118.9 |
| c-rust loss %, run 1 / run 2 | 1.1 / 2.0 | 20.8 / 6.1 |
| daemon CPU % one core, c-c run 2, tx / rx | 102.3 / 113.8 | 247.9 / 183.5 |
| CPU us/datagram at 100 Mbit/s, c-c tx / rx | 54.4/53.8, 70.1/66.2 | 39.2/40.8, 43.8/45.3 |
| CPU us/datagram at 100 Mbit/s, c-rust C sender | 66.5, 55.7 | 40.0, 39.6 |

Thread split (`sample`, c-c run 1):

- Sender: `fipsd-crypto-0`, the link worker, 1601/1722 active (`__sendmsg` 942,
  ChaCha20 362, Poly1305 94, `__fcntl` 53). Dispatcher 1356/1722 with no
  `__sendmsg`. `fipsd-crypto-1` (FSP seals) 517.
- Receiver: dispatcher 1141/1579, crypto 403 + 391.

The single-flow sender is now bound by its link worker. C-to-Rust delivers
1.1 Gbit/s unpaced; C-to-C collapses into receiver overload. The receiver drops
work it has already partly done: an arrival needs a second slab and job for its
FSP open, and delivery bursts overflow the native flow queue (stage 3).
Paced, the picture differs. A direct 400 Mbit/s c-c probe lost 111 of 125000
datagrams on stage 2 (109 `native.drop_flow_queue_full` at the receiver),
against 4128 on the baseline (3281 local send refusals, 756
`drop_flow_queue_full`). Single sustainable-rate searches (<1% loss) were
dominated by such edge losses and by host load: unchanged Rust-to-Rust ranged
197-687 Mbit/s between sessions. They are not reported as a stage metric.

### Stage 3: one job per arrival

Inbound fusion. The link open job's after-step (`fipsd_dp_worker_preopen`)
runs on the executing thread. For an established session frame addressed to
this node, it also opens that frame, using the session epochs of the packet's
own immutable, retained snapshot, tried in snapshot order as the open retry
chain does. The plaintext is left in the slab, and nothing is accepted there.

At completion the dispatcher runs the link step (FMP replay window), then
`fipsd_dp_preopened` from `hs_session_datagram_received`. That checks the
opening epoch is still live in the session machine (stale-epoch check), and
the authenticated-frame step marks the FSP replay window. A failed pre-open is
not a verdict. Its snapshot can predate a session epoch installed by an
earlier frame that was still completing, so the frame takes the ordinary open
with a fresh snapshot, the full retry chain and its accounting. One slab, one
job and one completion per arrival replace two.

Outbound fusion is deliberately not done. With per-link sharding, one job
would add the FSP seal (~500 samples in the stage 2 profile) to the link
worker, which is already the single-flow bottleneck (93% busy with
`__sendmsg` + FMP seal). That would cut single-flow throughput by about a
fifth to save dispatcher time, which is not the limit. At low load both seals
already run inline on the dispatcher. Upstream fuses outbound because its
rx_loop, not the worker, was its limit.

| Stage 3, back to back | Baseline `5b3c987c8` | Stage 3 |
|---|---:|---:|
| c-c Mbit/s, run 1 (sampled) / run 2 | 469.7 / 714.1 | 330.5 / 744.8 |
| c-c loss %, run 1 / run 2 | 1.0 / 0.2 | 48.3 / 31.4 |
| c-c one-way p50 fwd/rev us, run 2 | 88 / 89 | 82 / 81 |
| c-rust Mbit/s, run 1 (sampled) / run 2 | 690.5 / 721.8 | 1114.7 / 1052.4 |
| c-rust loss %, run 1 / run 2 | 2.0 / 0.5 | 5.0 / 8.8 |
| daemon CPU % one core, c-c run 2, tx / rx | 102.5 / 112.2 | 234.8 / 171.9 |
| CPU us/datagram at 100 Mbit/s, c-c tx / rx | 55.9/55.9, 56.6/55.5 | 36.3/38.4, 48.2/47.7 |
| CPU us/datagram at 100 Mbit/s, c-rust C sender | 55.4, 67.9 | 38.2, 36.0 |

Thread split (`sample`, c-c run 1):

- Sender: unchanged from stage 2. `fipsd-crypto-0` 1589/1690 active
  (`__sendmsg` 973), dispatcher 1302/1690, `fipsd-crypto-1` 505.
- Receiver: `fipsd-crypto-0` does both opens, 737/1730 active (ChaCha20 518,
  Poly1305 143). Dispatcher 1210/1730. The second worker serves other flows.

C-to-C overload loss falls from stage 2's 47% to 31% at more goodput. The
remaining loss is the receiver's dispatcher plus native delivery bursts
(`native.drop_flow_queue_full`).

## Native delivery: nostrc-rpqvh

Measured **2026-11-19** on the same M4 Max (Release `-O3 -g`), against the
unmodified base `810730485`, back to back and interleaved in one session,
every run reported. The host was shared with other agents' benchmarks and two
VMs (load average 12-40, mostly 15-25): even unchanged Rust-to-Rust lost
59-91% unpaced, and at 100 Mbit/s both builds' receivers, C and Rust, dropped
a few hundred datagrams to a descheduled client.

**Root cause.** In a busy daemon every fiber runs on one libgo worker: a
woken fiber runs on its waker's worker, so the dispatcher, the native receive
fiber and the flow's feeder share a thread (`sample` of the base receiver,
unpaced: that worker 1422/1431 samples busy, the second idle; dispatcher
~870, UDP receive ~395, feeder ~80, receive fiber ~75). The receive fiber
pushed a dispatcher batch (up to 64) into the flow's queue of
`pending_per_flow` = 16 without yielding, and the feeder could drain only that
queue's worth before the dispatcher's next batch: about 16 delivered per 64
decrypted. In that sampled unpaced run 601k of 1.15M datagrams were
`native.drop_flow_queue_full` at the receiver, after their AEAD work. CPU was
never the limit (feeder plus receive fiber ~11% of the thread); the queue's
per-round capacity was.

**Change** (`native/api.c`, design in `fipsd/README.md`, "Inbound delivery
keeps pace with the dispatcher"). The receive fiber delivers up to 64 notices
as a batch and writes a flow's datagrams straight onto the client's
descriptor while the feeder holds nothing (one `sendmsg` per batch per stream
flow, no copy, no fiber hop); only what the client cannot take yet is queued
for the feeder, in order, and the feeder writes up to 16 queued frames per
write, parking on writability without `gof_sendmsg`'s per-call `fcntl`. The
client's socket buffer (unchanged budget) now absorbs a burst, as upstream's
concurrently running writer lets it. Counters, outcomes, parking and drop
semantics are unchanged.

C-to-C at fixed rates, 4 s; loss, then the receiver's
`native.drop_flow_queue_full`, of 125000 / 208333 / 291666 datagrams. Four
runs each (two rounds in each of two passes); the rest of each loss is the
sender's local refusals, which this change does not touch:

| Offered | Base `810730485` | rpqvh |
|---|---:|---:|
| 100 Mbit/s (of 52083) | 764/669, 217/158, 225/225, 557/557 | 752/276, 647/113, 0/0, 0/0 |
| 300 Mbit/s | 652/626, 10534/8786, 81/81, 7094/4692 | 5792/1551, 1465/455, 749/203, 365/11 |
| 500 Mbit/s | 7265/5535, 5106/4862, 44238/33378, 46100/35912 | 2717/733, 7821/3690, 865/424, 2747/1287 |
| 700 Mbit/s | 55455/45599, 66852/48659, 44867/42313, 37737/33126 | 1735/610, 2786/1298, 23030/7302, 626/209 |

A first pair at 500 Mbit/s: base 21591/18096, rpqvh 46/0. A diagnostic build
that counted a full dispatcher-to-API sink separately (not committed) found
no sink overflow at 500 (2 queue drops), 700 or unpaced: every remaining
receiver drop is at the flow queue behind a client that fell behind. The
receiver is no longer saturated (`sample`, unpaced: busiest worker 824/1271
samples), and the perf client's per-datagram `poll` + two `recv` + payload
check is now the slower side; at moderate rates the residue is a client
descheduled for longer than ~64 KiB of socket plus the queue hold.

| | Base | rpqvh |
|---|---:|---:|
| c-c unpaced Mbit/s (loss %) | 545.5 (47.1), 599.5 (47.9), 303.8 (65.6) | 1023.5 (9.4), 992.2 (10.7), 1004.5 (6.5) |
| c-c unpaced receiver CPU us/delivered datagram | 21.6, 21.4, 33.7 | 12.1, 11.6, 12.4 |
| c-c unpaced daemon CPU % one core, tx / rx | 176/121, 194/132, 156/105 | 187/128, 177/118, 192/128 |
| c-c sustainable (<1% loss) Mbit/s | 293.7, 64.1 (load 40) | 750.0, 606.2 |
| c-c one-way p50 fwd/rev us, pre-load (unpaced runs) | 70/70, 63/66, 71/70 | 70/69, 85/85, 65/64 |
| c-c loaded one-way p50 us at 500 / 700 Mbit/s | 204, 181, 522, 415 / 377, 569, 538, 222 | 205, 181, 283, 459 / 313, 222, 615, 185 |
| c-c CPU us/datagram at 100 Mbit/s, rx (final code) | 39.6, 45.9, 41.5 | 47.6, 38.0, 43.5 |
| c-rust unpaced Mbit/s (loss %) | 330.1 (70.6), 604.5 (57.9), 281.0 (70.4) | 434.4 (65.2), 326.1 (69.9), 335.9 (64.5) |
| c-rust at 500 Mbit/s, loss / Rust receiver drops | 6979/6727, 6053/6051, 2956/2906, 2627/1483, 18658/17055, 58541/54195 | 22518/21795, 25433/24430, 2953/2677, 2039/2029, 1337/871, 101666/85973 |

C-to-Rust exercises only the unchanged C sender and the Rust receiver, whose
drops swung with host load in both builds. The first two 500 Mbit/s pairs ran
base first, each time into rising load, and looked worse for rpqvh; four
more pairs alternating the order (the last into a load spike that hit both)
did not repeat it. Its sustainable searches: base 96.9 and 11.5, rpqvh 181.2
and 106.2. No change is attributed there.
CPU per datagram at 100 Mbit/s before the last change (zero-filling a 2 KiB
iovec batch per write, now removed) was 43.1-48.4 base against 39.1-57.0
rpqvh over seven interleaved pairs; the final code's three pairs are in the
table, inside that noise.

Verification, Debug `-DGO_WARNINGS_AS_ERRORS=ON`, `FIPSD_UDP_INTEROP_RUST_BINARY`
and `FIPSD_TCP_INTEROP_RUST_BINARY` set (the unrelated `nips/nip57` target,
missing `bolt11/bolt11.h`, does not build in either tree; no FIPS target
depends on it):

- ASan+UBSan `ctest -R '^Fips' -E 'Soak|Perf'`: 222/222 (8 capability skips).
- TSan (nsync from source), `-E 'Soak|Perf|^FipsCore'`: 183/183 (8 skips), no
  ThreadSanitizer report; `ctest -R Native --repeat until-fail:5`: 45/45.
- `FipsdNativeApiTest` gains the direct-write stage (a stream flow behind a
  stalled 2 KiB client, `arrive` to the first refusal with an `inject`
  midway, all read whole and in order; it fails if the feeder ignores a
  partially written frame's offset). `test_perf_loopback.py`: 9/9.

Deferred, with beads:

- The C client's receive cost, now the unpaced C-to-C limit (nostrc-u19cz.8).
- A flow's burst headroom under host load (nostrc-u19cz.9).
- libgo running a busy daemon's fibers on one worker (nostrc-u19cz.10).
- Linux `SOCK_SEQPACKET` verification and `sendmmsg` (nostrc-u19cz.11).

## Dispatcher timer scans: nostrc-w3tfz

Measured **2026-11-19** on the M4 Max below (Release `-O3 -g`, local macOS,
not hosted/Linux qualification), base `810730485` built from an exported tree
against this branch, back to back in one session, in two ABBA rounds of
`perf_loopback.py --runs 2 --latency-samples 100` (every run reported). The
machine was shared with other agents' TSan suites and two VMs (load average
15-45), and the Rust daemon in the same invocations moved as much as the C
one, so read the medians and the `sample` evidence, not single runs.

Before, every dispatcher wake asked each handler subsystem for its nearest
deadline and polled after every batch by walking every slot of the 256-entry
leg, peer-record, configured-peer and session arrays (and the peer records
again for paths): about 2,800 slot checks over multi-kilobyte records per
wake, i.e. per packet at moderate rate. Now `handlers/timeout.c` keeps a
per-subsystem deadline cache (`hs_timers`: legs with restart holds and
configured retries, sessions, paths) that is never later than the true
earliest deadline:

- Hot paths fold the entry they changed: `hs_session_execute` (every
  successful session step, before and after execution), the session seal
  (the send counter can bring a rekey forward), a path Bundle step and
  `hs_path_account`. A fold only moves a deadline earlier, so a deadline that
  moved later costs one early wake and a rescan.
- Structural changes mark it stale: leg effects, leg free, handshake frames,
  restart holds, configured retries, session open/answer, a new Bundle,
  non-packet transport events, netmon events, every command but sends.
- Every poll pass that fires, and every tick, rescans exactly.

A forgotten fold is caught two ways. Debug builds audit the cache against an
exact scan on every wait and abort (removing the session fold aborts
`FipsdNodeTwoNodesTest`, the UDP two-node and multi-node tests at once). In
Release the cache is re-derived at the first wait 4 ms or more after an event,
and on every tick, so a missed fold makes a timer late by at most one tick
(1 s), never lost. Rescans ride existing wakes: a first version capped the
select at the end of the 4 ms window, which cost an idle pair about 2.5 extra
dispatcher wakes/s, so it was dropped. Clock reads on the dispatcher path are
unchanged: the remaining per-packet `hs_now` calls sit in `dataplane/` and
the timed-select `gettimeofday` in libgo (both outside this change).

CPU us per delivered datagram at 100 Mbit/s (~10.4k datagrams/s):

| | Base `810730485` | nostrc-w3tfz |
|---|---:|---:|
| c-c tx, 8 runs | 49.8, 50.7, 52.8, 53.9, 55.2, 39.8, 34.6, 36.8 (median 50.3) | 45.6, 35.0, 35.0, 45.6, 34.6, 35.8, 34.1, 32.2 (median 35.0) |
| c-c rx | 46.4, 48.3, 52.6, 48.0, 50.4, 40.0, 36.8, 38.0 (median 47.2) | 43.1, 36.7, 36.5, 43.0, 36.0, 36.5, 34.6, 33.1 (median 36.5) |
| c-rust C sender | 47.4, 51.9, 57.4, 46.3, 41.3, 44.4, 33.8, 40.2 (median 45.4) | 51.2, 33.4, 38.2, 50.6, 49.3, 31.9, 35.3, 40.8 (median 39.5) |
| Rust sender, rust-rust, same invocations | median 29.6 | median 23.5 |

The Rust median also fell by about a fifth in the same slots, so part of the
raw gain is a quieter machine; normalised to Rust in its own invocations the C
c-c sender gains about 12% (median ratio 1.70 to 1.49). The quietest base invocation (34.6/36.8,
36.8/38.0 c-c) against the quietest of this branch (34.1/34.6, 32.2/33.1)
agrees. `sample` of both daemons, 6 s mid-probe at 100 Mbit/s: the scans'
self time was 4.4% and 3.3% of busy samples in base and 0 here; inclusive
`hs_poll` fell from 46 and 23 samples to 2 and 1, `hs_wait_ms` (now mostly the
4 ms rescan) from 6 and 6 to 5 and 1, and busy samples overall by 15%.

Unpaced (`--seconds 5`), second round, Mbit/s: c-c base 684, 821, 732, 636
against 944, 947, 890, 966; c-rust base 827, 1077, 744, 330 against 589, 977,
1013, 982. The first round was dominated by starved runs on both sides (for
example base Rust-Rust 89 Mbit/s, this branch c-c 109 Mbit/s at 64/42% of a
core). No saturation regression is visible.

Idle: a connected pair with a proven XK session, measured with a temporary
dispatcher loop counter (not committed), over 30 s, two daemons each. Base 8.6 and 8.5 loop iterations/s (1.5 and 1.4
select timeouts/s), here 8.5 and 8.2/s (1.4 and 1.1/s); idle CPU over
32 s 0.20 s against 0.16 s per daemon.

Verification, Debug `-DGO_WARNINGS_AS_ERRORS=ON` with
`FIPSD_{UDP,TCP}_INTEROP_RUST_BINARY` set, the cache audit active:

- ASan+UBSan `ctest -R '^Fips' -E 'Soak|Perf'`: 222/222.
- TSan, same selection without `^FipsCore`: 183/183, no ThreadSanitizer report.
- TSan `--repeat until-fail:5` over STP acks (both), node handlers, two-node
  (node, UDP v4/v6, crossing dials, TCP v4/v6), rekey msg2, TCP rekey
  saturated, path medium/presence, restart-replay, the five multi-node
  topologies and the Rust multi interop: 20/20.
- Mutation check: without the `hs_session_execute` fold the audit aborts the
  node and UDP two-node and multi-node tests. Without the `hs_path_account`
  fold no test fires: kept from reading libfips-core (a roam arms the
  capability wake at once), coverage tracked as a follow-up.
- Capability skips: privileged TUN/Ethernet/IPv6 smoke, Tor, and the control
  and fipsctl interop tests.

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

**Rust-receiver bound (nostrc-u19cz).** Unpaced `c-rust` goodput measures a C
sender into the pinned Rust daemon's receiver. That receiver collapses under
unpaced overload: Rust↔Rust delivers about 55 Mbit/s at about 98% loss on hosted
runners. After the optimization pass the C sender offers more than the receiver
absorbs, so `c-rust` falls into the same collapse. Hosted run
[44700102649](https://github.com/chebizarro/nostrc/actions/runs/44700102649)
measured:
- `c-rust`: 421 → 58 Mbit/s;
- `rust-rust`: 55 → 57 Mbit/s;
- `c-c`: 434 → 924 Mbit/s.

So a hosted `c-rust` goodput drop is waived, and becomes a `::warning`, only
while `c-rust` still delivers at least 90% of the candidate's own `rust-rust`
goodput in the same run. A C sender that does worse than the Rust sender into
the same receiver still fails. `c-c` is never waived. Local mode is unchanged.
Unit tests: `test_c_rust_bounded_by_rust_receiver_warns_not_fails`,
`test_c_rust_below_rust_control_still_fails`, and
`test_rust_control_does_not_excuse_c_c`.

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

### Coordinate cache and history without O(table) work (nostrc-gfvud)

Release `fips_bench_coord_cache`, base and change binaries interleaved three
times, medians of the per-run medians (`FIPS_BENCH_ITERS=100`):

| `purge_expired` (dispatcher, every tick) | before | after |
| --- | --- | --- |
| 1,000 entries, nothing due | 0.96 us | 0.01 us |
| 50,000 entries, nothing due | 125 us | 0.01 us |
| 1,000 entries, 16 due | 1.66 us | 0.70 us |
| 50,000 entries, 16 due | 124 us | 0.94 us |

The purge pops an expiry heap instead of sweeping every slot (~51k slots of
2.7 KB at the default size), so its cost follows what it removes.

A touched cache used to force an O(len) rescan in `publish_snapshot`: the
counts swept every resident entry and the rows were taken on the way. The core
now keeps the counts as running totals (the residents' `created_at` sum and
the expiry heap), walks a residency bitmap for the rows and stops at the 256th,
so the rescan costs the rows. `fipsd_snapshot_bench --cache-churn 25` (256
retained peers, 2000 publishes), Release, CPU clock, base and change binaries
interleaved eight times, medians of the per-run figures:

| Scenario | p99 all, before | p99 all, after | cache-churn publishes p50, before | after | p99, before | after |
| --- | --- | --- | --- | --- | --- | --- |
| 50k-entry cache, touched before 25% | 492 us | 128 us | 327 us | 104 us | 610 us | 132 us |
| 4096-entry cache, touched before 25% | 159 us | 171 us | 119 us | 105 us | 205 us | 232 us |

The rescanning publish is now the same ~105 us at 4096 and 50k entries: what
is left is building the 256 control rows (strings and JSON). The 4096 p99
moves within this host's noise (load average 10-18). Every run, before and
after, has one publish over 1 ms (5-7 ms), the same in both: a history tick,
taken up below.

The dataplane snapshot (`fipsd_dp_publish`, run on every tick and after
control mutations) fingerprinted every live cache entry to decide whether to
republish, then copied them all. It now keeps what its cache section was
taken from and, while the generation is unchanged and `now` has not passed the
earliest live expiry, neither hashes nor copies the cache; a republish for
other state shares the previous section's array (ref-counted, immutable), as
does a new generation whose live entries hash the same (a demote, say).
`fipsd_snapshot_bench --dataplane` forces a dataplane republish after each
publish and times it separately (50k entries, 400 publishes, three interleaved
runs, medians of the per-run p50):

| Dataplane republish, 50k-entry cache | before | after |
| --- | --- | --- |
| cache untouched since the last (every idle tick) | 14.4 ms | 0.2 us |
| after cache traffic (touches, inserts) | 17.5 ms | 17.9 ms |

A change to the cache still rehashes and recopies every live entry (~130 MB
at 50k entries); nostrc-x345n (below) makes that incremental.

The stats history kept its peers in an address-sorted array and inserted with
`memmove`, so an arrival at a low address shifted up to every retained peer
(~283 KB each) on the live history and again in each view that replays the
tick. Peers now keep stable slots and the address order is an index over them
(see libfips-core `UPSTREAM.md`). Release `fips_bench_stats_history`, every
retained peer present each tick, base and change interleaved three times,
medians of the per-run medians:

| Tick | before | after |
| --- | --- | --- |
| 64 retained peers, no arrival | 3.0 us | 3.3 us |
| 64 retained peers, one arrival at the lowest address | 351 us | 7.0 us |
| 512 retained peers, no arrival | 45.8 us | 46.3 us |
| 512 retained peers, one arrival at the lowest address | 3122 us | 55 us |

In `fipsd_snapshot_bench` (whose arrivals all happen during setup) against the
preceding commit, six interleaved runs: 50k entries, 25% churn, p99 of all
publishes 140 -> 136 us and the worst publish 6.0 -> 1.9 ms; two slow clients
at 4096 entries p99 161 -> 165 us. The history-tick p99 there rises
(224 -> 885 us at 50k): two view replays at the fifth minute's downsample take
~0.8 ms each with ~1,200 minor page faults, first writes to view-storage pages
the setup's insertion `memmove`s used to touch. Over 20,000 publishes they
were the only replays that faulted; it is a one-time cost moved, not a
per-tick one.

End to end, base (30f44fde4) against all four changes, `fipsd_snapshot_bench
--cache-churn 25`, eight interleaved runs on a heavily loaded host (load
average 45-55), CPU clock, medians: 50k entries p99 606 -> 194 us, rescanning
publishes p50 392 -> 124 us (p99 994 -> 267 us), publishes over 1 ms 6.5 -> 1;
4096 entries p99 170 -> 154 us, rescanning publishes p50 136 -> 124 us. The
rescan costs the same at 4096 and 50k entries, and the `--max-p99-us 1000
--clock cpu` gate (`FipsdSnapshotPublishBench`) passes at 270 us.

### Indexed, incremental dataplane cache section (nostrc-x345n)

Forwarding found a destination's cached coordinates in the dataplane snapshot
by scanning every published entry with a 16-byte compare (rows ~2.6 KB apart):
O(cache) for every forwarded datagram that needed them. And any coordinate
cache change (lookups, sessions and transit hints all write it) re-hashed and
re-copied every live entry, ~130 MB at 50k, on the next publish.

The section (`src/node/dataplane/cache_view.c`) is now an index built at
publish time and shared the way the gfvud section was:

- **Index.** Rows are spread over groups of 64 parts by SipHash-2-4 of the
  address under a per-process key from host entropy (hint keys are
  peer-chosen, so a fixed hash could be flooded into one part). A part is one
  immutable allocation: a dense array of 32-bit hash tags, the rows, and their
  coordinate entries (`len` of them, not the 80-entry inline array). A lookup
  hashes, indexes group and part, scans the part's tags and compares one
  address. There are enough groups for a full cache to average at most 16 rows
  per part (64 groups and 4,096 parts at the default 50k).
- **Incremental publish.** libfips-core stamps each slot, and each block of 32
  slots, with the generation of its last change, and `fips_coord_cache_changes`
  visits the slots changed since a generation, reading one stamp for each block
  that did not change. For each reported slot, the publisher compares the row
  it would publish (address, expiry, coordinates) with the one the current
  section holds for that slot. If they differ, it marks the part holding the
  old row and the part the new row hashes to. It then rebuilds only those
  parts (the old part's other rows plus the new ones) and copies only their
  groups and the section's group table. Every other part and group is shared
  with the previous section by reference count. A new generation that changes
  no published row (a demote, a path-MTU change) keeps the section, as before.
- **Expiry, matching upstream.** The section holds every resident entry,
  expired or not, and the lookup applies upstream's `CacheEntry::is_expired`
  (`now > expires_at`), which is what upstream forwarding gets from
  `NodeRoutingView::cached_coords` calling `CoordCache::get`. The passage of
  time alone no longer forces a republish. An entry is still routable during
  its `expires_at` millisecond, where the old copy's `expires <= now` dropped
  it one millisecond early. Rows leave the section when the core removes the
  entry (purge, a touch or re-write of an expired entry, eviction,
  invalidation).

Parts, groups and sections are never written after the update that builds
them. The dispatcher and crypto workers read them through GoSwap-published
snapshots, and the last reference frees them on whichever thread drops it. A
row is 44 bytes plus 32 per coordinate entry (about 12 MB at 50k entries of
depth 6, shared across sections), where every changed section used to copy
about 130 MB.

`fipsd_dp_cache_bench` (new; built with `fipsd_tests`, run by hand, not part of
the upstream-parity micro set) drives a node's handlers. It times
`fipsd_dp_forward` to a transit destination that needs cached coordinates (a
resident hit, and a miss), and `fipsd_dp_publish` after one entry changed and
with nothing changed. Release, macOS arm64; base 572ed3215 and this change
interleaved five times (`--rounds 3 --publishes 40 --lookups 20000`); medians
of the per-run medians, dispatcher CPU time:

| | 4,096 before | 4,096 after | 50k before | 50k after |
| --- | --- | --- | --- | --- |
| lookup, resident hit (ns/packet) | 1,030 | 56 | 31,495 | 133 |
| lookup, miss (ns/packet) | 2,033 | 42 | 68,433 | 57 |
| publish after one touch | 1,219 us | 1.0 us | 17,136 us | 3.3 us |
| publish after one insert (evicting at capacity) | 1,221 us | 1.5 us | 17,039 us | 3.7 us |
| idle republish | 0.21 us | 0.13 us | 1.25 us | 0.13 us |
| first publish (whole cache) | 1.7 ms | 0.26 ms | 27.4 ms | 8.4 ms |

The lookup is O(1) at both sizes. What grows from 4,096 to 50k is memory
latency: uniformly random destinations over about 12 MB of rows miss the CPU
caches. `fipsd_snapshot_bench --dataplane` (50k entries, 400 publishes, three
interleaved runs, medians of the per-run p50): after cache traffic (eight
touches, a quarter of the time an insert) 16.8 ms -> 11.5 us; cache untouched
since the last publish 0.2 -> 0.2 us.

The four parity bench binaries still emit exactly 39 rows at
`FIPS_BENCH_ITERS=1000`, and `fips_bench_coord_cache`'s get rows are unchanged
(17/27/12/16 ns before, 18/27/11/16 after, three interleaved runs). The
loopback `Perf` ctest (`FipsdPerfLoopbackTest`, Rust peer at 6aa80504),
interleaved four times each (12 samples per pairing), shows no change beyond
this host's noise (load average 10-15): C/C 697 -> 685 Mbps, C/Rust
690 -> 693 Mbps, with the ranges overlapping.

`FipsdDataplaneCacheTest` runs random cache operation sequences: hint,
funded, stated and verified inserts, touches, demotes, removes, expiry and
purges, eviction at capacity, invalidations, clears, and re-inserts of evicted
keys. It updates a view after every one to eight operations and requires it
to equal a view built afresh from the cache: the same rows, each lookup
answering as a linear scan of the rows and as the core's `get` (present,
absent, expired, and at and just past each `expires_at`), one rebuilt part per
single touch, and an unchanged section after a demote. It fails if the old
part is not marked dirty when a row leaves it, if the core skips a block
stamp, or if an expiry change is taken for an unchanged row.
`FipsdCoordWarmTest` and `FipsdDataplaneTest` check the same through
`fipsd_dp_publish` and `fipsd_dp_forward`.

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

## Wave 26 A: exact scheduler wake routing (2026-11-21)

Code: **`33a0e20d4`** (wake routing introduced in `cb89bdccf`), based on **`96409f73c`**, branch `wave26/libgo`.
The final C/C series uses that libgo commit and otherwise the same
fipsd/native helper sources as the baseline. This is local macOS and Ubuntu
ARM64 VM evidence, not hosted qualification. No sibling UDP, native, core, pin,
or CI changes are included. libgo stays **0.3.0 Unreleased** (PATCH-class internal
performance correction folded into the unreleased version); fipsd is docs-only.

### Change and bead findings

- **u19cz.19: fixed.** Each worker has its own idle condvar. A targeted queue push
  signals only its destination. Untargeted inject/timer notifications claim a
  distinct idle wait, so two notifications cannot collapse onto the same worker
  before it reacquires the idle mutex. Untargeted selection rotates rather than
  always favoring the lowest-index idle worker. Bootstrap injection still wakes worker 0;
  stop and last-fiber completion still wake all. The state-claim protocol,
  waiter-before-arm ordering, TSan context annotations, and timer clock/skip
  policy are unchanged. The private idle-wake regression holds the mutex across
  two untargeted notifications and duplicate targeted notifications; all three
  intended workers must return. W8 external-channel and W4/two-poller idle-I/O
  registrations extend the existing lost-wake, shutdown and I/O race coverage.
- **u19cz.18: finding, not fixed.** A discarded POSIX prototype let a single idle
  worker own epoll/kqueue, with latched eventfd/EVFILT_USER interruption. Idle-only
  polling starved I/O when every worker had runnable yielding fibers (caught by
  the existing loopback tests). Adding a nonblocking readiness check every 64
  scheduling turns passed the 46-test macOS Debug suite, but the combined
  worker-poll/relative-timer experiment delivered only **49.45/41.52 Mbit/s** at
  offered 100 (loss **50.55/58.39%**). This was not an isolated measurement of
  polling alone: disabling worker polling in that combined build also regressed.
  The prototype was removed, not made an opt-in shipped path. A replacement needs
  readiness fairness while workers execute noncooperative daemon work, plus
  timer/notification ownership, rather than only moving the blocking syscall.
  The dedicated poller and the two-wake handoff remain.
- **u19cz.10: finding, not fixed.** `rq_steal_one` deliberately refuses a running
  victim, preserves its next-to-run head and applies live/queue thresholds;
  worker-originated wakes still choose the producer's local queue, not remembered
  affinity. Exact notifications do not provide placement. Removing that gate or
  enabling stealing by default is not a long-lived-fiber placement policy. No
  unwired placement API or default migration-policy change is included; real
  daemon caller/ownership decisions remain necessary.
- **u19cz.21: finding, not fixed.** The discarded Darwin relative-condvar/skip
  stage without worker polling delivered **93.13 Mbit/s, 6.63% loss** at offered
  100; relative waiting without the skip delivered **99.27 Mbit/s, 0.726% loss**.
  These preliminary shared-host samples do not establish a beneficial or safe
  timer-policy change. Both experiments were removed. The conservative REALTIME
  fallback and its per-park notification remain; no clock-step fix is claimed.
- **u19cz.7: finding, not fixed.** `MSG_DONTWAIT` makes the existing registered-fd
  message cache safe against stale state. `read`/`write` have no equivalent safety
  net. `reset_fd` invalidates a generation but does not assert exclusive ownership
  or forbid aliases clearing `O_NONBLOCK`. Silently extending caching can block a
  scheduler worker after descriptor reuse or alias mutation. TCP/TUN adoption and
  close/reset changes are outside this agent's `libgo/**` ownership; no sibling
  call sites or unused owner-registration API were added. The original bead is
  the follow-up for a caller-wired ownership contract.

### Measurements

Final **macOS**, counterbalanced candidate then baseline, 1200-byte payloads,
3-second traffic windows; CPU is sender / receiver microseconds per delivered
datagram (event-bounded traffic markers, including the harness's drain window).

| Binary | Mode | Offered / chosen max Mbit/s | Goodput Mbit/s | Loss % | CPU us/datagram S / R |
|---|---|---:|---:|---:|---:|
| base | 100 Mbit/s trial 1 | 100.00 | 99.57 | 0.426 | 26.99 / 25.07 |
| base | 100 Mbit/s trial 2 | 100.00 | 100.00 | 0.000 | 34.24 / 32.96 |
| base | paced maximum | 1112.50 | 1111.30 | 0.108 | 16.99 / 12.47 |
| base | unpaced | - | 1402.12 | 0.867 | 14.95 / 10.06 |
| final | 100 Mbit/s trial 1 | 100.00 | 99.22 | 0.784 | 30.32 / 28.06 |
| final | 100 Mbit/s trial 2 | 100.00 | 100.00 | 0.000 | 23.68 / 22.40 |
| final | paced maximum | 1600.00 | 1596.21 | 0.237 | 12.77 / 8.90 |
| final | unpaced | - | 1497.33 | 0.412 | 14.85 / 9.53 |

Final **Linux VM**, same traffic parameters, candidate then baseline:

| Binary | Mode | Offered / chosen max Mbit/s | Goodput Mbit/s | Loss % | CPU us/datagram S / R |
|---|---|---:|---:|---:|---:|
| base | 100 Mbit/s trial 1 | 100.00 | 97.14 | 2.864 | 63.91 / 49.74 |
| base | 100 Mbit/s trial 2 | 100.00 | 100.00 | 0.000 | 63.68 / 49.28 |
| base | paced maximum | 146.87 | 146.87 | 0.000 | 45.54 / 39.00 |
| base | unpaced | - | 1275.68 | 1.725 | 11.84 / 10.23 |
| final | 100 Mbit/s trial 1 | 100.00 | 100.00 | 0.000 | 52.16 / 42.24 |
| final | 100 Mbit/s trial 2 | 100.00 | 96.18 | 3.818 | 60.22 / 44.91 |
| final | paced maximum | 887.50 | 884.01 | 0.393 | 15.31 / 11.62 |
| final | unpaced | - | 1382.02 | 1.596 | 11.60 / 9.77 |

One baseline and one candidate Linux fixed-rate trial exceed 1% loss; their CPU
numbers remain visible but are **not loss-qualified comparisons**. The two
loss-free trials report sender/receiver **63.68/49.28 -> 52.16/42.24 us/datagram**.
The large movement in the chosen Linux search rate is not evidence of a stable
sixfold capacity gain: the search reuses daemon pairs, results are load- and
history-sensitive, and the earlier locked searches reported below show very
large variation. No benchmark assertion or search algorithm was weakened.


In the final eight-worker, 200-handoff Debug micro, baseline -> candidate:
**2326 -> 464 involuntary context switches**, **79.31 -> 20.00 million cycles**,
**64.49 -> 29.82 million instructions**, same **0.30 s** elapsed. This is direct
wake-path evidence, not a claim that every daemon datagram saves that ratio.




A first fully locked series at intermediate `cb89bdccf` (before rotating
untargeted selection) reported macOS unpaced **1480.53 -> 1472.20 Mbit/s**,
paced chosen rate **675.00 -> 393.75 Mbit/s**, and Linux unpaced
**1174.52 -> 1237.75 Mbit/s**, paced chosen rate **49.22 -> 196.87 Mbit/s**.
The opposite platform movements and earlier macOS baseline unpaced **350.00**
versus **1480.53** show why these are not stable release-capacity claims. In that
series one Linux baseline and one candidate fixed-rate trial exceeded 1% loss;
those trials are not loss-qualified CPU comparisons. The final series above is
counterbalanced (candidate before baseline), with this agent's host and VM
measurements serialized rather than overlapped. No earlier result was discarded.


All timing commands hold the plan's platform perf lock. The final Linux series
also holds the shared build lock to exclude concurrent VM builds. These remain
shared-host measurements: the VM and macOS share physical hardware, and other
agents' non-timing work is not globally stopped. Sustainable-rate search is a
report-only noisy capacity estimate, not a throughput gate; each reported chosen
probe must meet strict loss <1%. Unpaced goodput is not loss-free capacity.
Earlier diagnostic baseline/final and failed experiment JSON is retained under
`build/w26-evidence/` in the task worktree; it was not used to select a best run.

Reproduce the same Release closure (set `ROOT` to the absolute assigned worktree,
`/home/bizarro/w26-libgo` on Linux):

```sh
ROOT=/Users/bizarro/Documents/Projects/nostrc-wt/w26-libgo
cmake -S "$ROOT" -B "$ROOT/build/w26-release" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DFIPS_ENABLE_BENCH=ON -DBUILD_FIPS=ON \
  -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF -DBUILD_LIBHANAMI=OFF \
  -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF -DBUILD_NOSTR_GOBJECT=OFF \
  -DBUILD_NOSTR_GTK=OFF -DSIGNET_ENABLE=OFF -DWITH_NOSTRDB=OFF \
  -DLIBNOSTR_WITH_NOSTRDB=OFF
cmake --build "$ROOT/build/w26-release" -j4 --target fipsd fipsd_perf_native
lockf /tmp/w26-mac-perf.lock python3 "$ROOT/fipsd/tests/perf_loopback.py" \
  --pairs c-c --rust /tmp/fips-rust-target-perffix/release/fips \
  --fipsd "$ROOT/build/w26-release/fipsd/fipsd" \
  --native-tool "$ROOT/build/w26-release/fipsd/tests/fipsd_perf_native" \
  --latency-samples 100 --work-dir "$ROOT/build/p" --runs 2 \
  --paced-rate 100 --paced-seconds 3 --output "$ROOT/build/fixed.json"
```

For sustainable rate replace `--runs 2 --paced-rate 100 --paced-seconds 3` with
`--runs 1 --paced --paced-seconds 3` (default resolution .02, strict loss .01,
ceiling 3200). For unpaced use `--runs 1 --seconds 3` instead. The final series
runs candidate then baseline for each mode, in order fixed, paced, unpaced.
Linux uses `build/release`, `--rust /bin/true` (the C/C-only path never executes a
Rust binary), and wraps the whole series with
`flock /tmp/w26-build.lock flock /tmp/w26-perf.lock sh ...`; all Linux builds use
`flock /tmp/w26-build.lock` and `-j4`. The baseline daemon was copied before any
source edit; source, native helper and Release flags are otherwise identical.
The eight-worker wake micro uses the unchanged Debug `gof_test_chan_ext`,
`GOF_NWORKERS=8 /usr/bin/time -lp ...`, baseline then candidate under the macOS
perf lock; it is not the daemon throughput workload.

### Exact verification and limits

- **macOS Debug:** standalone `cmake -S "$ROOT/libgo" -B "$ROOT/build/w26-debug"
  -G Ninja -DCMAKE_BUILD_TYPE=Debug`; `cmake --build ... -j4`;
  `ctest --test-dir "$ROOT/build/w26-debug" -j4 --output-on-failure`: **49/49**.
- **macOS ASan/UBSan and TSan:** configure the same top-level closure as Release
  above in `build/w26-asan` / `build/w26-tsan`, with Debug, bench disabled and
  `-DNOSTR_USE_SYSTEM_NSYNC=OFF`. ASan adds `-DGO_ENABLE_ASAN=ON
  -DGO_ENABLE_UBSAN=ON -DSANITIZE=address,undefined`; TSan adds
  `-DGO_ENABLE_TSAN=ON -DSANITIZE=thread`. Build with `-j4 --target libgo/all
  libfips-core/all fipsd_tests fipsd fipsctl`. ASan environment is
  `ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1`; TSan is
  `TSAN_OPTIONS=halt_on_error=1`. For each, `ctest --test-dir "$BUILD/libgo"
  -j4 --output-on-failure`: **49/49**. `ctest --test-dir "$BUILD" -R '^Fips'
  -j4 --output-on-failure`: **211 passed, 14 skipped, 1 disabled, 0 failed**.
  The skips are external Rust control/UDP/TCP/multinode/fipsctl/Tor interop and
  privileged TUN/IPv6/Ethernet/BPF; the existing soak test is disabled. This is
  not all-226-tests-executed qualification. Apple's ASan runtime rejects
  `detect_leaks=1`; the initial invocation aborted before test bodies and was
  corrected to supported ASan/UBSan options, not reported as LSan success.
- **Linux Debug:** standalone `cmake -S /home/bizarro/w26-libgo/libgo
  -B /home/bizarro/w26-libgo/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug`, build
  `-j4`, then `ctest --test-dir .../build/debug -j4 --output-on-failure`:
  **49/49** (GCC 13.3).
- **Linux ASan/UBSan:** standalone build `build/asan-clang`, Debug,
  `-DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18
  -DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON -DNOSTR_USE_SYSTEM_NSYNC=OFF`, build
  `-j4`; `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
  UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir .../build/asan-clang -j4
  --output-on-failure`: **49/49**. The standalone vendored lookup uses a
  task-local `libgo/third_party -> ../third_party` symlink on the VM only.
- **Linux TSan:** same standalone/vendored configuration in `build/tsan` with
  Clang 18 and `-DGO_ENABLE_TSAN=ON`; `TSAN_OPTIONS=halt_on_error=1 ctest
  --test-dir .../build/tsan -j4 --output-on-failure`: **49/49**.
- **Both macOS and Linux TSan:** set `BUILD` to the macOS `build/w26-tsan/libgo`
  or Linux `build/tsan` test directory and additionally run `ctest --test-dir "$BUILD"
  -R 'GoFiber(TsanMigrationTest|SchedWakeTest|ChanExtTestW8|IoIdleMultiPollerTestW4)$'
  --repeat until-fail:10 --output-on-failure -j4` with the TSan environment:
  **40/40 executions passed per platform**.
- **Windows 11 ARM64:** `PATH=C:\msys64\clangarm64\bin;C:\msys64\usr\bin;...`,
  Clang 23.1.5, system nsync; `cmake -S C:/Users/bizarro/w26-libgo/libgo
  -B C:/Users/bizarro/w26-libgo/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug`;
  `cmake --build C:/Users/bizarro/w26-libgo/build/debug -j4`;
  `ctest --test-dir C:/Users/bizarro/w26-libgo/build/debug -j4
  --output-on-failure`: **49/49**. IOCP and Windows fiber code are unchanged.
- **Additional failure tracked as `nostrc-2k5ih`:** Linux GCC 13.3 ASan/UBSan
  timed out both starvation tests at their unchanged 20 s limit, **both on the
  candidate and independently on untouched `96409f73c`** (46/48 candidate tests
  passed; shared-host contention was not excluded). No timeout extension, retry-as-proof, suppression or assertion
  weakening was applied. Clang's passing matrix is reported separately.
- Static checks: `python3 "$ROOT/scripts/check-unsequenced-args.py" --root
  "$ROOT"`, `python3 "$ROOT/scripts/check-errno-after-park.py" --root "$ROOT"`,
  and `git -C "$ROOT" diff --check` pass. CI is unchanged (no actionlint needed).
  Oracle review's distinct-pending-wake finding was fixed and covered by the
  private regression; no re-review was substituted for execution.

Cleanup verified: `/home/bizarro/w26-libgo` and
`C:\Users\bizarro\w26-libgo` were removed after capturing logs, with no task
processes left. No build directory outside the assigned macOS worktree or
container was created. Shared locks, sibling task directories, pre-existing Rust
binaries and the unmerged branch/worktree were preserved. Detailed command logs,
all preliminary/final JSON and the discarded experiment patch remain only in the
ignored task-local `build/w26-evidence/`, not in sibling checkouts.

## Wave 26 A follow-up: owned TCP/TUN descriptors (u19cz.7)

Code **`449f2af75`**, on top of `0c8e8d50c`, completes `.7` and supersedes its
non-fix finding above. The accepted `.19` change and `.18/.10/.21` findings are
unchanged. Both TCP and TUN are included; this is not a TCP-only fallback.

### Ownership contract and production wiring

`gof_io_register_owned_fd(fd)` is a POSIX-only, explicit ownership assertion. It
sets `O_NONBLOCK`, prepares readiness state, and publishes a verified/owned bit
pair in the fd's registration-generation word. `gof_read`/`gof_write` skip
`F_GETFL` only for that owned generation. Every `gof_io_reset_fd` advances the
generation and clears both bits **before** checking readiness state, even when
reset fails. Registration checks for pending waits, and its compare/exchange
cannot publish ownership if a concurrent reset invalidated that generation.
Descriptors >=4096 remain correct but uncached. Socket messages keep their
`MSG_DONTWAIT` protection; unregistered reads/writes keep checking flags.

The owner must control **all users and aliases of the open file description**.
Cancel/join or otherwise quiesce users and reset registration before close,
reuse, transfer, or an alias clearing `O_NONBLOCK`; shared flag changes require
revoking **every registered alias**, not merely the fd used to change the flags.
Raw close/dup/flag changes are not intercepted: violating this contract can block a read/write worker. This is
not a promise to discover arbitrary aliases without a syscall.

TCP registers at both inbound configured-socket and outbound socket adoption in
`transport/tcp/conn.c`, propagating registration errors. Its existing
`tcp_fd_close` resets only after reader/writer joins, before the blocking close.
The current TCP code already reset descriptors for message caching; the new
ownership assertion removes the remaining stream-read checks and primes the
message verdict at adoption. Shared `transport/stream.c` is unchanged.

TUN's real POSIX system `prepare` registers after network-fd preparation, and its
system `close` resets before closing on setup failure or teardown. Shim shutdown
joins users before device close. Fake ops and Windows Wintun/IOCP paths are
unchanged. No UDP, native API, dataplane, core or CI changes are included.

libgo's additive API is MINOR-class, folded into **0.3.0 Unreleased**; the private
fipsd performance correction folds into **0.1.1**, its already-declared unreleased
patch after 0.1.0. No public layout/ABI break, numeric bump, wire/config/CLI change,
release tag or release-column change. `VERSION_MANIFEST.md` records this decision.

### Measured syscall saving

Ubuntu ARM64 VM, Clang 18 Debug, identical **10,000 AF_UNIX stream write/read
pairs** through the real `gof_write`/`gof_read` entrypoints in the same candidate
binary, with assertions enabled. Checked mode retains the previous mandatory
flag-check behavior; owned mode registers both descriptors before the loop.

| Mode | `F_GETFL` calls | `F_SETFL` calls | Completed pairs |
|---|---:|---:|---:|
| Checked / unregistered | 20,000 | 2 | 10,000 |
| Owned registration | 2 | 2 | 10,000 |

The two owned `F_GETFL` calls are registration setup: **zero during the I/O loop**,
19,998 fewer flag-query syscalls overall (99.99%). This is a syscall-count micro,
not a TCP/TUN throughput or daemon-capacity claim. Actual caller wiring is covered
by the TCP integration tests and the real-device IPv6 shim smoke below. No
macOS dtruss or macOS syscall-count result is claimed.

Exact measurement command, under the plan's Linux perf lock:

```sh
flock /tmp/w26-perf.lock sh -c '
  strace -f -e trace=fcntl -o /home/bizarro/w26-libgo/evidence/checked.strace \
    /home/bizarro/w26-libgo/build/debug/gof_test_io_owned --count-unowned &&
  strace -f -e trace=fcntl -o /home/bizarro/w26-libgo/evidence/owned.strace \
    /home/bizarro/w26-libgo/build/debug/gof_test_io_owned --count-owned'
grep -c F_GETFL /home/bizarro/w26-libgo/evidence/checked.strace # 20000
grep -c F_GETFL /home/bizarro/w26-libgo/evidence/owned.strace   # 2
```

### Verification

`GoFiberIoOwnedTest` covers registered read/write, revocation before alias flag
mutation, empty-read/full-write cooperative progress, pending-wait registration
rejection, failed registration on a closed fd, descriptor-number reuse, and
cached message safety despite an illicit alias change. It uses no timer to retry
I/O: a stale blocking verdict prevents the peer fiber from running and fails the
existing 10 s test timeout. The busy-waiter case uses the scheduler's block trace
for deterministic ordering. The final test-only errno accessor reacquires TLS
after any yield; its focused check was rerun in every POSIX configuration.

- **macOS libgo:** Debug, ASan/UBSan, TSan: **50/50 each**. Same configurations and
  selected top-level sanitizer closures as the preceding Wave 26 A section.
  Build: `cmake --build "$ROOT/build/w26-debug" -j4`; sanitizer builds use
  `-j4 --target libgo/all fipsd_tests`. Test commands:
  `ctest --test-dir "$ROOT/build/w26-debug" -j4 --output-on-failure`,
  `ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir
  "$ROOT/build/w26-asan/libgo" -j4 --output-on-failure`, and
  `TSAN_OPTIONS=halt_on_error=1 ctest --test-dir "$ROOT/build/w26-tsan/libgo"
  -j4 --output-on-failure`.
- **macOS Fips:** `ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1
  ctest --test-dir "$ROOT/build/w26-asan" -R '^Fips' -j4 --output-on-failure`
  and `TSAN_OPTIONS=halt_on_error=1 ctest --test-dir "$ROOT/build/w26-tsan"
  -R '^Fips' -j4 --output-on-failure`: **211 passed, 14 skipped, 1 disabled,
  0 failed per sanitizer**, with the same external/privileged limitations listed
  above. This is not all-226-tests-executed qualification or macOS LSan evidence.
- **Linux libgo:** standalone Clang 18 Debug, ASan/UBSan, TSan: **50/50 each**.
  Configure `-S /home/bizarro/w26-libgo/libgo -B /home/bizarro/w26-libgo/build/MODE
  -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang-18
  -DCMAKE_CXX_COMPILER=clang++-18`. ASan adds `-DGO_ENABLE_ASAN=ON
  -DGO_ENABLE_UBSAN=ON -DNOSTR_USE_SYSTEM_NSYNC=OFF`; TSan adds
  `-DGO_ENABLE_TSAN=ON -DNOSTR_USE_SYSTEM_NSYNC=OFF`. The task-local
  `libgo/third_party -> ../third_party` symlink supplies the pinned vendored nsync
  (instrumented under TSan). All builds use `flock /tmp/w26-build.lock` and `-j4`.
  `ctest --test-dir /home/bizarro/w26-libgo/build/MODE -j4 --output-on-failure`
  uses no sanitizer environment for Debug,
  `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1` for
  ASan, and `TSAN_OPTIONS=halt_on_error=1` for TSan.
- **Both POSIX TSan builds:** additionally
  `GOF_NWORKERS=4 TSAN_OPTIONS=halt_on_error=1 ctest --test-dir "$BUILD"
  -R '^GoFiber(IoOwnedTest|TsanMigrationTest)$' --repeat until-fail:10 -j2
  --output-on-failure`: **20/20 executions passed per platform**. `BUILD` is
  macOS `build/w26-tsan/libgo` or Linux `build/tsan`.
- **Linux TCP/TUN:** the same minimal top-level FIPS closure as above, Debug with
  Clang 18, in `/home/bizarro/w26-libgo/build/fips`; build `fipsd_tests` with the
  shared build lock and `-j4`. `ctest --test-dir /home/bizarro/w26-libgo/build/fips
  -R '^Fipsd(Tcp|Tun)' -j4 --output-on-failure`: **8 passed, 4 skipped, 0 failed**.
  Skips: the ordinary non-root TUN smoke and three external Rust TCP interop
  cases. The TUN smoke was then actually executed, not treated as acceptance by
  its skip.
- **Linux privileged real TUN:** both smokes **2/2 passed**, isolated from the
  VM's network namespace with the exact command:

  ```sh
  sudo -n unshare --net -- sh -c 'ip link set lo up &&
    FIPSD_TUN_PRIVILEGED_SMOKE=1 ctest \
      --test-dir /home/bizarro/w26-libgo/build/fips \
      -R "^Fipsd(Tun|Ipv6)PrivilegedSmokeTest$" --output-on-failure -V'
  ```

  Real device/address/MTU/self-ping/teardown passed. The real IPv6 shim reported
  **read=2, unreachable=1, written=1, host_drops=0**; the kernel accepted its
  ICMPv6 unreachable response. No `fipsSmoke` link remained after the tests, and
  exiting the private namespace released its routes. No host network settings
  were changed.
- **Windows 11 ARM64 libgo:** native Clang 23.1.5 Debug/system nsync: **49/49**.
  New owned-fd API/test are POSIX-only. With the existing clangarm64/bin and
  usr/bin PATH, `cmake -S C:/Users/bizarro/w26-libgo/libgo
  -B C:/Users/bizarro/w26-libgo/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug`;
  `cmake --build C:/Users/bizarro/w26-libgo/build/debug -j4`;
  `ctest --test-dir C:/Users/bizarro/w26-libgo/build/debug -j4 --output-on-failure`.
- Both static checkers (`check-unsequenced-args.py`, `check-errno-after-park.py`,
  each with `--root "$ROOT"`) and `git diff --check` pass. Oracle review found
  no correctness findings under the documented contract; it reviewed supplied
  source/lifecycle details, not executed tests. No new follow-up bead; the prior
  GCC ASan baseline issue `nostrc-2k5ih` is unchanged and no GCC success is claimed.

Detailed logs and syscall traces are retained in ignored, task-local
`build/w26-owned-evidence/`. Cleanup verified: `/home/bizarro/w26-libgo` and
`C:\Users\bizarro\w26-libgo` were removed after log collection, with no task
processes remaining. No outside-worktree macOS build directory, container or
persistent namespace was created. Shared locks, sibling directories and the
unmerged branch/worktree were preserved. No push or merge was performed.

## Native client receive cost and burst headroom (wave 26)

This follow-up to nostrc-u19cz.8/.9/.11 uses `96409f73c` as the before
binary, not the older queue-only `810730485`. The production native delivery
path, stream framing/readiness contract, per-flow limits and buffer sizing
are unchanged. Only the performance driver's receiver changed: drain before
polling (wait on `EAGAIN`), and verify the complete constant payload tail with
`memcmp` rather than one assertion per byte. Sequence bounds, per-record
length, timestamp handling and duplicate detection are unchanged.

### Isolated client cost, not daemon capacity

A diagnostic socketpair joined the **actual** `perf_native.c` drain function
and the unchanged plain-POSIX `client.c`, compiled with `-O3 -DNDEBUG`.
Two threads used `fips_native_send`/`fips_native_recv`, framed stream on
macOS and SEQPACKET on Linux, 1200-byte records, two seconds offered traffic
and a three-second receiver deadline. Every sent record had to be received
exactly once with its complete payload intact. Process CPU includes both
sender and receiver; it is not daemon CPU or a receive-only profile. Three
before/after pairs ran in AB/BA/AB order, under the shared perf locks. The
standalone diagnostic source/reproduction is in bead `nostrc-u19cz.8`.

| Platform | Before CPU us/record, median (range) | After | Change |
|---|---:|---:|---:|
| macOS arm64 | 5.612 (5.303–5.709) | 4.206 (4.169–4.554) | -25.1% |
| Linux arm64 VM | 2.629 (2.453–5.136) | 2.082 (1.650–2.237) | -20.8% |

This measures removal of driver overhead, **not** a production libfipsnative
speedup or a claim that the receive path no longer limits the mesh. Reading
beyond one frame inside the existing public receive API would hide bytes
from callers waiting on the descriptor; no such change was made.

### Deterministic stalled-client socket capacity

With neither endpoint reading, a nonblocking writer sent 1200-byte payloads
until refused. Both the sender's `SO_SNDBUF` and peer's `SO_RCVBUF` were set
to the requested budget. These are kernel/socket diagnostics, not end-to-end
bursts or measured scheduling latency. Linux reports doubled accounting
values; neither platform's reported option is a measurement of reserved RAM.

| Platform / flow | Requested bytes | Reported sndbuf / rcvbuf | Whole records accepted |
|---|---:|---:|---:|
| macOS framed stream (1204 wire bytes) | 65,536 | 65,536 / 65,536 | 54 |
| macOS framed stream | 262,144 | 262,144 / 262,144 | 217 |
| macOS datagram | 65,536 | 65,536 / 65,536 | 53 |
| macOS datagram | 262,144 | 262,144 / 262,144 | 215 |
| Linux SEQPACKET | 65,536 | 131,072 / 131,072 | 57 |
| Linux SEQPACKET | 262,144 | 524,288 / 524,288 | 228 |

At the default 16-slot flow queue, 54 socket records plus 16 queued records
are about 84,000 payload bytes: only 0.96 ms of idealized headroom at
700 Mbit/s (2.24 ms at 300 Mbit/s). Actual queue/feeder occupancy and kernel
accounting differ; this is not a guaranteed stall tolerance. Quadrupling the
socket budget buys roughly four times this socket capacity, not a fix for a
persistently slow client, and multiplies per-flow socket memory allowances.
No default increase or new configuration knob is justified by a shared,
heavily loaded host alone. Framed streams still park losslessly when the
queue can hold the burst; an exhausted bounded queue still counts drops.
FreeBSD's existing counted datagram drops are unchanged and were not
qualified on a FreeBSD host.

### Locked full-path comparison

Each platform ran three AB/BA/AB before/after pairs at fixed 300/500/700
payload Mbit/s and unpaced, three seconds each, 100 latency samples, C/C
only. `perf_loopback.py` still checks every payload and duplicate and records
native counter deltas. The host was shared with other wave-26 work and
sanitizer tests; the perf locks prevent simultaneous timing runs, not other
host work. Initial macOS measurements during compilation (load average
89) are discarded. These are **loaded-host observations, not idle capacity
baselines**; the ranges are too wide to infer an end-to-end improvement or
attribute a limit to native `send` syscalls. `sendmmsg` is therefore not
introduced speculatively. Linux direct SEQPACKET delivery is qualified by
its native API, ordered direct-write, stalled-reader and slow-reader tests,
not by a claim that it is faster than an unrelated historical revision.

| Platform / offered Mbit/s | Before delivered Mbit/s median (range) | After | Before / after median loss |
|---|---:|---:|---:|
| macOS / 300 | 298.6 (290.1–298.7) | 298.9 (298.4–299.2) | 0.46% / 0.36% |
| macOS / 500 | 495.0 (312.7–500.0) | 452.5 (406.1–490.4) | 0.99% / 9.51% |
| macOS / 700 | 690.7 (548.0–698.2) | 647.2 (217.7–696.4) | 1.33% / 7.55% |
| macOS / unpaced | 1527.4 (203.2–1562.9) | 1529.2 (660.3–1533.4) | 0.40% / 0.74% |
| Linux VM / 300 | 231.1 (146.8–288.3) | 297.2 (213.6–298.8) | 22.94% / 0.89% |
| Linux VM / 500 | 486.7 (282.6–492.4) | 467.8 (460.9–489.9) | 2.67% / 6.44% |
| Linux VM / 700 | 606.0 (402.8–696.8) | 546.3 (487.8–690.5) | 13.42% / 21.96% |
| Linux VM / unpaced | 750.9 (328.4–1246.4) | 666.4 (184.0–1191.9) | 15.99% / 19.54% |

Fixed-rate loss includes offers refused by local send-side backpressure, not
only receiver queue drops. Receiver `native.drop_flow_queue_full` increments
were observed on both versions; the socket/queue budgets remain bounded.
The modest/no full-path median improvement, wide ranges and high host load
are a written finding for the production receive-cost and send-batching
hypotheses, not proof of a regression or a production throughput fix.

Reproduction (absolute roots; use saved before binaries from `96409f73c` and
candidate binaries, alternate AB/BA/AB rather than run all before first):

```sh
# macOS; on Linux use ROOT=/home/bizarro/w26-native/src,
# BUILD=/home/bizarro/w26-native/build and flock /tmp/w26-perf.lock.
ROOT=/Users/bizarro/Documents/Projects/nostrc-wt/w26-native
BUILD="$ROOT/.build-release"
lockf /tmp/w26-mac-perf.lock python3 "$ROOT/fipsd/tests/perf_loopback.py" \
  --fipsd "$BUILD/fipsd/fipsd" \
  --native-tool "$BUILD/fipsd/tests/fipsd_perf_native" \
  --rust /tmp/fips-rust-target-perffix/release/fips \
  --pairs c-c --runs 1 --latency-samples 100 --paced-rate 300 --paced-seconds 3 \
  --work-dir /tmp/w26-c-measure/repro --output /tmp/w26-c-measure/repro.json
# Repeat at 500 and 700; for unpaced replace the two --paced-* flags with --seconds 3.
# For Linux C/C only, --rust was the C daemon path (unused with --pairs c-c),
# not a Rust control measurement. Native clients and daemons are both real C.
```

### Qualification and socket-directory consolidation

`nostrc-jyag5` moves the four copies (control, native API, BLE-agent control,
gateway control) into `fipsd_privilege_socket_parent` in `privilege.c`.
The live-control regression for a missing directory beneath a symlinked
ancestor (`/var` on macOS) is retained. The shared helper test additionally
checks canonical XDG modes, unchanged existing custom/ancestor modes, final
directory symlink/non-directory refusals, path bounds and newly created
intermediate directories under umask 0077. All newly created directories
receive the requested group and 0750; pre-existing ancestors are untouched.
These PATCH-class daemon corrections fold into unreleased fipsd 0.1.1;
libfipsnative is unchanged, and the performance driver is test-only. Other
initial unreleased components retain their versions; no release/tag changes.

Final commands and results (all build and VM directories were task-owned):

```sh
ROOT=/Users/bizarro/Documents/Projects/nostrc-wt/w26-native
ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir "$ROOT/.build-asan" -R '^Fips' -j6 --no-tests=error --output-on-failure
TSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir "$ROOT/.build-tsan" -R '^Fips' -j6 --no-tests=error --output-on-failure
# Linux VM
ctest --test-dir /home/bizarro/w26-native/build \
  -R '^Fipsd(Native|Control|Privilege|BleAgent|GatewayControl)' \
  --no-tests=error --output-on-failure
python3 /home/bizarro/w26-native/src/fipsd/tests/udp_interop.py \
  --fipsd /home/bizarro/w26-native/build/fipsd/fipsd \
  --rust /home/bizarro/w26-native/rust-target/release/fips \
  --native-tool /home/bizarro/w26-native/build/fipsd/tests/fipsd_native_client_tool \
  --rust-echo /home/bizarro/w26-native/rust-target/release/examples/native-echo --family 4
# Windows VM (PowerShell, MSYS2 CLANGARM64 tools on PATH)
cmake --build C:/Users/bizarro/w26-native/build -j4 --target fipsd fipsd_tests
ctest --test-dir C:/Users/bizarro/w26-native/build \
  -R '^Fipsd(Native|Control)' --no-tests=error --output-on-failure
```

| Platform / configuration | Final result |
|---|---|
| macOS Debug/Werror ASan+UBSan | 214 passed, 11 skipped, 1 disabled, 0 failed; 135.90 s |
| macOS Debug/Werror TSan, vendored instrumented nsync | 214 passed, 11 skipped, 1 disabled, 0 failed; 202.28 s |
| Linux arm64 Release/Werror native/control/privilege/BLE/gateway | 13 passed, 2 skipped, 0 failed; 14.40 s |
| Linux C-client→Rust-daemon and unmodified Rust-client→fipsd | Both daemon initiator roles passed, IPv4 |
| Windows 11 arm64 Debug/Werror build + TCP-control tests | Build passed; 2 passed, 0 failed; 14.67 s |

macOS builds use `GO_ENABLE_ASAN=ON, GO_ENABLE_UBSAN=ON` or
`GO_ENABLE_TSAN=ON, NOSTR_USE_SYSTEM_NSYNC=OFF`, and build `fipsd_tests` plus
`libfips-core/all`. This is not an LSan qualification (`detect_leaks=0`).
The eleven macOS skips are four privileged/BPF smokes and seven unbound
optional Rust-control/TCP/Tor interop gates; the mixed UDP soak is disabled
by default. The real UDP C↔Rust gates (IPv4/IPv6 and multi-node) **ran and
passed** in both sanitizer selections. A separate macOS ASan C-client↔Rust
daemon IPv4 run passed both roles; the pinned 6aa80504 Rust `native-echo`
example explicitly refuses macOS, so the Rust-client direction is qualified
on Linux instead (the failing macOS invocation is not a pass). The Linux
skips are unbound Rust-fipsctl control interop and the macOS-only kqueue
characterization. Windows excludes the POSIX helper and native API (no
SCM_RIGHTS); its two selected tests are TCP control, not native-API proof.

`test_perf_loopback.py` passed all nine tests. All five pre-push static
checkers and `git diff --check` passed; no workflow changes, actionlint need,
Nostr protocol edits or sibling-owned source changes.
