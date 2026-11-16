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
transports and the native/TUN host
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

`node/transport_loopback.{c,h}` is an in-process datagram medium behind the
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
msg2 loss with msg1 retransmission (metrics prove one msg2 and one promotion),
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

### Remaining for `9keoz.8`, Tier B2 and B4

`9keoz.8` / Tier B2: real UDP/TCP/connected receive/binder legs behind the
transport seam, discovery, transport interface presence and native network
interoperability/performance. No real socket dataplane or deployment proof is
claimed by loopback tests. B4: native API and IPv6 adapter owners replacing
`native.c`/`host_link` notices, clamp publication/MSS application to the adapter,
configured-peer address lists into path intake, ACL and rendezvous.

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
