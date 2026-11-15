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
consumes the runtime's SIGHUP reload channel. Not implemented yet: the control
socket (`9keoz.3`), transports and the node. Parsing a setting does not claim
that its owning subsystem is implemented.

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
retained and logged as requiring restart. CLI file selection/environment level
precedence remains in effect. Log limits change under the logger's write mutex.
Upstream `node/reloadable.rs` defines hosts/ACL resource snapshots, **not a YAML
reloadable-key list**; upstream `fips.rs` has no SIGHUP YAML reload. Therefore
this whitelist is an explicit C-host contract, not a claim of upstream YAML
reload parity. Peer-list/hosts/ACL application awaits their owning subsystems.

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

- `go_fiber` has no ThreadSanitizer fiber annotations
  (`__tsan_switch_to_fiber`). A fiber resumed on a different worker thread
  unbalances TSan's per-thread shadow stack and TSan faults inside its own
  runtime at worker exit (about 10% of two-worker runs; never under ASan, in
  plain builds or with one worker). The fipsd TSan tests therefore run with
  `FIPSD_WORKERS=1`; multi-worker migration is covered by the other lanes.

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
Later tasks plug transports, the node and the control socket in as
`fipsd_subsystem`s in that order and stop them in reverse.

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
FSP quorum/ingress gates remain in libfips-core. Cross-transport rebinding of an
attached link refuses BUSY until the node can close/re-attach its bindings; same
transport address roaming updates both table keys without touching Noise state.

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

### Verification

`FipsdPeerTest` (in `fipsd_tests`) ports representative lifecycle/reconciler,
ActivePeer, budget and identity-cache contracts: retry/cooldown boundaries,
resend success/failure, restart epoch matrix, both crossing outcomes/own-dial,
late authorization and loss, cap rejection/atomic promotion, table bounds and
rebind collisions, local-key ownership, tree/filter state, real AEAD/replay and
linear lease release through the rekey seam, and concurrent immutable snapshots
while the real runtime dispatcher processes 500 channel commands. It reuses
`libfips-core/tests/fakes/host.c`; all clock advances are injected, no sleeps.
TSan pins `FIPSD_WORKERS=1` for the runtime limitation described above; other
lanes use two workers. This is host-module proof, not a network/interoperability
claim and not a port of upstream's multipath/settlement/node test suites.
