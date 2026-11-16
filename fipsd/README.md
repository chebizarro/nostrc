# fipsd

The host daemon for the C FIPS port. `libfips-core` owns pure protocol
computation; this component drives it using libgo concurrency, the `go_fiber`
runtime and libnostr. Sockets, clocks, secure randomness, configuration,
logging, signals, privileges and platform-specific work belong to this host,
never to the core.

## Host components

| File | Role |
| ---- | ---- |
| `src/main.c` | Entry point: parse flags, open the log first, umask, bring the runtime up, run until a stop, map the result to an exit code. |
| `src/cli.{c,h}` | `getopt_long` parsing of exactly the documented flags; version and help text; `fipsd_options` (`--config`, `--log-file`) supplied to the configuration loader. |
| `src/log.{c,h}` | Structured logging: levels, UTC timestamps, module tags, injectable sink, size-rolled file sink. |
| `src/runtime.{c,h}` | Fiber runtime bring-up, signal fiber, root `GoContext`, reload channel, subsystem vtable, orderly bounded shutdown. |
| `src/privilege.{c,h}` | umask, `fips` group resolution, socket/directory ownership and mode helpers, no-op privilege-drop hook. |
| `src/host_os.{c,h}` | `fips/host.h` adapters: `CLOCK_MONOTONIC` clock, libsodium entropy. |
| `cmake/BuildInfo.cmake` | Build-time capture of the git revision and target triple for the version strings. |

`src/config/` loads and validates YAML, publishes immutable snapshots and
consumes the runtime's SIGHUP reload channel. `src/control/` implements the
Unix control socket (`9keoz.3`); `src/node/handlers/` implements the node
protocol handlers (`9keoz.6`). Not implemented yet: Tier B2 network
transports and the native host owner. The B4 TUN device owner is described below;
the IPv6 packet shim is not implemented yet. Parsing a setting does not claim that its owning subsystem is
protocol handlers (`9keoz.6`). `src/transport/udp/` implements real UDP and
authenticated connected-peer drains (`gfi8h.2`). Not implemented yet: the
other Tier B2 network transports and the native/TUN host
owners (B4). Parsing a setting does not claim that its owning subsystem is
implemented.

### Command line (docs/reference/cli-fips.md upstream)

```
fipsd [-c FILE] [--log-file FILE]
```

| Flag | Effect | Exit |
| ---- | ------ | ---- |
| `-c`, `--config FILE` | Configuration file; skips the default search paths. | — |
| `--log-file FILE` | Log to FILE instead of stderr, rolled by size; opened before anything else so a start-up error is logged there too. An unopenable file is fatal. | 1 on failure |
| `-V` | `fipsd <version> (rev <git-hash>[-dirty])`; `rev` omitted when the build could not read git. | 0 |
| `--version` | Short version, then `target: <triple>` and `libfips-core: <version>`. | 0 |
| `-h`, `--help` | Usage. Wins over `-V`/`--version`, as with clap. | 0 |
| unknown flag, missing value, positional | `error: ...`, usage and `For more information, try '--help'.` on stderr. | 2 |
| normal run | Foreground until `SIGINT`/`SIGTERM`. | 0 clean; 1 start-up failure or shutdown grace elapsed |

Environment: `FIPSD_WORKERS`
(scheduler worker count, 1..64), `FIPSD_LOG_LEVEL` (`error|warn|info|debug|trace`, overrides `node.log_level`).

### YAML configuration (`nostrc-9keoz.2`)

The host links **libyaml** (`pkg-config yaml-0.1`; Debian/Ubuntu `libyaml-dev`,
Homebrew `libyaml`). No YAML dependency or parsing is added to libfips-core.

`-c FILE` reads only that file; a missing/unreadable/invalid explicit file is
fatal (exit 1), with the reason sent to the already-open CLI `--log-file` sink
or stderr. Without `-c`, existing files overlay in this order:

1. `/etc/fips/fips.yaml`;
2. `/usr/local/etc/fips/fips.yaml` on macOS/FreeBSD;
3. the platform user config directory plus `fips/fips.yaml` (Linux:
   `$XDG_CONFIG_HOME` or `~/.config`; macOS: `~/Library/Application Support`);
4. `~/.fips.yaml`;
5. `./fips.yaml`.

Node settings layer per explicitly named key (including a later explicit
built-in default). Identity `persistent` and DNS/TUN booleans layer only when
named; `leaf_only: true` remains sticky, matching upstream. Each transport type
replaces whole, nonempty peer lists replace whole, and gateway replaces whole.
A null section leaves an earlier section alone. No files means built-in
defaults; files which together configure no identity, transport, peers or
gateway are refused. Empty partial files can accompany a useful one.

The schema tables in `src/config/{node,transport,peer,gateway,schema}.c` cover
all configuration reference keys (218 scalar/scalar-list paths plus record
collections): node identity/limits/rates/retry/netmon/cache/
lookup/rendezvous/tree/bloom/session/buffers/control/native API/MMP/ECN/rekey/path/
logging, TUN, DNS, six transport types with named instances, peers and gateway
(including grants and port forwards). Both deprecated discovery tables and
Ethernet `discovery` are accepted; removed tick/rekey switches warn and do not
change behavior. Unknown keys warn and are discarded. Enums, integer widths,
required fields, grant authorization and cross-field constraints are checked.
Semantic refusal/warning text follows the pinned Rust sources; YAML syntax and
scalar-type diagnostics are libyaml/C diagnostics, **not byte-for-byte serde_yaml
errors**. Non-finite/negative handshake refill rates are additionally refused
because the C core's rate-limit API requires finite, nonnegative rates.

Protocol floors/ceilings match `floors.rs`, including the non-mutating warnings
for discovery resend counts and link-rekey handshake caps. Coordinate TTLs
map to saturating milliseconds; `-DFIPSD_TEST_OVERRIDES=ON` alone honours
`node.cache.test_coord_ttl_secs` below the 300-second floor. Ordinary builds
recognize but ignore that key with the upstream warning. Core adapters expose
validated rekey configs, independent handshake/setup buckets, cache sizing/TTL
and MMP mode/window/log intervals (`fipsd_config_core_params`). No core state,
socket or identity key is created by parsing. Secret values are redacted from
dumps and owned scalar buffers are zeroized on release.

**Deliberate policy difference:** the upstream default relay list includes a
relay forbidden by this repository's AGENTS.md. The C defaults omit it, keeping
`wss://nos.lol` and `wss://offchain.pub`; no banned relay is introduced in code,
fixtures or goldens. Parsing rendezvous configuration does not connect relays.

`fipsd_config`/values are opaque and read-only. `GoSwap` owns each snapshot;
readers hold a `GoRefPtr` from `go_swap_load()` until `go_refptr_release()`.
Snapshot contents need no application lock, but **GoSwap itself currently uses
a short mutex for load-and-retain; it is not a lock-free API**. This task does
not alter libgo's lifetime/reclamation contract.

SIGHUP coalesces on the runtime reload channel. A tracked fiber selects reload
or cancellation, and `go_blocking_submit` performs bounded regular-file reads
and parsing off the scheduler. A failed reload keeps the entire last good
snapshot. Successful reloads apply **only `node.log_level`,
`node.log_max_size_mb`, `node.log_max_files`**; every other changed setting is
retained and logged as requiring restart. ACL/hosts resources are rebuilt in the same
config snapshot on SIGHUP (see below); this introduces no YAML ACL keys. CLI file selection/environment level
precedence remains in effect. Log limits change under the logger's write mutex.
Upstream `node/reloadable.rs` defines hosts/ACL resource snapshots, **not a YAML
reloadable-key list**; upstream `fips.rs` has no SIGHUP YAML reload. Therefore
this whitelist is an explicit C-host contract, not a claim of upstream YAML
reload parity. Peer-list changes still require restart; the active peer-alias base is retained
on YAML reload. ACL/hosts file changes are applied by the config resource owner.

The load log captures all defaults (DEBUG), bounds/deprecations/unknown keys
(WARN) and loaded paths (INFO) before the final logger is configured; replay
uses the configured filter. Parser resource limits: 8 MiB regular file,
64 nesting levels, 100000 expanded value nodes, one YAML document, no embedded
NUL scalars or custom tags; duplicate keys are refused.

Tests include complete effective golden dumps for the shipped config and all
32 upstream testing YAMLs, with two raw templates recorded as parser refusals.
Scenario/topology YAMLs are **not daemon configs**: parsing a partial document
ignores their unknown root keys and dumps defaults; the file-loading entrypoint
separately rejects configurations that configure nothing. See
`tests/config_golden/README.md` for oracle/provenance and default checks. The
self-contained fixture copies remove comment lines and normalize trailing blank lines only. Configure with
`-DFIPSD_UPSTREAM_CONFIG_DIR=/absolute/read-only/fips` to also replay the original
33 files directly. Malformed corpus replay parses every seed prefix; unit and
real-process tests exercise SIGHUP, restart-only rejection, failed reload,
layering, immutable retained snapshots and concurrent readers. Build the new
tests through the existing `fipsd_tests` aggregate.


### Admission ACLs, metrics, history and diagnostics (`nostrc-9keoz.8`)

| File | Role |
| ---- | ---- |
| `src/node/acl.{c,h}` | Immutable file-backed npub/alias policy, upstream allow-first decision order, sorted effective/status entries and last-good reload guards. |
| `src/node/metrics.{c,h}` | Upstream counter vocabulary and relaxed atomic registry; typed handler/dataplane counter producers, immutable provider copies. |
| `src/node/stats.{c,h}` | Dispatcher counters and one-second collection into existing `fips_stats_history` node/per-peer rings, with retained-peer storage growth. |
| `src/node/diag.{c,h}` | Lowercase diagnostic hex and upstream node-wide unauthenticated-line budget (burst 10, 1/sec, saturating withheld count). |

**Policy follows the checked Rust source, not a new firewall design.** There is
no `node.acl` YAML section or configurable default-deny key. The inputs are
`peers.allow`, `peers.deny` and `hosts` beside the platform system config:
`/usr/local/etc/fips` on macOS/FreeBSD, `/etc/fips` on other supported Unix hosts.
`peers[].alias` supplies the alias base; case-insensitive hosts-file aliases win
on conflicts. ACL files take one npub or alias per line, `#` inline comments,
and case-insensitive `ALL`. Allow matches override deny matches; unmatched peers
are allowed. An allowlist alone does **not** imply default deny: put `ALL` in
`peers.deny` to obtain allow-then-deny-all. Invalid lines are skipped with warnings.
No prefix/wildcard syntax other than `ALL` is introduced.

Admission gates are outbound dial (before reservation/index/DH), authenticated
inbound IK admission (after encrypted-key recovery/classification, before index
allocation and reply DH), and authenticated outbound IK completion (before
promotion, catching reloads during a handshake). The existing-link rekey path
bypasses new admission. Inbound restart candidates are rechecked at key
confirmation (`inbound_confirmation`), so a reload while msg2 is in flight
cannot let a now-denied identity evict a live link. There are **no session-setup
or forwarding ACL checks**, and reload does not disconnect existing peers.

`main` loads policy before publishing the initial config generation. Parsing
YAML alone performs no ACL I/O; embedding/tests opt in with
`fipsd_config_enable_acl` before publication and can supply isolated absolute
resource paths. The single config service serializes SIGHUP and one-second
resource-mtime maintenance on its blocking executor. Readers retain the config
snapshot with `GoSwap`; dispatcher admission never reads files. Missing files
are empty policy. Reload read/UTF-8 failures hold the last-good policy and retry;
a clean in-place emptying of enforcing ACL files is held once before accepting
it. Deletion and hosts-only alias removal apply immediately. Startup read faults
start empty and retry, matching upstream's explicitly logged behavior. Automatic
change detection uses upstream's mtime contract; timestamp-preserving edits need
SIGHUP. No new config schema/defaults or config-golden changes are required.

`show_acl` now projects actual policy. Rejection counts are upstream
`show_metrics.handshake.bad_state`, **not an invented `show_acl` counter**.
`show_metrics` is a documented upstream command; it exposes all 154 upstream
uint64 counter names in the ten families below (plus the deprecated `discovery`
alias of `lookup`). No gauges/histograms or C-only handshake progress names are
added. Dispatcher totals and typed rejection counters feed the registry;
control readers receive retained immutable copies after changes/maintenance,
not pointers into mutable counters. Fields with no producer in the currently
ported host stay zero rather than manufacturing activity from an unimplemented
flow, TUN, cache-budget or kernel-drop owner:

| Family | Upstream names | Current producer names | Names awaiting a corresponding host producer |
| ------ | -------------- | ---------------------- | ------------------------------------------- |
| `forwarding` | 31 | 24 | `coord_hint_changed`, `coord_hint_rejected`, `coord_hint_unfunded`, `coord_warm_foreign_root`, `coord_warm_key_mismatch`, `warm_malformed_bytes`, `warm_malformed_packets` |
| `lookup` | 26 | 24 | `req_dedup_cache_full`, `resp_no_route` |
| `tree` | 17 | 17 | None |
| `bloom` | 12 | 11 | `debounce_suppressed` |
| `congestion` | 4 | 2 | `congestion_detected`, `kernel_drop_events` |
| `errors` | 18 | 5 | `broken_below_quorum`, `broken_demoted`, `broken_link_mismatch`, `broken_reporter_mismatch`, `emit_over_peer_budget`, `lookup_resp_mtu_below_floor`, `mtu_exceeded_below_floor`, `mtu_exceeded_uncorroborated`, `path_mtu_notif_below_floor`, `unbound_broken`, `unbound_coords`, `unbound_forged`, `unbound_mtu` |
| `native` | 18 | 4 | `drop_arrival_queue_full`, `drop_backlog_full`, `drop_flow_queue_full`, `drop_listener_gone`, `drop_listener_not_reading`, `drop_no_port`, `drop_oversize`, `drop_peer_share`, `drop_pending_queue_full`, `drop_too_many_flows`, `flows_accepted`, `flows_closed`, `flows_expired`, `flows_opened` |
| `dataplane` | 2 | 1 | `orphaned_index` |
| `handshake` | 8 | 8 | None |
| `session` | 18 | 16 | `addr_mismatch`, `half_open_full` |

`show_stats_list`, `show_stats_history`, `show_stats_peers`,
`show_stats_all_history` and `show_stats_history_all_peers` use the existing
core rings, delta baselines, 60:1 downsampling, 24-hour peer retention and
non-finite-to-null visitor. Collection follows upstream's forwarding-byte/packet
basis and session-table length; node totals do not fall when a link disappears.
Every active peer is sampled (no former 64-peer truncation), and retained storage
grows for churn. Mesh size is the rounded cardinality of the OR-union of every
peer's inbound filter plus self; no contributing filter means unknown.
`show_status` sparklines read the same rings. Bloom diagnostic fields describe
the last filter actually sent to the parent, remaining null at a root or before
that send. Existing peers/links/MMP/tree/bloom/probe diagnostic surfaces stay
live; **upstream has no diagnostic query command**, so none is invented.

Tests include actual ACL-loader canonical replay of `show_acl.json`, all five
existing `show_stats_*.json` goldens through a real empty history owner, fake-fed
and concurrent registry projection, >64-peer sampling/retained churn, and a
real two-node denied-dial/denied-inbound -> SIGHUP -> allowed link/datagram/rekey
trace with nonempty node/peer histories. The pre-existing control corpus still
matches byte-for-byte after its canonicalization. There is **no upstream
`show_metrics.json` golden** in this checkout: metric names/types and feeds are
asserted directly instead. No new golden is presented as upstream evidence.

### Logging

One line per record, `2026-11-15T19:10:10.849Z INFO  runtime: message`.
Records above the level are dropped before formatting; the formatted line
(bounded at 1 KiB, truncated with `...`) is handed to the sink under a short
nsync mutex. The default sink is stderr; `--log-file` installs the rolling file
sink: live file keeps its name, rolled files are `<name>.1` (newest) to
`<name>.<max_files>`, a write that would take a non-empty file past the cap
rolls first, a failed roll keeps writing and retries after another cap, a
symlink at the path is refused, nothing else in the directory is touched.
Defaults are upstream's `node.log_max_size_mb` = 10 and `node.log_max_files` =
4, clamped as documented (size ≥ 1 MiB, 1 ≤ files ≤ 100);
`fipsd_log_file_set_limits()` applies the configured values once they are
known. The sink is injectable (`fipsd_log_sink`) so tests capture lines.

Fiber rule: a log call costs one `write(2)` under the mutex, no fsync, no
retry loop, so a fiber is never held on a slow disk longer than one append.

### Runtime and shutdown

```
create → register subsystems → start → run → destroy

start: size scheduler (FIPSD_WORKERS | cfg | GOF_NWORKERS | online CPUs;
       pollers = clamp(workers/4, 1, 4)) → gof_init → spawn signal fiber →
       gof_start_background → blocking executor → wait for the signal fiber's
       ready token (scheduler live, fiber hooks verified) → install
       SIGINT/SIGTERM/SIGHUP handlers → subsystem start() in order
run:   main thread waits on the root context's done channel
       ← SIGINT/SIGTERM via signal fiber, or fipsd_runtime_shutdown()
       → subsystem stop() in reverse order
       → wait for the subsystem wait group, bounded by grace_ms (default 5 s)
       → restore signal dispositions, stop the signal fiber
       → gof_request_stop → gof_join_background → blocking executor shutdown
```

Signals become events through a self-pipe: the async-signal-safe handler
writes the signal number to a non-blocking pipe; a fiber parked in `gof_read()`
dispatches it. `SIGINT`/`SIGTERM` cancel the root context; `SIGHUP` puts one
token on the reload channel (capacity 1, so repeats coalesce) for the
configuration loader to consume. The signal fiber is spawned *before* the
background workers because libgo's workers exit as soon as no fiber is live;
it anchors the scheduler for the whole run. The handled signals are unblocked
on the main thread after the handler is installed, so a stop that a spawner
left blocked still reaches the handler. The main thread is not a fiber and
waits with nsync; no fiber ever blocks on a raw syscall other than `gof_*`.

Subsystems register a `fipsd_subsystem` vtable (`name`, `start`, `stop`,
`user`). `start()` runs on the starting thread and spawns fibers through
`fipsd_runtime_spawn()`, which accounts for them in the wait group; fibers
observe `fipsd_runtime_context()` (its done channel is a select case) and
return after cancellation. `stop()` runs in reverse order after cancellation and
must return promptly. A failing `start()` stops what started and tears the
runtime down (exit 1). A subsystem fiber that outlives the grace is logged and
the process exits 1 rather than hanging.

Known libgo behaviours this design works around, to be fixed upstream in libgo:

`go_fiber` now has ThreadSanitizer fiber annotations (`nostrc-9965o`);
runtime, peer and node tests exercise two migrating scheduler workers under TSan.
The node native smoke exposed an older gof_read errno TLS-address cache across
migration; its loop now uses the same worker-local errno helpers as message I/O.
Those helpers use relaxed scalar accesses for the errno cell shared by fibers on
one OS worker, without synchronizing unrelated application memory or suppressing
TSan checks.
- `go_fiber` carries TSan fiber annotations; concurrent-spawn arguments are
  caller-local. Runtime/control tests retain two-worker migration coverage.

- `gof_read()` without a deadline parks in 2 ms slices, so the idle signal
  fiber wakes ~500 times a second. Harmless but not free.
- libgo's `GoChannel` calls weak `gof_hook_*` stubs that `go_fiber` overrides.
  With static archives the stubs win unless the `go_fiber` archive is scanned
  while a hook is undefined, so `fipsd_lib` links `go_fiber` before `libgo`
  and `runtime.c` references `gof_hook_current()`; the start-up barrier
  verifies the real hooks are live and refuses to run otherwise (a fiber
  blocked in a channel wait would otherwise stall its worker thread).

### Privileges (control-socket.md, run-as-unprivileged-user.md upstream)

`fipsd_privilege_set_umask()` applies 0027 at start-up. Helpers resolve the
`fips` group and set a bound socket to `fips` 0770 and a private parent
directory to `fips` 0750 (directory changes go through an opened
`O_DIRECTORY|O_NOFOLLOW` descriptor; a symlink or non-socket is refused). Which
parents qualify (daemon-created, or canonical `/run/fips`, `/var/run/fips`,
`$XDG_RUNTIME_DIR/fips`) is the control-socket owner's decision. The daemon
runs as root by default; unprivileged operation needs `CAP_NET_ADMIN` for the
TUN device, a `fips` user in the `fips` group, a readable config directory and
`RuntimeDirectory=fips`; privilege dropping is delegated to the service manager
(`User=`/`Group=`) and `fipsd_privilege_drop()` refuses anything else.

## Architecture sketch for the host (dispatcher/worker model)

The Tokio host becomes fibers on `go_fiber` with libgo primitives
(`/tmp/fips-port-assessment.md` §5.3): `tokio::spawn` → `fipsd_runtime_spawn`;
`mpsc::channel(n)` → `go_channel_create(n)` with a bound chosen from the
config limits; `oneshot` → capacity-1 channel closed after one send;
`select!` → `go_select_timeout` with the context's done channel as a case;
`interval` → `create_ticker`; `CancellationToken` → the root `GoContext`;
`Arc<Mutex<T>>` → `refptr` + `nsync_mu` held briefly and never across a
`gof_*` call; `watch`/`ArcSwap` → `GoSwap` snapshots; blocking calls
(`getaddrinfo`, D-Bus, disk) → `go_blocking_submit`; CPU-bound ticks yield.

Per transport the shape is one **dispatcher** fiber owning the socket receive
path and the session's single-writer state, and N **worker** fibers doing the
crypto. The libfips-core Noise audit fixes the threading contract the workers
must follow: cipher clones are per-worker independent states that never share
counters; the dispatcher serializes send-counter reservation across all clones
of a key and workers seal with those explicit counters, never auto-sealing from
the same snapshot; decrypt workers authenticate, and the sole replay-window
owner (the dispatcher) rechecks and marks only on acceptance; lifecycle and
mutation need host synchronization (`libfips-core/README.md`, `session.h`).
The node's lifecycle is a `fipsd_subsystem` after configuration; the control
subsystem can follow it, loading `node/status.h` snapshots without taking a
node lock.

## Build and tests

`fipsd_lib` (everything but `main()`) is shared by the daemon and the tests.
`BuildInfo.cmake` regenerates `fipsd_build_info.h` on every build, rewriting
it only when the revision changes. Tests (`ctest -R '^Fipsd'`, all
event-driven, no sleeps): `FipsdCliTest` (real binary: flags, exit codes,
output shape), `FipsdCliParseTest`, `FipsdLogTest` (filtering, format,
rolling into a temp dir), `FipsdRuntimeSignalsTest` (start → SIGHUP reload
token → SIGTERM → exit, reverse stop order), `FipsdRuntimeStartFailureTest`
(failed start tears down; a second runtime in the same process),
`FipsdPrivilegeTest` (temp socket/dir; `fips`-group assignment is reported as
skipped when the group is absent or not assignable), `FipsdProcessTest` (real
binary: SIGTERM → 0, SIGINT, `--log-file` ownership and failure → 1),
`FipsdHostOsTest`. The `fipsd_tests` target builds every test executable; the
FIPS CI lane must build it (or list the new executables) for `ctest -R
'^Fips'` to find them. The TSan lane applies to all of them.

Tests that create files or Unix sockets use `tests/test_tmpdir.h`:
`fipsd_test_tmpdir("/runtime/fips/control.sock")` creates a private, short
`fd-XXXXXX` directory under `$TMPDIR` (unset/empty falls back to `/tmp`). Pass
the **longest socket suffix including its leading slash**, or `NULL` for
file-only tests. The helper rejects a directory + suffix whose byte length
would not leave a NUL in the platform's `sockaddr_un.sun_path` (104 bytes on
macOS, 108 on Linux), naming the full offending path and asking for a shorter
`TMPDIR` before any bind. Directories and remaining contents are removed at
normal process exit without following symlinks; explicit cleanup assertions
remain useful. Call it on the main thread before starting workers. Every
`fipsd_unit_test` target links the helper; new socket tests must use it rather
than source/build-rooted paths. The Python control interop test runs through
`fipsd_test_tmpdir_run /control.sock <command> [args...]`, which supplies the
same checked directory as `FIPSD_TEST_TMPDIR` and cleans up after the child
(including skipped/failed children). Source paths are only for read-only
fixtures/goldens, never temporary sockets.

The top-level `BUILD_FIPS` option defaults ON. `fipsd` is configured only on
platforms with a `go_fiber` target (currently not Windows). `CMakeLists.txt`
owns the component's version, 0.1.0 Unreleased. Warning flags honor nostrc's
shared `GO_WARNINGS_AS_ERRORS`; sanitizer flags reuse its `GO_ENABLE_*`
helpers.

## Peer owners (nostrc-9keoz.4)

`src/peer/active.h` is an opaque, stable-address `fipsd_active_peer`. Its public
`fipsd_peer_info` exposes identity (including node/IPv6 addresses and public key),
transport/address, link id/direction, lifecycle, tree declaration/ancestry, inbound
filter/sequence, link statistics, heartbeat/heard stamps and pointer-free MMP
metrics. `data.h` mirrors the non-secret peer record; `machine.h` owns the
pending-leg lifecycle, never sessions or index leases. A new ActivePeer is an
already authenticated/promoted peer (including a legacy peer without a Noise
session), not an unauthenticated pending connection. Pending machines are separate
and become active only after authorization, successful transport send and promotion
feedback. Authenticated events are trusted inputs, not raw ingress parsers.

The new module is linked into `fipsd_lib`; `main` still runs the bare process
skeleton until the node/transport subsystem lands. It does not invent peers,
read YAML, dial sockets or pretend that enqueueing a send completed a handshake.

### Ownership and snapshots

The transport **dispatcher fiber is the sole state writer**. Its pending machines,
durable retry entries, local identities, admission tickets and peer table are not
shared mutable worker objects. `dispatcher.h` supplies the runnable mailbox/deadline
loop for the future subsystem to spawn with `fipsd_runtime_spawn`. Its bounded
GoChannel transfers malloc-owned commands; callbacks execute effects and report
admission refusals explicitly. Stop producers on the GoContext; join producers and
dispatcher before destroying storage. Cancellation closes/drains the mailbox,
including rejected/not-yet-attached insertions. Before destroying the table, the
owner must close attached links and execute their release outputs. A live link or
outstanding pending ticket makes destruction refuse, not silently leak a lease.

The loop selects context cancellation, mailbox input and the **nearest stored
monotonic deadline** (`go_select` when none exists, otherwise
`go_select_timeout`). It services due deadlines even with queued commands. There
is no periodic peer poll, raw blocking wait, sleep or per-peer OS thread.
`FIPSD_PEER_POLL_LINK` asks the effect executor to call `link_step(Poll)` at the
shared machine's `next_wake`, with the executor's real Env. Result/effect callbacks
must not change table membership while a borrowed peer/action is being used.

Publish after each dispatcher mutation with `fipsd_peer_table_publish`; direct
ActivePeer consumers use `fipsd_active_peer_publish`. Publication failure is an
explicit error. Workers/control readers load immutable GoSwap/refptr snapshots,
release exactly once, and can retain old snapshots across removals. Snapshots
contain no keys, owning handles, MMP storage pointers or mutable session slots.
Free the slots only after all loaders quiesce; already loaded references survive.
Cipher copies are **per job/worker**, obtained by dispatcher `link_seal`, with the
separately reserved counter. Auto-seal on a clone refuses. Decrypt results return
through the owner, which authenticates/rechecks replay through machine inputs;
workers must not mutate the peer record or race its replay window. In particular,
`HEARD` and its byte/wall-time accounting are authenticated results, not evidence
that a datagram arrived from an unverified address. Heartbeat attempt and successful
send stamps remain separate; effects mark success only after actual send success.

### Rekey / session-table attachment seam

`src/peer/link.h` is **host-internal**, not the public monitoring surface. It
constructs the existing `fips_rekey` in place from an authenticated completed
session, consumes its index lease, drives `step`, reserves send jobs, observes
`view`/`next_wake`, and closes it. No external session-slot mutation or legacy
`replace_session`/K-bit setters exist. The machine is never copied or moved.
The injected profile must be FMP and Env transport must match the peer. Execute
**every** Bind/Unbind/Delegate/Revoke/Release/output in order, then clear the
output list on every path. Clear every reserved send job after use, even on failure.
The Env's profile/local-key/index/tag/route backing stores must outlive their use.
The node effect executor also applies record settlement on cutover/restart (MMP
reset, replay baseline, responder filter invalidation); `link_step` does not run
node policy behind the executor's back.

This is the named attachment point for the concurrently written **IK profile and
session table (`f12v2.2`)**. No replacement profile/session-table files are created
here. XK/FSP sessions belong to that separate node table, not to a link peer;
FSP quorum/ingress gates remain in libfips-core. An attached link may rebind its
send endpoint across transports or addresses **on the dispatcher only**. The
table checks exclusive pair ownership and transfers the admission ticket before
changing the endpoint; collision/budget refusal leaves both unchanged. Link
identity, Noise keys, counters and index-lease provenance do not move or restart.
Workers read owned snapshots, never these mutable records.

On a Bundle `Switched` output (`nostrc-9keoz.12`), `path.c` claims the new pair,
then `dataplane/peer_actions.c` releases the old path's MTU clamp, seeds from the
new transport/proven path MTU and publishes the new endpoint. It lowers MMP MTU
for every session whose **outgoing next hop** is that peer, using the production
routing decision (not the inbound carrier); session MTUs never rise merely
because a path switched. A refused claim/publication closes the link rather than
leaving the Bundle and send endpoint inconsistent. Active-path `Proven` outputs
reseed too; standby proofs wait for activation, and connection-oriented transports
use the transport MTU.

This mirrors upstream `Run::switched`'s clear-old-UDP → claim → seed-MTU →
tighten-sessions sequence. UDP now closes/joins the old connected socket before
the endpoint claim and primes the new authenticated endpoint during reseeding.
The other send state is the immutable endpoint snapshot, destination clamp and
captured crypto job endpoint. A completion resolves ordinary traffic to the
current active pair and checks its MTU, so jobs sealed before the switch cannot
send to the old pair. Explicit path probes/acks/closes still use their named
path, but completions for a path no longer held are discarded. The Bundle owns
hold/margin/dwell and return selection; the host does not force a switch back.

Path ranking reads each transport's synchronized detected-medium snapshot, not
an OS query on the send path. The unchanged core prefers wired, then wireless,
then BLE among known media within **1.3 times the best score**, after **2 seconds**
of dwell and at least **2 samples**; a wireless path better by more than that
margin wins. Unknown media retain the C transport-kind defaults (Ethernet wired,
BLE ble, otherwise unknown). Binder presence and medium-change notifications
rerank through the dispatcher; disappearing bindings release their paths and
returning ones can be proved again. A lost medium hint is reranked with its kind
fallback, without bypassing the core's ordinary score hysteresis. No new timer
or polling loop is used. This follows upstream `68ad7164`'s detected-medium rule;
the retained unknown-Ethernet kind fallback is the C compatibility exception.

### Bounds and admission

The table owns a sorted node-address index and a bounded linear `(transport,
opaque address bytes)` lookup; both reject duplicates. Address MAC display hints
are not part of equality. Its explicit capacity bounds storage even when policy
caps are zero (unlimited). No live-peer eviction occurs on capacity failure.
Admission independently checks total and per-transport peers/links/connections;
reserve a linear ticket **before** outbound connect (at most table-capacity
outstanding tickets even with unlimited policy), keep it through handshake,
then use `insert_reserved` to atomically promote it into a peer/link charge.
Failure preserves the reservation; release it on every abandoned leg. Unknown
transports still pay the total cap and bounded transport-slot cost; empty
unconfigured slots are reclaimed across churn. Rebind preflights collision and
budget limits before changing either lookup key. Tickets/owners are dispatcher-only.

An ancestry update is capped at **64 entries** by the host (not by the core wire
format) and rejects oversized input unchanged. Noise remains bounded by the core
machine. Local `identities.h` owns multiple opaque keypairs, rejecting duplicate
addresses/full storage without consuming the caller's key. Remove keys only once
all local-key borrowers quiesce. It also supplies a separate bounded public-key
prefix cache. Upstream `node/identities.rs` currently implements this cache/feed,
not a local-key set; the future node/TUN owner attaches its feed. Likewise upstream
`node/peer_budget.rs` is a per-authenticated-link token budget, separate from
admission caps: `budget.h` supplies thousandth-token accounting (20/s, burst 50 for
routing errors; earned coordinate-hint budgets can start empty). Its caller must
retain an exhausted bucket across reconnects or until refill, rather than giving
a reconnecting identity fresh tokens. No unbounded peer-keyed map is added.

### Mirrored schedules and configuration contract

These are the fields the config/node owner must translate, not a new YAML loader:

| Expected input | Peer API / default |
| --- | --- |
| `peers[]`: identity/npub, transport, addresses, auto-connect, auto-reconnect | Validate identity with core APIs, resolve transport addresses outside fibers via the blocking executor as needed, create pending machines; do not dial from the parser. |
| `node.retry.max_retries`, `base_interval_secs`, `max_backoff_secs` | `fipsd_peer_retry_policy`: milliseconds; upstream defaults **5, 5 s, 300 s**. Zero max retries suppresses handshake-failure retries, not enabled link-loss reconnects. |
| handshake retransmit interval/backoff/max resends, timeout | `fipsd_peer_timing`: **1 s / 2.0 / 5 / 30 s**. Successful resends at 1, 3, 7, 15 s; timeout at 30 s wins over a coincident/late resend (next would be 31 s). Failed sends do not advance resend count. |
| heartbeat interval, closed cooldown | `fipsd_peer_timing`: **10 s / 5 s**. Closed is terminal; the reconciler creates a new connection, not a self-redial. The cooldown stamp is diagnostic/eligibility input, not a second retry multiplier. |
| `node.limits.max_peers`, `max_links`, `max_connections` | `fipsd_peer_caps` total plus per-transport overrides. Zero disables a policy limb, never the explicit table/transport storage bound. |
| host table, identity-cache/local-identity and mailbox capacities | Required finite allocation bounds; no silently unbounded channels/maps. |

Durable retry entries outlive individual connections: first handshake failure is
count 1 → **10, 20, 40, 80, 160, 300, 300… seconds**; a first enabled link-loss
reconnect is count 0 → **5 seconds**, then increments the retained count. Configured
reconnects are unlimited; transient entries obey max retries/absolute expiry.
Connected and suspended/draining gates cancel schedules. A taken retry parks until
the handshake window closes, avoiding repeated dials. The future reconciler must
retain the upstream **16 attempts per reconcile** ceiling across entries.
The FMP silent-session guard is a separate core policy: third distinct silent
setup → **30, 60, 120, 240, 480, 600… seconds**, with authenticated startup epoch
changes bypassing stale-incarnation cooldown, and authenticated heard evidence
clearing the record. Restart requires two known, differing epochs; a missing
epoch is not a restart. Crossing/own-dial decisions delegate to FMP's existing
cores, including the node-address tie-break, not a new host heuristic.

**Restart admission (H1, `nostrc-f66sr.7`).** FMP classifies any differing known
epoch as a restart, and IK authenticates the recorded identity but not freshness,
so a captured earlier-epoch msg1 would otherwise evict the live link. The wire
format and the core decision are unchanged; the host no longer acts on the
decision alone. A restart msg1 from a live peer becomes a **held candidate** in
`handshake.c`: its own receive index (never bound to the index map), msg2
answered, exact retransmits re-answered from the hold wherever they arrive, one
candidate per peer, `max_pending_inbound` node-wide, released at
`handshake_timeout_ms` (the same knob that bounds the initiator's identical-msg1
retransmits). Frames at the candidate index on the candidate's transport are
opened by the ordinary workers under a clone of the candidate's keys; **only a
frame that authenticates under those keys closes the incumbent**
(`hs_restart_result`), because it proves the sender holds the msg1 ephemeral and
processed our fresh msg2 — a recording cannot. The proving frame becomes the new
link's first authenticated input, and its arrival address is judged by the
ordinary authenticated endpoint policy after promotion, not refused beforehand.
A bounded 64-entry memory of retired epochs (proven left behind) and expired
candidate ephemerals (refused for one further hold window, then challenged
afresh) refuses known-stale msg1 cheaply; it is supplementary and never a
freshness proof. A dropped incumbent with a candidate pending is not remembered,
so the peer's identical retransmit establishes through the ordinary new-peer path.

### Verification

`FipsdPeerTest` (in `fipsd_tests`) ports representative lifecycle/reconciler,
ActivePeer, budget and identity-cache contracts: retry/cooldown boundaries,
resend success/failure, restart epoch matrix, both crossing outcomes/own-dial,
late authorization and loss, cap rejection/atomic promotion, table bounds and
rebind collisions, local-key ownership, tree/filter state, real AEAD/replay and
linear lease release through the rekey seam, and concurrent immutable snapshots
while the real runtime dispatcher processes 500 channel commands. It reuses
`libfips-core/tests/fakes/host.c`; all clock advances are injected, no sleeps.
Runtime, peer and node threading tests use two workers, including TSan. This is host-module proof, not a network/interoperability
claim and not a port of upstream's multipath/settlement/node test suites.

## Node lifecycle (`nostrc-9keoz.5`)

```
main: load YAML → GoSwap config → runtime start → config reload service
                                             → node lifecycle actor

actor start (fiber):
  config snapshot → identity/epoch → sealed transport registry
    → transport start_async (creation order, ABSENT is success)
    → netmon → dispatcher → N crypto workers → publish substrate health

actor stop (root cancellation or subsystem stop):
  close worker inputs → dispatcher drains accepted results to EOF
    → stop/join dispatchers → stop netmon → stop transports (reverse order)
    → release identity/config after runtime join

runtime: all tracked fibers, including binder/monitor/worker fibers,
         must finish within its one shared shutdown grace (default 5 s)
```

`node/lifecycle.{c,h}` separates a pure event/action supervisor from its
channel-driven fiber driver. Config/identity/registry/netmon/dispatcher failures
are required-substrate failures; worker-pool short starts are optional, retained
and degraded. Zero transports up is **Failed**, not ready. One or more up with
a failed/absent configured child is **Degraded** (substrate-ready); otherwise
**Ready**. A transport whose interface is absent stays up. Present clears absence
and can restore Ready, but does not clear a startup/task failure. Duplicate,
unknown and late events are inert. An exited child is never automatically
restarted; binder tasks own rebind. Runtime loss of the last transport publishes
Failed rather than forcing teardown. The reducer includes bounded-drain/gating
states for later peer handlers; the production actor currently uses immediate
reverse teardown, not an unimplemented Disconnect/drain protocol.

**Bootstrap policy:** until Tier B2 registers real transports, `main` continues
serving the config/control host with a visible Failed, **not-ready** node and a
warning. This intentionally differs from upstream's fatal zero-transport startup;
there is no fabricated loopback/default transport or claim of network readiness.
Other startup health failures remain visible through the same status seam.
`fipsd running` describes the foreground host, not node/network readiness.

`node/context.{c,h}` is the borrowed per-node handle: configuration references,
read-only identity/epoch, immutable network/peer snapshots, and the peer-command
mailbox. Mutable peer/table/session state belongs only to the peer dispatcher;
other actors send commands, never share mutable peers. Crypto completion handlers
run on the node event dispatcher and must return acceptance commands to that
owner. The protocol handlers attach to that dispatcher (see below).
Explicit `node.identity.nsec` is decoded by the core; otherwise a secure ephemeral
identity is generated off-scheduler. A requested persistent identity without a
key is refused visibly (Failed), never silently replaced by an ephemeral key;
filesystem key persistence is not implemented by this task.

`node/worker_set.{c,h}` has a bounded input channel per worker and a bounded
completion channel, round-robin nonblocking distribution and explicit ownership
on refusal. Each owned job has its own cipher state, input/AD/output buffers,
reserved counter and stale-result token. SEAL calls only
`fips_cipher_state_seal_with_counter`; OPEN authenticates without replay mutation.
The dispatcher still owns epoch validation and replay recheck/mark on acceptance.
Every key and owned buffer is cleared on completion/discard. Partial spawn is
reported without restart. Shutdown closes inputs but drains already accepted jobs
even after root cancellation; the dispatcher stays alive until completion EOF.
An unhandled/refused callback cancels the runtime but keeps draining, so a full
completion queue cannot deadlock shutdown.

`node/transport.{c,h}` is the host-internal Tier B2 registration contract: stable
id/type/name, start_async/stop/state, per-link or default MTU, send/discover,
connect_async/close_connection and network-change notification, plus auto-connect
and inbound-accept policy. Instances register before the registry seals; duplicate
ids/names, late registration and incomplete required vtables are refused. Methods
run on fibers; start_async launches runtime-tracked binder/receiver tasks, returns
success while absent and emits owned channel events. Network notifications wake
the binder, which owns descriptors and detach/rebind. Lifecycle needs no
transport-specific branches. No real network transport is implemented here.

### Network monitor

`node/netmon.{c,h}` opens Linux NETLINK_ROUTE with link/IPv4-address/IPv6-address
groups, or macOS/FreeBSD PF_ROUTE. A tracked kernel fiber waits with a bounded
`gof_recvfrom(MSG_PEEK)` (gof_read has no cancellation API), then consumes with
`gof_read`. The wait deadline checks cancellation only; it does not close a
subscription or schedule sampling. Wakes coalesce on a capacity-one channel.
The monitor samples on a kernel wake or the **one-second Ticker** backstop,
using `go_blocking_submit(getifaddrs)` so system enumeration and ticker-thread
join never block a scheduler worker. Socket-open/stream failures retain the
backstop. This is an interface/prefix monitor, **not** the newer upstream
peer-route fingerprint/connected-UDP repair implementation.

Presence is `IFF_UP`, never `IFF_RUNNING`. Snapshot comparison emits interface
up/down and address-change events; disappearance also invalidates addresses.
Enumeration/capacity/publication failures never become false interface-down
edges. The last good snapshot is retained on read failure with an explicit
failure event; callers must not treat retained prefixes as fresh authorization.
`fipsd_onlink_read_system` includes loopback/down interfaces and address prefixes
in its raw snapshot (including lo0/en0); its core `fips_onlink_*` view excludes
loopback and down interfaces, matching upstream read_system policy. BSD embedded
IPv6 link-local zones are normalized to interface indexes. Prefix storage belongs
to the retained snapshot and cannot be copied without rebuilding its view.

Tests in the `fipsd_tests` aggregate cover the supervisor escalation/recovery and
no-restart table, ordered startup/reverse teardown, real node/context/transport
registration, required-identity failure, shutdown after a refused handler with
64 queued crypto jobs, two-worker explicit-counter seal/open/authentication failure
and cancellation drain, injected monitor snapshots/wakes/backstop, and native
kernel-socket opening plus an actual Ticker event. The native macOS/FreeBSD test
cross-checks every `ifconfig -a` IP/prefix against read_system. Injected tests use
channels and a manually driven backstop, not sleeps or timing-based completion.

## Node protocol handlers (`nostrc-9keoz.6`)

`src/node/handlers/` drives libfips-core's sans-IO machines from the node's
**single dispatcher fiber**. `node/context.c`'s event loop now also performs the
peer dispatcher's duties (`peer/dispatcher.c` exposes `wait_ms`/`command`/
`service` for it): one `go_select` multiplexes transport events, netmon events,
crypto results, the peer-command mailbox and the handlers' command mailbox with
the nearest stored deadline. The handlers hook in through the `fipsd_node_options`
seam (`attach`/`detach`/`wait_ms`/`poll`/`peer_effects`/`command`); `attach` runs
in `dispatcher_start` before the fiber exists, so a started node always has a
published snapshot, and `detach` runs on the fiber after the last event so links
and sessions close with their Release outputs executed before the table is freed.

| Upstream (`src/node/handlers`, `src/node`) | C file | Lines | Scope |
| --- | --- | ---: | --- |
| `handshake.rs` (2654), lifecycle dial/msg1 | `handshake.c` | 555 | Pending legs (`fipsd_peer_machine` + IK handshake owner), msg1 rate limit → `fips_fmp_rekey_read_msg1` → `fips_fmp_establish_inbound`, msg2 → `fips_fmp_establish_outbound`, promote/restart/replace/cross-connect, Disconnect, configured peers and durable retries |
| `rekey.rs` (1098) | `rekey.c` | 354 | One `fips_session_executor` for link and session machines: Send/Prompt → wire, Bind/Unbind → receive-index map, Release → allocator, Installed/PeerRestarted settlement, deferred Deliver, link seal/send, encrypted frame ingress, link-message dispatch |
| `session.rs` (3556) | `session.c` | 453 | `fips_session_table` (XK), strict FSP ingress, setup admission by the authenticated carrier, Ack/Msg3/Frame via `fips_fsp_rekey_receive`, pending/held datagrams, `fips_router_route` for origination and transit, routing signals, idle purge |
| `tree.rs` (909) | `tree.c` | 181 | Signed announces, `fips_stp_receive_announce`/`periodic`/`remove_peer`, action execution (sign, announce, cache invalidation, backoff reset, filter refresh), per-peer announce interval, root-disagreement echo |
| `bloom.rs` (489) | `bloom.c` | 110 | `fips_bloom_state` receive/refresh/sent, roles from the tree, delivery evidence from receiver reports |
| `lookup.rs` (1151) | `lookup.c` | 214 | Request classification, signed target responses, transit forwarding, response verification, coordinate cache writes, retry ladder, held-datagram retry |
| `mmp.rs` (944) + session MMP | `mmp.c` | 223 | Link sender/receiver reports and logs, SRTT-driven intervals, dead-link detection, session reports and path-MTU notifications |
| `path.rs`/`path_intake.rs`/`path_control.rs` (2026) | `path.c` | 202 | One `fips_path_bundle` per peer: facts vtable from the registry, probe/ack/close inputs, encoded sends on the named path, endpoint switches through the table, `path_show`/`path_pin`/`path_unpin` |
| `probe.rs` (546) | `probe.c` | 187 | Bounded diagnostic job registry over `fips_probe`, observations, actions, control rendering |
| `timeout.rs` (766) + rx_loop tick | `timeout.c` | 79 | Timer wheel: nearest deadline over legs, sessions, paths and the maintenance tick; tick duties (tree, bloom, lookup, MMP, sessions, probes, caches, limiters, stats history) |
| `native.rs`/`host_link.rs` (673) | `native.c` | 18 | Seams for B4: inbound datagrams become `FIPSD_NOTICE_DATAGRAM`; outbound is `fipsd_node_handlers_send` |
| `netmon.rs` (228) | `netmon.c` | 18 | Network change → path presence seam, immediate heartbeats |
| control `snapshot.rs`/`queries.rs` rows | `provider.c` | 540 | Immutable snapshot arena (typed rows + JSON projections with upstream field names) published via GoSwap; live commands enqueued to the dispatcher |
| `mod.rs`/`reject.rs`/`diag.rs` plumbing | `handlers.c`, `internal.h` | 864 | Creation, storage, attach/detach, event entry points, notices, command mailbox, counters |

Inbound path: transport `PACKET` → `fips_fmp_receive` gate → msg1/msg2 to
`handshake.c`, established frames to the receive-index map → `link_step(Frame)`
→ executor → deferred delivery → link-message dispatch by type byte
(SessionDatagram, Sender/ReceiverReport, TreeAnnounce, FilterAnnounce,
Lookup*, Disconnect, Heartbeat, Path*). Session datagrams for us go through
`fips_fsp_receive` to the session machine; others through the router. Outbound:
every link message reserves its counter with `fipsd_active_peer_link_seal`,
queues an explicit-counter worker seal, then sends and accounts `DataSent` on
successful completion.
Packet AEAD now runs in the dataplane worker pool (`9keoz.7`), including
executor prompts; Noise handshakes remain on the control-plane dispatcher. Each machine's output list is executed
to completion before any nested step; deliveries are handed on afterwards.

Deliberate policy notes: the receiver-index map is host state fed by Bind/
Unbind; a link closed through `fipsd_active_peer_link_close` has its Release
outputs executed by `hs_release_outputs` (no machine is required); a retransmitted
IK msg1 is answered from the link's setup record rather than re-established;
STP announces stay outstanding until accepted MMP receiver reports confirm
delivery, with upstream loss/unverified retry budgets and backoff; interface
UP/DOWN maps bound transport names to Bundle presence (carrier is diagnostic,
binders own descriptors); the bloom send floor is the refresh pass counter.

### Loopback transport and the two-node proof

`FipsdNodePathPresenceTest` uses two real loopback transports and injected
interface events. It checks the peer table and published dataplane endpoint,
releases a retained pre-switch crypto completion (+0 frames on transport 1,
+1 on transport 2), and delivers eight application datagrams in each of two
X-absent intervals: transport 1 must send zero frames while transport 2 sends
at least eight. A narrower second transport tightens the established session;
an injected stale 900-byte clamp is replaced by the new 1100-byte path seed.
When X returns it is re-proved through real probe/ack traffic and transport 2
remains active under the Bundle's selection rules; application traffic is
rechecked. All observations run on the dispatcher or use atomic counters/owned
snapshots and channel synchronization, with no sleeps or host-interface changes.
A 900-byte proof is then raised by a real 1000-byte discovery frame/ack; probes
use the named transport's ceiling, not the current proof, while ordinary traffic
uses `fips_path_carried`'s proven-size bound. The established session stays at
900 until its own destination notifications permit an increase.

`transport/loopback.{c,h}` (with the old `node/transport_loopback.h` include retained) is an in-process datagram medium behind the
`fipsd_transport` seam: endpoints deliver owned `PACKET` events to each other's
node channel with `go_channel_send_with_context` against the destination's root
context, plus an optional drop filter. `main.c` does not register it.

`FipsdNodeTwoNodesTest` (`tests/test_node_two_nodes.c`): two nodes with seeded
identities → `connect` → IK link established both ways → spanning tree converges
(one root, depth 0/1, consistent `show_tree` on both) → filters exchanged → an
application datagram opens an XK session and is delivered end to end, then the
reverse direction → one session rekey and one link rekey complete (K bit true on
both, traffic still flows) → control queries (`show_peers/sessions/links/mmp/
routing/status/cache/bloom/tree`) read the live state → `disconnect` drops the
link on both ends. Every assertion is a notice (`fipsd_node_handlers_notices`) or
a published snapshot; waits are bounded channel selects, no sleeps. The proof
lowers the rekey `after`/floor parameters (keeping the link floor above the
machine's 5 s `young` window) through the handler configuration, not YAML.

`FipsdNodeHandlersTest` (`tests/test_node_handlers.c`) adapts the upstream
handler tests (`mmp.rs` silence/heartbeat pacing, `session.rs` held-packet fate,
`timeout.rs` stranded release) and C-host contracts: single-node control seeds,
msg2 loss with msg1 retransmission (notices plus quiescent progress counters prove the receive/promotion totals),
dead-link detection on authenticated silence, pending-connection projection and
handshake timeout, held datagram release without a route, and the live
`probe_start/poll`, `path_show` and `disconnect` commands through the provider.
Both run on two scheduler workers under ASan/UBSan and TSan.

### Snapshot dataplane (`nostrc-9keoz.7`)

The production seams `hs_link_send*`, `hs_link_frame`, executor Prompt and the
FSP encrypted datagram path use `node/dataplane/`. There is **no inline packet
crypto fallback**. The core's default synchronous executor remains available to
other core callers/tests, not to the installed daemon handlers. `rx_loop.c`
classifies transport events; handshake/control work is handed to the dispatcher,
while established frames become bounded authentication jobs. `encrypted.c`
assembles owned jobs, `dispatch.c` validates completions, `peer_actions.c`
performs authenticated heard/MMP/path/message effects, `forwarding.c` resolves
next hops over immutable tables and invokes `fips_router_route`; `reject.c`
keeps typed silent rejects and rate-limited routing/MTU signals. TTL/local
delivery ordering, CE propagation and error synthesis remain core decisions.
`path_mtu.c` owns the bounded destination clamp map. MMP notifications/reactive
errors update it; session close, probe-owned teardown, peer removal and restart
release it (including the upstream 2026-11-12 release paths). Native adapter
publication/MSS application still belongs to its future host owner.

**Ownership and replay:** the dispatcher is the only mutable peer/session,
send-counter and replay owner. `fips_rekey_seal`/`*_seal_prompt` reserve through
`take_send_counter` and return bare send-key clones; refused queues burn the
reservation, never roll it back. `encrypt_worker.c` seals only at that explicit
counter. `decrypt_worker.c` opens with an independent bare receive-key clone at
the wire counter, without any replay state. Completion revalidates the live
peer, tag AND key generation (tag zero can recur on a replacement machine).
`FIPS_REKEY_IN_AuthenticatedFrame` rechecks replay on the dispatcher and uses
the existing mark/cutover/deliver path for current, pending and previous epochs.
`env.workers` deliberately remains false: no Delegate window/authority handoff
is used. FSP trial retries own another slab, never resubmit a borrowed result.
A queued Disconnect may outlive table removal; it retains its original endpoint
and snapshot. Handshake crypto is control-plane work and is not offloaded.

**Snapshots:** the dispatcher publishes an entirely owned routing/peer/session
table through `GoSwap` after control-plane invalidation changes its topology/key
fingerprint (not a whole-table scan on every packet), sharing
unchanged owned coordinate/filter objects between generations. Tables contain
transport endpoints/MTUs, bloom filters, costs, coordinates, cache expiry and
bare current/pending/previous epoch keys, never mutable peer pointers/windows.
Jobs pin a generation with atomic `GoRefPtr` retains. Packet readers take no
table locks. This is NOT a claim that GoSwap is lock-free: its publication and
load-and-retain use an internal short mutex, confined here to publication of a
changed generation; no worker calls `go_swap_load` per packet.

**Bounds:** 256 reusable packet slabs per handler set, each with independent
2048-byte AD/input/output storage; no per-packet heap allocation is introduced
by dataplane or worker jobs. Existing transport events/control snapshots/notices
retain their pre-existing allocation behavior. Per-worker input and shared
completion queues remain bounded by node options. Submission tries all live
lanes before refusing; there is no inline fallback, unbounded backlog, timer or
retry loop. Saturated FSP trials may drop with backpressure because the original
borrowed completion cannot be recycled until the callback returns. All keys and
packet buffers are wiped on recycle; snapshot keys are wiped on last release.
Shutdown closes worker input, drains accepted results through dispatcher EOF,
then detaches handlers, joins and frees slab/table storage.

`FipsdDataplaneTest` covers direct/excluded/TTL/local/CE forwarding, MTU folding,
shared routing-error rate limits, clamp release, four-worker unique counter
reservations, queue/slab refusal, trial saturation, replay-free worker auth and
delayed transport consumption after caller-buffer overwrite. Core rekey tests
exercise duplicate/stale completions, pending cutover and previous-epoch
acceptance. The two-node proof reports actual per-worker seal/open counters and
a replay-mark observer compares the executing fiber with the dispatcher owner;
zero off-dispatcher writes is asserted after runtime join.

Throughput smoke (not a qualification or default ctest workload):

```sh
# Build fipsd_tests; run directly, optional datagram count (default 10000).
build/fipsd/tests/fipsd_dataplane_throughput 10000
# Or configure -DFIPSD_DATAPLANE_BENCH_TEST=ON, then ctest -L bench -V.
```

The optional registered test is labelled `fips;bench`; ordinary configurations
leave it unregistered. It sends 512-byte datagrams A→B in bounded batches,
checks every sequence exactly once and reports payload pps/decimal MB/s with
four scheduler workers and two crypto workers per node.

Recorded for `nostrc-f66sr.4` (local macOS arm64, warnings-as-errors, 10000 ×
512-byte payloads, no loss/retry): Release smoke **818 ms, 12225 pps,
6.259 MB/s**; ASan/UBSan smoke **5401 ms, 1852 pps, 0.948 MB/s**. These are
single loopback smoke samples including host/notice overhead, not network
throughput or a Rust/C performance comparison. The separate sanitizer gates
run `ctest -R '^Fips'` (117 registered tests: 116 passes, optional Rust-control
interop unavailable/skipped); the benchmark is not included in that default.

### Remaining for Tier B2 and B4

Tier B2: TCP and the other concrete transports, discovery and the full C/Rust
interoperability/performance gate. UDP's real-socket two-node proof and optional
Rust link smoke are described below; neither is deployment/performance proof.
Shared interface presence/binding and UDP's concrete callbacks are implemented.
B4: native API and IPv6 adapter owners replacing
`native.c`/`host_link` notices, clamp publication/MSS application to the adapter,
configured-peer address lists into path intake and rendezvous. Admission ACLs
are active at the existing dial/handshake seam; they do not claim these remaining
network or host adapters are implemented.

## Unix control socket (`nostrc-9keoz.3`)

The control subsystem is registered after configuration only when
`node.control.enabled` is true. It honours `node.control.socket_path`;
without an override, resolution is `/run/fips/control.sock` when its private
directory exists, then `/var/run/fips/control.sock` on macOS/FreeBSD when its
directory exists (a privileged macOS daemon also selects/creates it), then
`$XDG_RUNTIME_DIR/fips/control.sock`, then `/tmp/fips-control.sock`.
Configuration resolves its default against an existing XDG runtime directory.
Startup creates missing private parents, uses the existing privilege helpers
for private-parent `fips` 0750 and socket `fips` 0770, and never changes an
existing shared/custom parent's ownership/mode. When the group is absent it
logs a warning and retains the effective group, still enforcing both modes.
A present but unassignable group is a startup error, not a silent fallback.

The accept loop and each connection run as tracked fibers. One JSON request
line in, one response line out, close; subsequent requests are not executed.
The newline counts toward the **4096-byte** bound. Oversize input is refused
as `read error: request too large` without an unbounded read allocation.
Read and write each have a 5-second protocol deadline, and dispatch has a
separate 5-second execution budget; a companion fiber
selects phase completion against the root context and deadline and shuts down
(but never closes/reuses) the I/O owner's descriptor. `gof_accept`, `gof_read`
and `gof_write` park cooperatively; Linux sends use `gof_sendmsg(MSG_NOSIGNAL)`
because it has no `SO_NOSIGPIPE`. Filesystem/group operations run on startup's
thread, not a scheduler worker. Shutdown replaces the listening descriptor with a preallocated non-listening
socket (keeping the fd slot reserved, independent of its pathname) and explicitly
wakes the accept fiber; descriptor replacement alone does not signal the netpoller.
It joins all tracked fibers and unlinks only the inode it bound. Existing live
sockets are refused. **Safety difference from Rust:** stale cleanup refuses
non-sockets, symlinks and other users' sockets rather than deleting arbitrary
files at the configured path.

`protocol.{c,h}` uses **jansson** and emits the exact consumer envelope:
`{"status":"ok","data":...}` or `{"status":"error","message":...}`.
The Response struct's outer order is `status`, then `data`/`message`; nested
maps are lexical, like upstream `serde_json::Value` without `preserve_order`.
Malformed-input details are Jansson diagnostics, not invented serde text;
the common error prefix and all command/required-field errors match upstream.
As the actual Rust `serve` does, a complete JSON value followed by EOF is
accepted even without the documented newline. Unknown top-level fields are
ignored. Profiling-only, gateway and IPv6-adapter commands are not daemon
commands in this default build. In particular **`reload`, `probe`,
`show_filters` and `show_stats` are not upstream command names**; YAML reload
remains SIGHUP, diagnostics use `probe_start/poll/cancel`, filters use
`show_bloom`, and history uses the `show_stats_*` family.

### Command/provider boundary

`control/snapshot.h` defines `fipsd_control_provider`: a synchronized retained
load of an immutable `fipsd_control_snapshot`, plus a live-command method.
Snapshots may be published through GoSwap; their GoRefPtr is held through
serialization. Typed status/routing/cache/connection/transport/native/history
fields are rendered here. Conditional peer/session/link/MMP/tree/bloom/ACL
rows and counter extensions are immutable JSON-valued **data projections**
owned by the provider, not envelopes; they are deep-copied, never mutated or
stolen. This intentionally keeps node dispatchers, handlers, lifecycle and
dataplane out of this subsystem. Live-command providers receive the root context and an absolute monotonic
execution deadline. They must enqueue to the single writer, select replies
against cancellation/deadline and offload blocking work;
they must not mutate a node from the connection fiber.

The node handlers install the provider once the node attaches (`main.c`);
before that, or when the node fails to start (no identity), the empty seed
views below remain the answer.

| Commands | Implementation | Before the node attaches |
| --- | --- | --- |
| `show_status` | Identity, lifecycle/counts, forwarding, history sparklines | Error `node not yet initialized`; no invented identity or mesh estimate |
| `show_acl`, `show_tree`, `show_bloom` | Snapshot projections plus counter families | Upstream pre-first-tick seed: empty ACL strings/tables, zero tree addresses, null root npub/uptree estimates, no root claim |
| `show_peers`, `show_sessions`, `show_links`, `show_mmp` | Immutable entity projections | Empty entity tables |
| `show_cache`, `show_routing`, `show_identity_cache` | Cache/lookup rows, counts, counters, saturating ages | Empty/zero seed views (not configured live caches) |
| `show_connections`, `show_transports`, `show_native_flows` | Typed rows; optional fields preserved; transport interface presence is separate from transport state | Empty tables |
| `show_stats_list` | Actual libfips-core metric metadata and ring constants | Fully available |
| `show_stats_history`, `show_stats_all_history`, `show_stats_peers`, `show_stats_history_all_peers` | Metric/window/granularity/peer validation, actual history queries, retained peer metadata and display ordering | Empty rings, null-padded history; per-peer query refuses an untracked peer |
| `show_metrics` | All registry families, including discovery/lookup alias | Error `subsystem not started` until dataplane/handshake/session families are supplied |
| `connect`, `disconnect`, `path_show`, `path_pin`, `path_unpin`, `probe_start`, `probe_poll`, `probe_cancel` | Required-parameter validation and live provider dispatch; `probe.{c,h}` renders stage reports | Error `subsystem not started` after validation; never claims a mutation succeeded |

These are **20 read queries and 8 live commands**, pinned to upstream
`6aa80504baa0fa312b392009eea0e3e59fb6fe4c`. The C-host pre-node initialization
errors are explicit availability refusals in the upstream envelope, not a
claim that upstream constructs a node without an identity. Once a node owner
supplies a real identity, even its `created` state is reportable. History uses
`fips_stats_series_walk`: **all NaN and +/-infinity values become JSON null**.

### Control verification

`FipsdControlTest` links the production library: independently constructed
provider state (including the upstream `[0xAB;32]` identity), all 24 upstream
fixtures, interface presence, optional probe fields, request limits, malformed
JSON, missing params, unknown commands, EOF, one-request-only connections,
24 simultaneous clients while another client is incomplete, the real read
deadline, resolution precedence and socket/private-parent modes. Group checks
are explicitly skipped only when the `fips` group does not exist.
See `tests/control_golden/README.md` for fixture classification and byte-level
canonicalization. `FipsdControlInteropTest` runs the real daemon, checks disabled
control, and drives it with the pinned Rust client (`fipsctl show status`,
`show peers`, `show transports`, `stats list`). Status is deliberately an
initialization error until the node owner exists; the other three succeed.

Enable the optional oracle with
`-DFIPSD_UPSTREAM_CONTROL_DIR=/absolute/read-only/fips`, or supply
`-DFIPSD_FIPSCTL_EXECUTABLE=/absolute/path/to/pinned/fipsctl`. The test builds
with `cargo --locked`, writing only to the CMake binary directory's
`fipsd/tests/cargo-control`. Missing cargo or an unconfigured upstream checkout
is reported as CTest **skipped**, not passed. Both control tests belong to the
`fipsd_tests` aggregate. The control concurrency test also covers concurrent
main-thread/accept-fiber spawns under TSan: libgo's pthread_once initializer
uses a caller-local stack-size argument, rather than racing concurrent writes
to a global before pthread_once. Multi-worker testing is not reduced.

Jansson has signed 64-bit integers: an unsigned snapshot counter above
`INT64_MAX` is explicitly refused as `value exceeds JSON integer range`, never
wrapped negative or rounded through a double. Failed JSON construction is
discarded as `response construction failed` (or the connection closes if even
the error cannot be allocated), not sent as a partial success. Fault-injection
and signed-boundary tests cover these paths. Shutdown tests remove and replace
the live socket pathname while accept is parked.

Version decision: additive work and the PATCH-class control shutdown correction
fold into **fipsd 0.1.0 Unreleased**; no manifest, declared version or release/tag
change. The required libgo race correction remains in its unreleased 0.3.0
version (PATCH-class).

## Shared transport layer (`nostrc-gfi8h.1`, Tier B2)

The existing `node/transport.h` vtable, event channel and `interface_name`
contract are unchanged. Its append-only `shared` pointer is optional for legacy
or test drivers. Embed `fipsd_transport_shared`, initialize before registration,
and keep it and immutable metadata alive through runtime join. There is no
implicit UDP listener or new registration in `main.c`.

| Upstream `src/transport/` | C host `src/transport/` | Responsibility |
| --- | --- | --- |
| `mod.rs` | `transport.{c,h}` | Shared instance state, owned event copies, bounded link table, per-link stats/MTU, generation-safe connection replacement |
| `framing.rs` | `framing.{c,h}` | FMP prefix sizing and datagram envelope validation |
| `stream.rs`, `framing.rs` | `stream.{c,h}`, shared link table | Incremental framing, fiber read/write, unique connection IDs and remove-own guard |
| `medium.rs` | `medium.{c,h}` | macOS SIOCGIFMEDIA; Linux flags/wireless/ethtool and conservative sysfs classification |
| `presence.rs` | `presence.{c,h}`, `watcher.c` | IFF_UP, separate carrier, episode age, attempts, binds, retry/churn damping |
| `watcher.rs`, Ethernet binder in `mod.rs` | `watcher.{c,h}` + existing `node/netmon.c` | Coalesced kernel/netmon notifications, one-second backstop, sole binder ownership |
| `sockbuf.rs` | `sockbuf.{c,h}` | Linux force/plain fallback, readback, clamp advice; no sysctl mutation |
| `stats_common.rs` | `stats_common.{c,h}` | Atomic packet/byte counters and connection-pool occupancy |
| `loopback.rs` / existing C loopback | `loopback.{c,h}` | Same two-node API and drop-filter behavior, shared state/events/stats |

### Wire formats

There is **no separate TCP/Tor length prefix**. As current upstream
`docs/reference/wire-formats.md` specifies, the FMP prefix is
`[version:4 | phase:4][flags:8][payload_len:16 little-endian]`.
Established frames are `16-byte header + payload_len + 16-byte AEAD tag`;
IK msg1 is 114 bytes (`payload_len=110`), msg2 is 69 (`payload_len=65`).
Stream receives use a 65535-byte allocation ceiling, not the local send MTU.
The incremental reader accepts partial prefixes/bodies and coalesced packets,
returns one complete frame at a time, and rejects invalid version/phase/size,
capacity exhaustion and truncated EOF. Stream prefix framing leaves flags and
ciphertext validation to the node. Datagram boundaries are preserved unchanged;
the validator rejects truncation, trailing bytes and malformed FMP envelopes.
No Ethernet data/beacon wrapper or BLE framing is implemented in this task.

### Binding and ownership

`Stopped -> Absent -> Binding -> Present`; absence at start is success.
A netmon event wakes a capacity-one mailbox. A one-second ticker is the backstop
and services retry/stability deadlines. The actor reloads an immutable netmon
snapshot (or injected facts); native getifaddrs/ioctl and ticker setup/teardown
run on the blocking executor. Kernel event sockets remain owned by netmon.
`IFF_UP`, not `IFF_RUNNING`, controls binding; carrier changes only diagnostics.
Invalid reads keep the last binding. Disappearance, index/MAC replacement or
current-generation receive-loop exit invokes unbind on the sole binder, clears
stale links, then waits for the resource to return. Late exits from an old
binding cannot tear down a replacement.

Absence races do not increment failures. Other bind failures back off
1/2/4/8/16/30 seconds. Bindings dying before ten seconds use the same damping;
after three consecutive young deaths, presence announcements stay held until
a binding survives ten seconds. Required sustained absence is logged once at
ten seconds; optional absence does not log that error. Bind/attempt counters
and episode age survive retries. Control rows publish `interface.carrier` and
`interface.medium` alongside presence, policy, binds and attempts.

The driver must unwind failed binds. Unbind must cancel/wake **and join** that
generation's receive/writer tasks before closing/reusing descriptors. Raw close
is not cancellation for parked `gof_read`/`gof_recv*`. Runtime-tracked tasks and
root/stop channel selects provide shutdown; no timeout is used as a readiness
signal. Stream drivers must set SO_NOSIGPIPE on macOS sockets and serialize
writers; Linux uses MSG_NOSIGNAL. The shared table supplies remove-own identity
checks, not a TCP pool, accept loop, queued writer or drain-task implementation.
Those concrete stream owners belong to their transport tasks.

Medium values are the existing core `fips_path_medium`; the core selector
already prefers wired over wireless within its multiplicative switch margin.
The shared accessor returns cached medium without I/O on the send path.
Unknown/virtual interfaces are not guessed to be wired. The existing node
`handlers/path.c` callback still hardcodes Ethernet as wired: replacing it with
`fipsd_transport_medium` is a small follow-up requiring approval beyond this
task's node-include/provider-only edit boundary. Detection, projection and the
core preference rule are tested; live detected-medium selection is not claimed.

### UDP (`gfi8h.2`)

| Module under `src/transport/udp/` | Owner |
|---|---|
| `udp.{c,h}`, `internal.h` | Instance/config registration, watcher binding, numeric peer pool, send leases and MTU |
| `io.{c,h}` | Socket options, interface binding, address conversion, checked pktinfo/ECN decoding and source cmsgs |
| `connected.c` | Tracked listener/peer receive drains and cancel/join/close ownership |
| `classify.{c,h}` | Before-start subsystem classifier registry; reserved Nostr punch prefixes never enter FMP |
| `dns.{c,h}` | Numeric fast path and bounded 256-entry, 60-second peer-hostname cache |

`main.c` registers singleton or named UDP instances **before** sealing the node
subsystem. `bind_addr` includes its port; each instance owns one IPv4 or IPv6
listener (IPv6-only; configure separate instances for both families). Existing
normalized `mtu`, buffer sizes, `interface`, `outbound_only` and
`accept_connections` settings drive the socket owner. Outbound-only binds an
ephemeral IPv4 port and refuses fresh inbound handshakes. UDP discovery itself
does not invent multicast peers; the node dials configured peer entries.

Socket setup/close and hostname `getaddrinfo` run on the blocking executor.
Hostname resolution here is configured-peer DNS, **not** the Tier B4 `.fips`
DNS service. Unlike upstream's held-datagram/stale-refresh optimization, a cold
hostname lookup awaits the executor result; numeric addresses never invoke DNS.
The shared sockbuf helper sizes both listener and peer sockets and reports
kernel clamping. Listener reuse options are set after bind: another listener
cannot silently join an occupied port, but authenticated connected sockets can.

Every receive uses indefinite `gof_recvmsg`, with packet and ancillary
truncation rejected. Linux uses `IP_PKTINFO`, BSD/macOS `IP_RECVDSTADDR` plus
`IP_RECVIF`, and IPv6 uses `IPV6_RECVPKTINFO`. ECN comes from IPv4 TOS or IPv6
traffic class with DSCP bits masked out. Linux's cumulative `SO_RXQ_OVFL`
counter is accumulated with wrapping deltas. `show_transports.stats` publishes
`pktinfo_recv`, `ecn_recv`, `ce_recv`, `kernel_drops`, `malformed`,
`classifier_drops` and `pool_full` alongside packet/byte totals. Darwin has no
kernel UDP queue-drop ancillary counter. Listener replies select the received
local destination via sendmsg control data; connected sends use the kernel's
pinned source/route. One datagram is one FMP frame, without a stream prefix.
Sends reject the configured MTU and report socket backpressure as failure.

Only the node's authenticated IK/AEAD callback admits a numeric endpoint to the
bounded connected pool (64 by default); parsing an envelope is not admission.
Duplicate admission is idempotent; a full pool keeps using the listener.
Connected sockets share the listener port, inherit its interface and buffers,
and have their own tracked receive drains with identical classifier validation.
Disconnect/path replacement removes the entry from lookup before cancellation,
joins outstanding send leases and the drain, then closes the descriptor.
Unexpected drain failure invalidates that binding generation, so a dead
connected tuple cannot retain a kernel receive black hole.

Absent-interface startup returns success. Netmon notifications and the shared
watcher's backstop drive appearance/disappearance/rebind; no receive polling or
timeout-close is used. `gof_io_cancel_fd` latches cancellation across the
register/park race, independently of routes or interface presence. The socket
owner joins all users, resets that latch, then closes. Raw close/shutdown is
never used to cancel a parked receive. The added libgo trace hook exposes the
armed/pre-park boundary for deterministic cancellation tests.

`FipsdUdpTest` covers crafted v4/v6 ancillary records and truncated headers,
real IPv4 CE marking, wildcard-listener reply source selection,
buffer readback, permitted native loopback-interface
binding, classifier dispatch on connected drains, malformed frame offsets,
unauthenticated/duplicate/capped admission, drain close/reopen, and real socket
rebinding driven by injected interface facts. Cancellation is tested before
registration, after arming/before park (with simultaneous data readiness,
repeated cancellation and EBUSY reset), and after committed park; no sleeps.
`FipsdUdpTwoNodesV4Test` and `FipsdUdpTwoNodesV6Test` compile the **same complete
proof** as `test_node_two_nodes.c`: link, tree, filters, XK session, bidirectional
datagrams, session/link rekey, post-rekey traffic and disconnect.

The one-direction `FipsdUdpInteropSmokeTest` is superseded by the two-node
C<->Rust UDP interoperability gate (`FipsdInteropUdpTest`, `FipsdInteropUdpV6Test`;
see [C<->Rust UDP interoperability gate](#crust-udp-interoperability-gate-nostrc-f66sr1)).
All UDP targets belong to `fipsd_tests`. Native macOS ARM64 sanitizer runs are
not Linux/deployment proof.

`FipsdTransportTest` covers unit-vector framing (upstream has no
`tests/vectors/` transport corpus), every truncation/split/capacity offset,
malformed prefix fields, injected ifaddrs/ioctl facts, native lo0/en0 smoke on
macOS, real UDP sockbuf readback, medium ranking and link replacement/counters.
`FipsdTransportWatcherTest` drives the same reducer with an injected clock and
the actor with a real injected netmon source (absent, appears, disappears,
new-index return), plus real socketpair fiber stream I/O. Existing two-node
regressions now exercise the shared layer and assert provider carrier/medium
and actual packet counters. Both new targets belong to `fipsd_tests`.

Version decision: **fipsd remains 0.1.0 Unreleased**; libfips-core/libgo unchanged.
### Nostr event contracts (`nostrc-8hvs6.1`)

`src/nostr/{events,advert,traversal,inbox}.{c,h}` are pure event helpers, not a
running rendezvous subsystem. There are **no relay connections, subscriptions,
publish acknowledgements, timers or background workers** here. Activation and
relay lifecycle belong to `8hvs6.2`; parsing config does not activate discovery.
Builders take the existing `fips_identity` (one secp256k1 keypair, not a second
Nostr identity), explicit seconds and TTL. Parsers take explicit millisecond
`now`; caller outputs remain unchanged on error. Event outputs use
`nostr_event_free`, JSON strings use `free`. Payload values are bounded PODs;
use parser-produced values or initialize all fields and terminate all strings.

| API | Contract |
| --- | --- |
| `fipsd_nostr_event_parse` | Explicit-length bounded JSON preflight, signed event shape, canonical id and libnostr signature verification. Only the three FIPS kinds are admitted. |
| `fipsd_nostr_advert_build/parse`, `advert_content` | Addressable 37195; tag order `d`, `protocol`, `version`, NIP-40 `expiration`; Rust struct JSON field order. Config `app` is distinct from fixed `d`/body identifier. Unknown/unusable endpoints are dropped; NAT requires relay/STUN lists; non-NAT drops those optional lists. |
| `fipsd_nostr_inbox_build/parse` | NIP-17 create/parse adapters for 10050; deployed FIPS bare `r` output, both `relay` and unmarked `r` input, ordered URL deduplication (empty/root-slash equality), no NIP-65 read/write markers. Relays come from the caller/config, never compiled public defaults. |
| `fipsd_nostr_traversal_build/open`, `traversal_content/parse_content` | Kind-14 unsigned rumor, NIP-44, kind-13 identity-signed seal, NIP-59 wrap with a fresh one-time key, re-kind/re-sign to ephemeral 21059 with NIP-40 expiry. Both signatures/ids, seal/rumor author, recipient and payload identities are checked. Only the seal is backdated, matching upstream's custom outer wrap. |
| `traversal_freshness`, `traversal_answer_for_offer` | Saturating TTL clamp and 60,000 ms grace; distinct stale/future rejection and strict/skew success; reply correlation, reversed identities, accepted addresses/rejection reason. |
| `event_id`, `seen_contains/insert` | Verified binary event id and caller-owned bounded replay set. Insert **after** successful admission; full/duplicate refuses without eviction. The relay owner must provide explicit expiry/generation lifecycle and synchronization. |

`advert_parse` mirrors upstream's deliberate **future-time clamping**, not
rejection: `created_at` is capped at `(now + 60000)/1000`, then validity is the
minimum of NIP-40 expiry and created-time + max-age. This is not a permanent
replay tombstone. Traversal outer expiration has no grace; authenticated
payload timestamps use the upstream freshness grace. Answers must be opened
with the expected sender identity and checked against their pending offer by
the future relay/session owner before replay insertion or side effects.

#### Bounds and upstream differences

Pinned oracle: `6aa80504baa0fa312b392009eea0e3e59fb6fe4c`.
`src/nostr/runtime.rs:1967-2134`, `signal.rs:17,105-259,303-335`, and
`traversal.rs:28-49,398-426` are the corresponding upstream rules. The November
12 bounding fix limits **planning** to 32 offered local candidates and eight
punch targets; it does **not** cap serde's advert endpoint/relay/STUN vectors,
wire strings, or JSON body lengths. Do not mistake new C host safety limits
for upstream-enforced wire limits:

| C host ceiling | Limit (decoded UTF-8 bytes unless noted) |
| --- | --- |
| Signed JSON event / payload JSON | 65,536 / 16,384 bytes |
| Ciphertext / decrypted envelope preflight | 32,768 bytes |
| JSON tokens / depth / children per container | 1,024 / 8 / 64; object keys 128 bytes |
| Event tags / values per tag | 64 / 8; each string at most 1,024 bytes |
| Advert endpoints / signal relays / STUN servers | 32 / 16 / 8 |
| Endpoint address / relay or STUN URL | 256 / 1,024 bytes |
| Builder app / parsed advert transport label | 128 / 64 bytes |
| Session id, nonce, reply nonce / rejection reason | 128 / 256 bytes |
| Traversal local candidates / protocol / IP text | 32 / 16 / 64 bytes; port u16 |
| Npub / timestamps / advert version | canonical identity comparison on open, 63-byte npub text; u64 payload milliseconds, nonnegative i64 Nostr seconds; version 1 |

The C parser rejects oversized candidate arrays instead of allocating an
unbounded vector and later truncating planning. Raw signed-event syntax/tags
are preflighted without allocation; decoded payload parsing uses a fixed token
budget and fixed destination storage, with no JSON DOM allocation. Crypto,
libnostr event objects, URI validation and JSON serialization allocate only
inside bounded input budgets. Invalid UTF-8/escapes, NUL, duplicate decoded
keys, malformed numbers, overflow and trailing input are rejected. Advert
`d`/version tags and traversal rumor/seal shape are additionally checked in C.
Private/on-link punch admission and eight-target planning are not event
parsers and remain outside this task.

#### Differential fixtures

`FipsdNostrEventsTest` is in `fipsd_tests`. Its `nostr.json` family and raw
sidecars come from `tools/fips-vectors`: the root **fips-mesh** dependency at
the same pin compiles and parses the actual `OverlayAdvert`, `TraversalOffer`
and `TraversalAnswer` types. Its private `cfg(test)` fixture builders cannot
be called externally, so the four signed v0.44 captures are copied **verbatim**
from `src/nostr/fixtures/v044/`, with file/line provenance. Rust nostr 0.44
verifies all signatures and opens both encryption layers before export. No
recapture, deterministic signature regeneration, or calls to private helpers
are claimed. Immutable upstream signed test captures are not relay defaults.

The C test verifies every capture, decoded field and exact canonical payload;
rebuilds advert/inbox JSON including id, excluding randomized signature bytes;
and rebuilds/opens offers and answers, comparing decrypted payload bytes
rather than random ciphertext/keys/seal timestamps. SplitMix64-seeded property
tests cover 256 payload round trips including u64 boundaries, optional fields,
JSON escaping and candidate counts. Malformed/boundary, freshness, identity,
NAT/version/endpoint, URL normalization and replay-id tests run without relay
I/O or sleeps. This is local event-contract proof, not live relay interop.

### TUN device owner (`nostrc-9nxwk.1`)

`src/tun/` owns the OS device, not the IPv6-to-FIPS packet adapter. The current
configuration keys are **top-level** `tun.enabled`, `tun.name`, `tun.mtu`
(the upstream schema does not put them under `node`). With `tun.enabled: true`,
`main.c` registers TUN after the node subsystem and before control. The node's
startup barrier must report ready (including ready/degraded) and supply its
identity before any device syscall occurs. A not-ready node leaves TUN waiting;
a device configuration failure is visible as failed but does not kill the host.
There is no fabricated transport readiness or automatic packet sink.

One platform vtable keeps the later IPv6 shim OS-independent:

- Linux: `/dev/net/tun`, `TUNSETIFF` with `IFF_TUN | IFF_NO_PI`; acknowledged
  netlink `/128` address, MTU, up flag and `fd00::/8` main-table route changes.
- macOS: kernel-assigned utun via `PF_SYSTEM`/`SYSPROTO_CONTROL`; IPv6 alias/MTU/
  flags ioctls and `PF_ROUTE`. utun frames prepend/strip a **network-order**
  four-byte `AF_INET6` (30) prefix.
- FreeBSD: `/dev/tun` clone, kernel-reported name, `TUNSIFHEAD` (family 28),
  IPv6 alias/MTU/flags (including clearing `ND6_IFF_IFDISABLED`), `PF_ROUTE`,
  and destruction of the owned clone. Names on BSD/macOS are kernel assigned;
  `tun.name` selects the Linux name only.

All device/configuration/control/frame syscalls cross a per-device
`fipsd_tun_ops` seam, with no global fake state. The default route socket and
nonblocking/cloexec preparation reuse netmon's helpers. Control transactions run
on the non-fiber runtime start/stop thread; packet reads/writes use
`gof_read`/`gof_write` and park fibers. The shim must cancel and join its packet
I/O before device teardown; closing/reusing a descriptor is **not** cancellation.
No packet I/O fibers are started by this device-only layer.

Bring-up is open → node-derived `fd || NodeAddr[0..15]` address `/128` → MTU →
up → mesh route. Teardown reverses successful stages, closes the fd and (on
FreeBSD) destroys the clone. Failed destruction retains the owned name/index
and must succeed before another clone can open. Failed attempts fully unwind; `EBUSY`, absent
`ENOENT` and disappeared `ENODEV` devices get at most three immediate attempts,
without sleeps, takeover or deleting a foreign interface/route. Other errors
are not retried. Unlike upstream's persistent-TUN recovery/policy-rule logic,
this initial owner conservatively refuses existing Linux addresses/routes; it
does not flush pre-existing network state or install Linux policy rules.
BSD accepts an existing route only after a sequence-and-pid-matched lookup
proves it uses this interface; a borrowed route is not removed on stop.

The device MTU is `max(1280, min(tun.mtu, transport_mtu - 77))` with saturating
subtraction, optional configured ceiling, and a 65535 maximum. The configured
transport budget is the minimum configured transport payload MTU (1357 when
none exists). Thus 1357 → 1280 and 1472 → 1395 before a smaller TUN ceiling.
The **77-byte IPv6 shim overhead** is `106 + 4 + 7 - 40`; it must not replace
core `FIPS_OVERHEAD` / ECN overhead (106/110). Paths below 1357 still expose
IPv6's 1280 minimum and require the later shim's fragmentation/PMTU handling;
this device task does not claim those paths can carry packets yet. References:
upstream `docs/design/{fips-ipv6-adapter,fips-mtu}.md`.

When enabled, the composed retained control provider adds upstream adapter
`show_status` fields `tun_state`, `tun_name`, `effective_ipv6_mtu`, `host_drops`.
This C host intentionally composes these into the daemon control reply; upstream
serves them on a separate adapter status socket. Wire states follow upstream:
`disabled`, `configured` (waiting), `active`, `failed`; stopped maps to disabled.
`host_drops` is zero until there is a packet adapter; failed/not-up devices
report `tun_name: "-"`. There is no `show_tun` command in the pinned upstream
`control-socket.md`/adapter responder, so none is invented here. Device status
also exposes the address, ifindex, attempts and kernel error to host callers.

`9nxwk.2` builds on the device's frame API, ready-gated lifecycle and retained
status. Compression, identity-cache lookup, fragmentation, ICMPv6/MSS, DNS,
packet queues, per-destination PMTU and mesh delivery remain that layer's work.
Kernel mutations need Linux `CAP_NET_ADMIN`, or root for macOS utun/FreeBSD.
Ordinary tests use fake ops, byte fixtures and an unprivileged socketpair for
real two-worker fiber I/O. Route fixtures expand Linux UAPI and upstream
`ipv6tun/{bsdnet.rs,tests/bsdnet.rs}` layouts, including XNU 4-byte and FreeBSD64
8-byte sockaddr padding, and reject stale/out-of-order BSD replies by sequence.
The current route encoders target 64-bit little-endian host ABIs.

`FipsdTunPrivilegedSmokeTest` skips with an explicit message unless
`geteuid()==0` **and** `FIPSD_TUN_PRIVILEGED_SMOKE=1`. When explicitly enabled in
a disposable privileged environment, it creates/configures a real device,
pings its own address and tears it down. Do not elevate ordinary test runs.
## Nostr rendezvous runtime (`nostrc-8hvs6.2`)

`main.c` registers `nostr/runtime` after the node when
`node.rendezvous.nostr.enabled` is true. It copies the configuration, borrows the
node identity until shutdown, and owns a channel/select-driven host thread and a
`NostrSimplePool`. Blocking HTTP/WebSocket work never runs on a node fiber.
Cancellation joins the producer, pool workers and relay connections before the
node/identity is destroyed. No relay URLs are embedded in this runtime.

| Module | Ownership / contract |
| --- | --- |
| `nostr/runtime.{c,h}` | Configuration, pool/subscriptions, replaceable advert and inbox publication, EOSE cache boundary, refetches, bounded advert/session state, signal correlation and subsystem lifetime. |
| `nostr/driver.{c,h}` | `RendezvousDriver` vtable; node adapter learns public peer identities and enqueues dial candidates through the existing handler commands. |
| `nostr/failure_state.{c,h}` | Bounded oldest-touched eviction, saturating failure streak, warning interval, threshold-crossing refetch, cooldown, protocol-mismatch latch, retained skew and success/refresh resets. Actor-owned, not shared mutable node state. |
| `nostr/signal_gate.{c,h}` | Global pre-decrypt 5/sec, burst-20 allowance, plus a 5/sec, burst-20 reserve only while awaiting answers. Injected monotonic time; no timer or sender-key bypass. |
| `nostr/signal_route.{c,h}` | Pool middleware takes ownership of signals onto an independent 4096-entry mailbox and bounded event-ID replay memory. Advert traffic cannot evict signals from that mailbox or its replay guard. |

**Subscriptions and admission.** The persistent REQ contains a kind-37195,
`#d=fips-overlay-v1` advert filter, bounded by `since=now-advert_ttl_secs`.
Configured-only mode adds canonical peer authors (an empty set never becomes a
wildcard); open mode deliberately discovers arbitrary authors within that kind
and identifier, then validates the configured app and advert contract. The second
filter is kind 21059, `#p=<our identity>`, `since=now`, `limit=0`. The configured
advert/signal relay union is probed before connecting. NIP-11 must advertise
NIP-01/40, NIP-42 if authentication is required, at least two filters/subscriptions
when those limits are declared, and no mandatory payment/PoW. Unknown or explicitly
incompatible capability documents are refused and logged. NIP-59 is client-side
cryptography, not a required relay extension. Remote relay/STUN hints retain the
pure parsers' bounds; signal routing only selects already-probed configured relays.

The pool's ordered delivery API drains history before EOSE. The actor caches
history without dialing; EOSE releases catch-up candidates, and subsequent live
replacements produce candidates immediately. A canonical event ID and replaceable
`(author, created_at, lowest-id tie-break)` cache make replay idempotent across
relays and reconnects. Signal middleware validates target, bounds, expiry and
signature, applies the global gate, and remembers IDs only after successful queue
admission. It never sends a traversal signal through the advert/refetch mailbox.
A full mailbox fails visibly rather than manufacturing a live/empty-fetch result.

**Recovery and publication.** The pool's opt-in CLOSED backoff preserves the
existing authenticated connection, retains/logs the CLOSED reason, and reissues
REQ with a per-relay 1-second exponential ladder capped at five minutes; EOSE
resets it. AUTH is taken off the reader callback onto the actor and signed with
the FIPS identity via `nostr_relay_auth_wait`; that helper checks the event-ID OK
and frees its authentication event. Network disconnects/failed initial dials
have one retry owner, a channel-select deadline with the same bounded ladder.
Terminal relay objects are replaced while the pool worker is joined; retired
connection control frames cannot affect their replacements.

The actor requires an advert TTL of at least two seconds and clamps refresh
below that TTL; unsupported resource bounds fail construction explicitly.
The first advert is published only after the combined advert/signal REQ reaches
EOSE, so advertising cannot precede inbox readiness. Kind 37195 retains its
replaceable identifier and NIP-40 expiry; a `Ticker` republishes before expiry.
The ticker's wait is interruptible on stop (no refresh-interval shutdown stall).
Every advert, inbox and traversal publish uses `publish_and_wait`; negative OK
and missing OK are failures. This schedule republishes local state; it never
queries relays for new messages.

**Refetch outcomes (B6).** A separate one-shot pool on the connected advert
relays uses author/kind/identifier plus `until=now` and `limit=1`; its independent
middleware still sees an unchanged event. All selected relays must reach EOSE.
Failed/disconnected/CLOSED/incomplete fetches (including the failure deadline),
foreign-only or invalid newer results, and refused cache insertion are `Skipped`.
A successful empty result is `Evicted`; a matching effective timestamp no newer
than the cached advert is `SameAdvert`; a valid, admitted newer advert is
`Refreshed`. B6 deliberately compares effective timestamps only, exactly as upstream
`refetch_advert_for_stale_check`: equal timestamps remain `SameAdvert` even
though the live cache separately uses the replaceable-event ID tie-breaker.
Only `Evicted` and `Refreshed` reset the failure streak. A deadline
never proves absence. Signals continue on their independent channel during fetch.

**Traversal landing boundary.** Offers and answers are authenticated/opened by
the landed traversal parser; answers must match a pending offer's sender,
session and nonce and pass its freshness gate. Incoming offers pay global and
per-sender concurrent limits. Outgoing answer reservations have a separate bounded table (one active offer per
recipient), so full responder replay memory cannot deny our own traversals.
Unexpired seen sessions are never evicted to admit
another offer: full storage refuses new offers until replay expiry. The driver
exposes `traversal_offer`, `traversal_answer`, signal publication, session-finished
and peer-result seams. The traversal machine, STUN observation, adopted UDP
transport and named punch-packet classifier attachment remain `8hvs6.3` work;
missing callbacks refuse offers, never report a successful punch. The node
adapter advertises `udp:nat` only for an actually bound, opted-in UDP transport;
it never advertises a wildcard bind address as a public endpoint.

### Protocol-smell checklist (A–J, module and local relay tests)

- [x] A: Long-lived REQ/event delivery; no periodic relay queries.
- [x] B: History and successful fetch completion are EOSE-driven, never timer-closes.
- [x] C: Publish OK/false, CLOSED reasons/backoff and NIP-42 challenge/OK are observed.
- [x] D: Explicit history-cache → EOSE → live effects; recovery uses a bounded freshness window; refetch uses until/limit.
- [x] E: Kind/identifier/authors and kind/recipient/since filters; wildcard authors only for explicitly open discovery.
- [x] F: Independent bounded signal IDs/session memory and replaceable advert idempotency, including reconnect replay.
- [x] G: Channels are owned actor mailboxes, not relay polling/queue wrappers.
- [x] H: Every configured relay gets a cancellable NIP-11 probe; explicit refusal, multiple relays and authenticated retry.
- [x] I: Timers are only advert publication, reconnect backoff, reservation expiry or a failed-fetch deadline; none establishes history completion.
- [x] J: Grotto `bunker_test_relay` main-context/Soup fixture adapted to Jansson over real local WebSockets/HTTP, driven by main-context commands and channel barriers, no sleeps. Tests hold EOSE, replay duplicate history/live events across two relays, send AUTH with CLOSED, reject publishes, and exercise B6/session/freshness/failure contracts.

The fixture uses only libsoup and Jansson already present in the FIPS CI package
lists; it does not require JSON-GLib. The older Testing mock server has startup
sleeps/volatile service polling and was not used for these event-driven gates.

The upstream open-discovery startup scenario seeds adverts before node startup;
the local test preserves that shape and additionally holds EOSE before any dial
effect is permitted. These are local native tests, not public-relay, hosted-CI,
STUN, traversal or deployment proof. `8hvs6.4` remains the formal protocol review.

## TCP transport

`transports.tcp` uses the same singleton/named-instance configuration and node
registration path as UDP. (The canonical YAML key is **`transports.tcp`**, not
`node.transports.tcp`.) Omit it to disable TCP; an empty instance enables
outbound-only operation. A numeric `bind_addr` enables inbound acceptance:

```yaml
transports:
  tcp:
    bind_addr: "127.0.0.1:2121"  # or "[::1]:2121"; omit for outbound only
    mtu: 1400
    connect_timeout_ms: 5000
    nodelay: true
    keepalive_secs: 30          # 0 disables kernel TCP keepalive
    recv_buf_size: 2097152
    send_buf_size: 2097152
    max_inbound_connections: 256
    max_inbound_per_source: 8
```

All keys at the Rust pin already exist in the configuration schema, including
`advertise_on_nostr`, `external_addr`, `role` and `kind`. This transport does not
add a relay actor or change the rendezvous driver's advertising capabilities.
An omitted `max_inbound_connections` inherits `node.limits.max_connections`;
an explicit value, including zero, wins. Per-source admission canonicalizes
IPv4-mapped addresses and counts IPv6 /64 prefixes. Pending outbound attempts
reserve a node-budget-sized host pool; inbound limits do not consume that
outbound budget. Outbound DNS resolution runs on the blocking executor, tries
all returned IPv4/IPv6 candidates under one connect deadline, and preserves
the caller's opaque endpoint identity. Native DNS itself is subject to the
host resolver's timeout, not the TCP connect timeout.

### Implementation and ownership

- `tcp/{tcp.c,tcp.h}`: vtable, config registration, copied nonblocking send
  admission, configured MTU, public address/readiness and stats interfaces.
- `tcp/listener.c`: fiber accept, endpoint resolution, listener binding,
  nonblocking/CLOEXEC sockets, keepalive/NODELAY and shared socket buffers.
- `tcp/conn.c`: one tracked owner per connection, a connect/reader fiber and
  a separate writer fiber. Only the writer writes; no dispatcher waits on
  socket writability. A 64-frame owned queue refuses immediately when full.
- `tcp/pool.c`: inbound local/remote tuples, outbound endpoint deduplication,
  newest-inbound/outbound-first lookup, capacity/source admission and exact
  owner removal. A closing endpoint cannot be replaced ahead of its exit event.
- `tcp/stats.c`: atomic upstream TCP counters, projected by `show_transports`
  alongside the shared byte/frame counters. Pool outbound counts established
  sockets, while the admission budget also reserves pending attempts.

The shared `framing`/`stream` code consumes the existing four-byte FMP prefix:
there is **no extra TCP length envelope**. Send MTU is the configured value
(default 1400), including the FMP frame. Receive ceiling is 65535 regardless
of local send MTU. Linux `TCP_MAXSEG` is diagnostic only, not a frame limit;
TCP segmentation is left to the kernel. The shared presence/watcher and medium
modules are reused; unbound TCP reports unknown medium rather than guessing
Ethernet from an IP address.

The node's connection-oriented dial/retry state machine owns reconnection and
exponential backoff. Transport `CONNECTED`/`DISCONNECTED` events drive it;
there is no second retry loop. The C vtable's send is the equivalent of Rust's
`send_existing`, not its connect-on-send convenience API: late encrypted work
must never reopen a deliberately closed link. The current vtable has no lane
argument, so its single bounded writer queue does not claim Rust's separate
path-priority lane support.

Stop closes cancellation channels, uses libgo's owner-directed
`gof_io_cancel_fd` latch (including connect/read/write pre-park races), joins
both I/O fibers, then resets and closes the fd. It never uses close as a wake.
Deliberate peer close drains queued frames for at most the upstream five-second
grace; stop and root cancellation interrupt that drain. Inbound first-frame
admission expires after 30 seconds; subsequent complete frames re-arm the
node-derived link-silence bound (64 seconds at defaults). Deadlines cover
whole frames, not individual bytes.

### Verification

`fipsd_tests` includes `FipsdTcpTest` (framing, real IPv4/IPv6 streams, ownership,
MTU asymmetry, queue/pool/source limits, socket options, deadlines, reconnect,
connect-arm/readiness cancellation and committed mid-read cancellation) and
`FipsdTcpTwoNodesV4Test`/`V6Test`. The latter reuse the full two-node proof:
IK link, tree convergence, bloom, XK session, datagrams, rekey and disconnect,
with transport counters read through the production control provider. Socket
fixtures use `test_tmpdir.h`; synchronization is channels, I/O readiness and
explicit trace barriers, never warm-up sleeps.

`FipsdTcpInteropTest` runs `tests/tcp_interop.py` against the real C daemon and
an independently built pinned Rust daemon. Set CMake's
`FIPSD_TCP_INTEROP_RUST_BINARY` (or that environment variable at configure time)
to enable it; otherwise it returns SKIP (77). It verifies authenticated TCP
links in both control views and clean SIGTERM joins. This is link interop,
not a cross-language XK/datagram or deployment claim. A local pinned build is:

```sh
CARGO_TARGET_DIR=/tmp/fips-rust-target-tcp cargo build --locked --release
```

Keep that target outside the read-only Rust checkout and delete it after the
run. No dependency or CI workflow changes are needed. Version decision:
**fipsd stays 0.1.0 Unreleased**; the TCP addition folds into the initial daemon,
with no published release/tag or FMP wire-format change.
## C<->Rust UDP interoperability gate (`nostrc-f66sr.1`)

`tests/udp_interop.py` runs one `fipsd` and one `fips` daemon built from the
`libfips-core/UPSTREAM.md` pin (`6aa80504`) on loopback UDP, once per role
assignment (C dials Rust, then Rust dials C), for IPv4 (`FipsdInteropUdpTest`)
and IPv6 (`FipsdInteropUdpV6Test`). Both tests report CTest **skipped** (exit 77,
`SKIP: ...`) unless `FIPSD_UDP_INTEROP_RUST_BINARY` names the Rust daemon. Every
step is an observation through one of the two control sockets or a daemon log,
waited for under a deadline; the only pause is the 100 ms poll between Rust's
tick-published control snapshots. A failure prints both daemons' complete logs
and the harness trace. The two daemons never share configuration beyond
`node.rekey.after_secs: 25`, `tun`/`dns` off and a loopback `transports.udp`
bind; the Rust side also enables `node.native_api`.

What each step observes, per role assignment:

| Step | C side (`fipsd` control socket) | Rust side (`fips` control socket, native API) |
|---|---|---|
| IK link | `connect` on the initiator; `show_links[0].state == active` with the assigned direction; `show_peers[0].npub` is the Rust npub; `show_status` peer/link counts are 1 | `show_links[0].state == connected` with the complementary direction; `show_peers[0].npub` is the C npub |
| Tree and filters | `show_tree`: both agree on `root`, one side is root at depth 0 and the other hangs at depth 1 with `parent` = the root's `my_node_addr`, `peer_tree_count 1`; `show_bloom.peer_filters[0].has_filter`; `show_peers[0].is_parent`/`is_child` match the tree | the same fields |
| XK session | C dials: a C `probe_start` opens it (`is_initiator true`); Rust dials: Rust's first native-API write opens it (`is_initiator false`). `identity proven`, `state established` | the complementary `is_initiator`; `Session established (... XK)` in the log |
| Datagrams | Rust->C application datagrams: `show_metrics.native.received_datagrams`/`received_bytes` equal what was written, before and after the rekey. Session traffic both ways: a C probe and a Rust probe over the live session (`rtt.samples >= 1`, `reports_seen >= 1`, session stage `skipped`/preexisting, `session_left_intact`) and the prober's `show_sessions[0].mmp.srtt_ms` | native-API `connect` (reply plus the flow descriptor over `SCM_RIGHTS`), `send` on the flow; `show_native_flows` lists the flow with matching `sent_datagrams`/`sent_bytes` |
| Rekey | `show_sessions[0].current_k_bit` then `show_peers[0].current_k_bit` flip from the value first observed, with exactly one session throughout; then a datagram and both probes again under the new keys; no non-zero mismatch/unknown/expired/unanswered counter in `show_metrics.session`/`handshake` | the same fields |
| Shutdown | SIGTERM -> exit 0, no sanitizer marker in the log, control socket unlinked | SIGTERM -> exit 0, control, `-ipv6tun` status and API sockets unlinked |

When C dials, the C probe opens the XK session and the Rust->C datagram is sent
once the probe's session stage has passed (the observe that records the probe's
adoption baseline), so the probe leaves the session in place
(`left_intact_reason: adopted_by_traffic`) instead of tearing it down; the
harness asserts that reason.

**Not verified:** a C->Rust *application* datagram. fipsd has no native datagram
API yet (Tier B4) and TUN delivery needs privileges, so the C->Rust direction is
proven at the session layer (the C probe's CoordsWarmup and MMP reports, which
Rust answers with the reports the C probe measures RTT from), not with an
application payload. Rust->C application delivery is verified end to end through
C's `native` counters. Measured locally on macOS ARM64 (default build, pin
`6aa80504`): 50-60 s per role assignment, dominated by the rekey waits (session
rekey observed 13-30 s after establishment, link rekey 40-51 s after the link
came up); all four role/family scenarios passed.

Run it locally:

```bash
# Read-only pinned upstream checkout; build only into a scratch target directory.
CARGO_TARGET_DIR=/tmp/fips-rust-target cargo build --locked --release --bin fips \
  --manifest-path /path/to/fips/Cargo.toml          # HEAD = libfips-core/UPSTREAM.md pin
cmake -S . -B build -G Ninja -DBUILD_FIPS=ON \
  -DFIPSD_UDP_INTEROP_RUST_BINARY=/tmp/fips-rust-target/release/fips   # plus the usual BUILD_* switches
cmake --build build --target fipsd fipsd_test_tmpdir_run
ctest --test-dir build -R '^FipsdInteropUdp' --output-on-failure
# Or one family/role by hand (the harness makes its own temp dir when FIPSD_TEST_TMPDIR is unset):
python3 fipsd/tests/udp_interop.py --fipsd build/fipsd/fipsd \
  --rust /tmp/fips-rust-target/release/fips --family 6 --initiator rust
```

CI runs the gate in the `interop-udp` job of `.github/workflows/fips-ci.yml` on
ubuntu-24.04 and macos-latest: it reads the pin from `libfips-core/UPSTREAM.md`,
checks upstream out at that commit, builds the daemon with the toolchain named by
upstream's `rust-toolchain.toml` (`cargo build --locked --release`), caches the
resulting binary keyed on OS, architecture, pin and toolchain, builds fipsd with
ASan/UBSan and runs `ctest -R '^FipsdInteropUdp'`.

Two fipsd corrections came out of the gate, both PATCH-class control output with
regressions in `FipsdNodeHandlersTest` and `FipsdNodeTwoNodesTest`: a probe
report now states the host's teardown refusal (`session_left_intact`,
`left_intact_reason`) instead of the core's `torn_down` for a session the host
kept, and `show_sessions.state` uses upstream's vocabulary (`established` while
draining with `is_draining: true`; `initiating`/`awaiting_msg3` before
establishment) instead of `draining`/`handshaking`.
