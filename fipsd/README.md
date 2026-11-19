# fipsd

The host daemon for the C FIPS port. `libfips-core` owns pure protocol
computation; this component drives it using libgo concurrency, the `go_fiber`
runtime and libnostr. Sockets, clocks, secure randomness, configuration,
logging, signals, privileges and platform-specific work belong to this host,
never to the core.

## Host components

| File | Role |
| ---- | ---- |
| `src/main.c` | Entry point: parse flags, open the log first, umask, bring the runtime up, run until a stop, map the result to an exit code; on Windows also the service body and `--install-service`/`--uninstall-service`. |
| `src/service.{c,h}`, `src/service_win32.c` | The Windows service: the SCM status sequence as pure data (every host), and the dispatcher, control handler and install/uninstall over the SCM API (Windows). |
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
the IPv6 packet shim (`src/ipv6/`, `9nxwk.2`) runs on it. Parsing a setting does not claim that its owning subsystem is
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
| `--install-service` (Windows) | Register (or repoint) the `fipsd` service at this executable with `--service`, automatic start, LocalSystem. | 0, or 1 with the reason |
| `--uninstall-service` (Windows) | Stop the service (waiting on the SCM's own notification) and delete it. | 0, or 1 |
| `--service` (Windows, hidden) | The registered service's command line: run under the SCM. From a console it fails at once with a hint. | the daemon's exit code |

The three Windows flags are upstream's `cfg(windows)` ones; elsewhere they are
unknown flags (exit 2).

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

`native.drop_client_buffer_full` is C-only, beyond upstream's 18 native names:
where flows use `SOCK_DGRAM` (macOS, FreeBSD) a client that is not draining
makes the daemon's write fail with `ENOBUFS` and no writability event, so the
datagram is dropped. Upstream drops it with only a debug log; fipsd counts it.
On Linux (`SOCK_SEQPACKET`) the writer waits instead and the counter stays zero.

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
platforms with a `go_fiber` target (Linux, macOS, FreeBSD and, since
nostrc-iq1n9.6.1, Windows; see below). `CMakeLists.txt`
owns the component's version, 0.1.0 Unreleased. Warning flags honor nostrc's
shared `GO_WARNINGS_AS_ERRORS`; sanitizer flags reuse its `GO_ENABLE_*`
helpers.

## Building on Windows (MSYS2)

Verified on Windows 11 ARM64 (10.0.26200) with MSYS2's CLANGARM64 environment
(clang 23); the steps are the same in CLANG64/UCRT64 with that environment's
package prefix. libgo runs fibers on the Windows Fibers API and I/O on an I/O
completion port (`libgo/fiber/README.md`, "Windows").

1. Packages (from an MSYS2 CLANGARM64 shell):

   ```sh
   pacman -S --needed git mingw-w64-clang-aarch64-{cmake,ninja,pkgconf,openssl,libsodium,jansson,libyaml,nsync,libwebsockets,curl,glib2,python,ncurses}
   ```

   `glib2` is needed because `src/nostr/events.c` includes `glib.h` and the
   top-level test tree configures against it; `ncurses` (optional) builds
   `fipstop`.

2. libsecp256k1 (no MSYS2 package), installed into the environment prefix:

   ```sh
   git clone --depth 1 https://github.com/bitcoin-core/secp256k1.git
   cmake -S secp256k1 -B secp256k1-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_INSTALL_PREFIX=$MINGW_PREFIX -DSECP256K1_ENABLE_MODULE_RECOVERY=ON \
     -DSECP256K1_ENABLE_MODULE_ECDH=ON -DSECP256K1_ENABLE_MODULE_SCHNORRSIG=ON \
     -DSECP256K1_ENABLE_MODULE_EXTRAKEYS=ON -DSECP256K1_BUILD_TESTS=OFF \
     -DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF -DSECP256K1_BUILD_BENCHMARK=OFF
   cmake --build secp256k1-build && cmake --install secp256k1-build
   ```

3. Configure and build (nsync comes from the `nsync` package; the vendored
   submodule also works). The configure-time and pre-push checkers
   (`scripts/check-*.py`) read sources as UTF-8 whatever the Windows code page
   (`nostrc-do87t`), and `FIPSD_ENABLE_BLE` defaults OFF on Windows
   (`nostrc-znhk6`): upstream has no Windows BLE backend, so a configured BLE
   transport is ignored with a warning there either way (`ble_disabled.c`),
   and the core's simulated radio and stream fast path are built on POSIX
   `socketpair()`/`fcntl()` descriptors; turning it ON on Windows is a
   configure error. No extra flag or environment is needed:

   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
     -DGO_WARNINGS_AS_ERRORS=ON -DBUILD_TESTING=ON -DBUILD_FIPS=ON \
     -DBUILD_TESTING_FRAMEWORK=OFF -DBUILD_SIGNER_TESTS=OFF -DLIBNOSTR_BUILD_TESTS=OFF \
     -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF -DBUILD_LIBHANAMI=OFF \
     -DBUILD_GNOSTR_APP=OFF -DBUILD_GROUNDHOG=OFF -DBUILD_RELAYD=OFF -DSIGNET_ENABLE=OFF \
     -DENABLE_NIP55L=OFF -DENABLE_NIP5F=OFF -DENABLE_NIP57=OFF \
     -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF -DBUILD_NOSTR_GOBJECT=OFF \
     -DBUILD_NOSTR_GTK=OFF -DBUILD_APPS=OFF -DBUILD_LIBBLOSSOM=OFF \
     -DBUILD_LIBPORTHOME=OFF -DBUILD_LIBVAULT=OFF -DBUILD_LIBVAULT_MARMOT=OFF -DENABLE_BUD02=OFF
   cmake --build build --target fipsd fipsd_tests fipsctl fipstop
   ninja -C build libfips-core/all libgo/all test_channel_incremental
   ```

4. Tests (run from the CLANGARM64 shell so the environment's DLLs are on
   `PATH`):

   ```sh
   ctest --test-dir build -R '^Go|^FipsCore|^FipsDiff|^Fipsd|^FipsTop' --timeout 300
   ```

   `tests/windows.cmake` lists the Windows subset (same names, labels and
   timeouts as the POSIX suite): the node, handler, dataplane, ACL, peer and
   stats suites, the two-node proof on the loopback transport and on real
   UDP (`FipsdUdpTwoNodesV4Test`/`V6Test`: IK link, tree, bloom, XK session,
   datagrams, rekey, disconnect), `FipsdUdpConnResetTest` (below), and the
   TUN/IPv6 suites of "TUN on Windows (Wintun)", the control socket on TCP
   (`FipsdControlTcpTest`, `FipsdControlTcpCapacityTest`), the service status
   sequence (`FipsdServiceStatusTest`), the CLI with the service flags, and
   fipstop's model, renderer and golden frames (`FipsTop*`). Suites that need
   AF_UNIX control/native sockets, POSIX signals or file modes, socket pairs,
   or one of the subsystems below are not built there yet.

5. Running: `build/fipsd/fipsd.exe -c fips.yaml` (or as the service; see
   "Windows service, control socket and packaging"). Without `-c` the daemon
   searches `C:\ProgramData\fips\fips.yaml`, then `%APPDATA%\fips\fips.yaml`,
   then `fips.yaml` in the working directory (upstream's Windows layout; never
   the drive-relative `\etc\fips`). Admission ACL files default to
   `C:\ProgramData\fips\peers.{allow,deny}` and `hosts`. Ctrl+C/Ctrl+Break are
   SIGINT, closing the console, logoff and shutdown SIGTERM (a console control
   handler feeding the signal fiber through a loopback socket pair); there is
   no SIGHUP reload on Windows yet. Two daemons on one host:

   ```yaml
   node:
     identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: false}
     control: {enabled: false}
   tun: {enabled: false}
   dns: {enabled: false}
   transports:
     udp: {bind_addr: '127.0.0.1:21001'}
   peers:
     - npub: '<the other node's npub, logged as "handlers attached: npub1...">'
       addresses: [{transport: udp, addr: '127.0.0.1:21002'}]
   ```

   Each log shows `link established with <node_addr>` and, at
   `FIPSD_LOG_LEVEL=debug`, the link's MMP reports. A daemon started with
   `Start-Process` gets its own console: stop it with Ctrl+Break sent to that
   console (`tests/windows_two_node_smoke.ps1`, `Send-CtrlBreak`); a
   `CTRL_C_EVENT` does not reach it.

What runs on Windows: the Windows service, the control socket on TCP
127.0.0.1:21210 with `fipsctl` and `fipstop`, the UDP transport (exclusive bind, no `SO_REUSEPORT`
connected per-peer sockets, so every peer uses the listener as upstream does on
Windows; `SIO_UDP_CONNRESET`/`SIO_UDP_NETRESET` off; `IP_PKTINFO`/`IPV6_PKTINFO`
and `IP_RECVECN`/`IPV6_RECVECN` ancillary data), the in-process loopback
transport, node, handlers, dataplane and crypto workers, the TUN device on
Wintun and the IPv6 shim, configuration, ACLs, peer owners, the Nostr event
contracts, logging and the runtime. Interface
presence, medium and the network monitor read the IP Helper API (one
translation unit, `src/node/ifinfo_win32.c`, because its headers include
`ole2.h`); the monitor runs on its 1 s backstop (no route-change wakes yet).
POSIX socket headers map to Winsock through `src/compat/win32/`.

Not yet on Windows (each replaced by a `*_unsupported.c` stub that fails
start-up when configured; the default-on DNS responder warns and serves
nothing instead): the `.fips` DNS responder, the TCP, Tor and Nym
transports, Nostr relay rendezvous and traversal, mDNS and BLE. Not on
Windows by design, as upstream: the native datagram API and libfipsnative
(it hands descriptors over `SCM_RIGHTS`, which Windows does not have; upstream
builds it on Linux, FreeBSD and macOS only) and the gateway (nftables and
proxy NDP; upstream builds it on Linux only). The Windows CI job is described
in "Windows CI".

### TUN on Windows (Wintun)

`src/tun/tun_windows.c` puts a [Wintun](https://www.wintun.net/) 0.14 adapter
behind the same `fipsd_tun_platform`/`fipsd_tun_ops` seam as the other
platforms, so the IPv6 shim (`src/ipv6/*`) runs on Windows unchanged.

**wintun.dll.** Not in the tree: fetch the signed release and check it, then
put the DLL for the build's architecture **beside `fipsd.exe`** (and beside
the test executables, `build/fipsd/tests/`, for the TUN tests). fipsd loads it
at run time with `LoadLibraryExW(LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
LOAD_LIBRARY_SEARCH_SYSTEM32)`: the executable's directory or System32, never
the working directory or `PATH`. The zip's SHA-256 is the one upstream fips
CI pins:

```powershell
Invoke-WebRequest -UseBasicParsing https://www.wintun.net/builds/wintun-0.14.1.zip -OutFile wintun-0.14.1.zip
(Get-FileHash -Algorithm SHA256 wintun-0.14.1.zip).Hash  # 07C256185D6EE3652E09FA55C0B673E2624B565E02C4B9091C79CA7D2F24EF51
Expand-Archive wintun-0.14.1.zip wintun
Get-AuthenticodeSignature wintun\wintun\bin\arm64\wintun.dll  # Valid, CN=WireGuard LLC
Copy-Item wintun\wintun\bin\arm64\wintun.dll build\fipsd\  # amd64\ on x64
```

A missing DLL fails TUN start-up with `wintun.dll not found: put the arm64
wintun.dll ... beside fipsd.exe`; one without the 0.14 exports, with `does not
export the Wintun 0.14 API`.

**Administrator.** Creating an adapter needs an elevated token (an
Administrator console, a service, or an SSH session of an administrator; `whoami
/groups` shows `High Mandatory Level`). Otherwise start-up logs `creating
Wintun adapter fips0 needs Administrator: run fipsd elevated` and continues
without TUN, as a failed device does on the other platforms.

**Adapter.** Named `tun.name` (default `fips0`), tunnel type `FIPS` (Windows
shows "FIPS Tunnel"), with a system-chosen GUID (upstream passes a random
one), a 2 MiB session ring (upstream's) and the adapter MTU
`fipsd_tun_mtu()` as everywhere. Upstream names every adapter `FIPS` and
passes `tun.name` as the tunnel type; here the name is per daemon, so two
daemons on one host each have their own adapter and `tun_name` reports the
alias Windows shows. A name an interface in use already carries (one with an
IP interface, or up), or another Wintun adapter's name, is refused (`EEXIST`):
Wintun would rename the foreign adapter to make room. A just-stopped adapter
is not in use (its IP interfaces are gone before `WintunCloseAdapter` returns;
Plug and Play removes its row about 100 ms later), and adapters a killed
daemon left are removed by Wintun at its next call.

**Receive.** Wintun has no descriptor to wait on. A dedicated thread runs
Wintun's own loop (`WintunReceivePacket` until `ERROR_NO_MORE_ITEMS`, then
`WaitForMultipleObjects` on the session's read event and a quit event), copies
each packet, releases its ring slot at once and sends the copy into a bounded
GoChannel (256 packets); a full channel blocks the thread, so overload backs up
into the ring, where Wintun drops as a NIC queue does. The reader fiber takes
packets without blocking and, when there is none, parks reading the device's
`fd`: one end of a connected loopback UDP pair (the doorbell), rung only after
the reader said it is about to park. The shim's `gof_io_reset_fd`/
`gof_io_cancel_fd` therefore act on a real socket the completion port knows,
and a stop ends the parked read with `ECANCELED`. Nothing polls. **Send**:
`WintunAllocateSendPacket`/`WintunSendPacket` from the writer fiber; a full
ring is `ENOBUFS` (the shim counts a write error).

**Host configuration** (upstream `winnet.rs`, one IP Helper translation unit,
`src/tun/winnet_win32.c`, apart from fipsd headers because of `ole2.h`'s
macros): wait for the adapter's IPv6 interface on IP Helper's
interface-change notification (at most 5 s; upstream polls every 50 ms), turn
duplicate address detection and router discovery off on it (beyond
`winnet.rs`: otherwise the `/128` is tentative and unusable for its first
second; WireGuard for Windows does the same), then the node's `fd...`
address `/128` (`CreateUnicastIpAddressEntry`, manual origins, infinite
lifetimes), the IPv6 MTU (`SetIpInterfaceEntry` `NlMtu`) and the `fd00::/8`
on-link route (`CreateIpForwardEntry2`, metric 256, `MIB_IPPROTO_NETMGMT`),
all keyed on the adapter's LUID; a row already present counts as done, and a
route this device did not create is left at stop. Stop deletes the route and
the address (a row someone else already removed counts as done: Windows
answers that delete `ERROR_INVALID_PARAMETER`, so the row is read back), stops
the thread, ends the session and closes the adapter, which removes it.
Neighbor unreachability detection stays on (Windows refuses to turn it off on
a Wintun interface), so now and then a unicast Neighbor Solicitation to a mesh
peer the host talks to crosses the mesh. `src/tun/winnet.{h,c}` holds the
rows, the create/delete judgement, the Win32-to-errno vocabulary, the ring
bounds and the UTF-16 names as pure data, tested on every host
(`FipsdTunWinnetTest`).

**Running.** From an elevated shell, with `tun: {enabled: true, name: fips0}`
in the configuration (and Windows Firewall rules for whatever the host should
accept from the mesh: each new adapter is an unidentified, Public network, so
inbound ICMPv6 echo and services are blocked by default):

```powershell
Get-NetAdapter -Name fips0                                    # FIPS Tunnel, Up
Get-NetIPAddress -InterfaceAlias fips0 -AddressFamily IPv6    # fd..../128, Preferred
Get-NetRoute -InterfaceAlias fips0 -DestinationPrefix fd00::/8  # on-link, metric 256
Get-NetIPInterface -InterfaceAlias fips0 -AddressFamily IPv6 | Format-List NlMtu, DadTransmits, RouterDiscovery
ping -6 <a peer's fd00::/8 address>
```

**Tests** (from the CLANGARM64 shell, wintun.dll beside the executables):
`FipsdTunWinnetTest` (also the `MIB_*` rows field by field) and
`FipsdTunWindowsTest`, which drives the real backend against a fake Wintun
through `fipsd_tun_ops.wintun` with no driver or elevation: bring-up order and
rows, 303 packets in order through the thread, channel and doorbell (more than
the channel holds), writes and a full ring, a parked read cancelled as the
shim stops it, the session ending under a reader, every failure unwinding what
it made, and the shim answering an unknown `fd00::/8` destination through the
device; the IPv6 codec/ICMP/firewall suites. The privileged ones skip (77)
unless the token is elevated and `FIPSD_TUN_PRIVILEGED_SMOKE=1`:

```sh
FIPSD_TUN_PRIVILEGED_SMOKE=1 ctest --test-dir build -R 'FipsdTun|FipsdIpv6|FipsdWindowsTwoNode' --output-on-failure
```

`FipsdTunPrivilegedSmokeTest` creates a real adapter twice in turn and reads
the `/128` (preferred), MTU, route and DAD/router-discovery settings back
through IP Helper and `Get-NetIPAddress`/`Get-NetRoute`/`Get-NetIPInterface`/
`Get-NetAdapter`, pings the address, removes the rows behind the device before
a stop that must still succeed, and checks the adapter is gone.
`FipsdIpv6PrivilegedSmokeTest` runs one node with the shim on a real adapter:
its own address answers ping, and a ping of `fd00::dead:beef` is routed by
Windows into the adapter, read by the shim and answered with a Destination
Unreachable that Windows hands to ping (`Destination net unreachable.`).
`FipsdWindowsTwoNodeSmokeTest` (`tests/windows_two_node_smoke.ps1`) starts two
daemons peered over UDP on `127.0.0.1`, each with its own adapter. Both
`/128`s live in one IP stack, but Windows sends by the strong host model: a
packet whose source is A's address leaves through A's adapter even to B's
address on the same host. So `ping -S <A> <B>` crosses the mesh and back (the
first reply ~250 ms, the session handshake), each adapter's receive counter
showing exactly the requests or replies its shim wrote, the same from B to A,
and a TCP connect and echo between sockets bound to the two addresses crosses
both ways; then both daemons stop on Ctrl+Break (exit 0, no error, no adapter
or route left). Without `-S` Windows may pick either adapter or answer its own
address locally. The script adds firewall rules for ICMPv6 echo and its TCP
port on the two adapters for the run and removes them.

**UDP and ICMP port-unreachable.** Every UDP socket the transport opens turns
`SIO_UDP_CONNRESET` and `SIO_UDP_NETRESET` off (upstream
`ignore_port_unreachable`), so one peer whose port is closed cannot fail the
receive every peer shares. `FipsdUdpConnResetTest` shows a plain socket's next
receive failing with `WSAECONNRESET` after a datagram to a closed loopback
port, then the transport's socket receiving another peer's datagram instead,
with a plain receive and through libgo's completion port (skips 77 if the host
reports no reset at all). The TUN doorbell pair needs neither: one end only
sends, the other only receives, from each other.

### Windows service, control socket and packaging (`nostrc-iq1n9.6.3`)

**Control socket.** As upstream (`src/control/mod.rs` `windows_impl`,
`docs/reference/control-socket.md`): a TCP listener on `127.0.0.1` whose port
is `node.control.socket_path`, which on Windows is a port number, `21210` by
default. One that does not parse is logged and replaced by 21210, as
upstream; `0` binds an ephemeral port. The listener is the Unix socket's
production code (`src/control/control.c`): one request line of at most 4096
bytes, one response, the 5 s read, execution and write deadlines and the
32-connection cap. Windows differences, all in that file: the port is bound
exclusively (`SO_EXCLUSIVEADDRUSE`, so no other socket of any account can
take it over while the daemon holds it; upstream's tokio listener does not
set it); every accepted peer is checked to be loopback, as upstream does; a
deadline cancels the connection's pending overlapped read or write through
libgo (`gof_io_cancel_fd`), because `shutdown()` does not complete a pending
overlapped receive, and a read-deadline cancel is cleared before the reply is
written so the client still reads `read timeout`; stop cancels the pending
`AcceptEx` the same way; every listener and accepted socket is reset first,
because Windows recycles a `SOCKET` value at once.

**Trust model.** Upstream's, unchanged: the socket is reachable from
loopback only and has no access control beyond that, so any process of any
local account can query the daemon and issue every command (`connect`,
`disconnect`, `path_pin`, ...). That is acceptable on a single-user host and
not on a shared one; Windows Firewall does not filter loopback, so it cannot
narrow it. Upstream lists a named pipe with an ACL or a token file as future
work and implements neither, so neither is ported. The configuration
directory is the protected part (below).

**Ports.** `21210` is the control socket. `21212`, the control port plus 2,
is where upstream's separate IPv6 adapter serves its status and where
`fipsctl`/`fipstop` look for the TUN keys when a node's `show_status` lacks
them (`fipsctl_ipv6tun_socket`, as upstream's `config::ipv6tun_socket`).
Here the node's own `show_status` already carries `tun_state`, `tun_name`,
`effective_ipv6_mtu` and `host_drops`, so nothing listens there and the
clients never need it. `21211`, between them, is `fipstop
--gateway-socket`'s default, upstream's placeholder for `fips-gateway`,
which exists on Linux only, so on Windows fipstop's Gateway tab reports it
not running. The native datagram API has no Windows endpoint at all (see
above): there is nothing to put on 21211 or elsewhere.

**Clients.** `fipsctl` and `fipstop` build on Windows; `-s <port>` (default
21210) names the port, an unparsable one prints upstream's `warning: invalid
port '<x>', using default 21210`, and a refused connection prints the system
message and `(os error <n>)` as Rust's `io::Error` does. `fipsctl keygen`
and `address` default to `C:\ProgramData\fips` (keygen refuses to write
through a reparse point), the hosts file to `C:\ProgramData\fips\hosts`.
fipstop selects the UCRT's UTF-8 locale and measures cells with a Basic
Multilingual Plane width table (Windows has no `wcwidth`); its golden frames
are byte-identical to the POSIX ones.

**Service.** `fipsd --install-service` registers the service `fipsd`
(display name `FIPS Mesh Network Daemon`, upstream's description) as
`"<this fipsd.exe>" --service`, automatic start, LocalSystem; run again from
another folder it repoints the registration and keeps the operator's start
type and account (upstream's `repoint_service`). `--uninstall-service` stops
it, waits for STOPPED on the SCM's own notification
(`NotifyServiceStatusChangeW`), and deletes it. Upstream names the service
`fips`; here it carries the daemon's name, as `fipsd.service` and
`com.fips.fipsd` do. Under the SCM (`src/service_win32.c`):

```
sc.exe start fipsd -> fipsd.exe --service -> StartServiceCtrlDispatcherW
  service main: RegisterServiceCtrlHandlerExW
    START_PENDING checkpoint 1, wait hint 30 s
    configuration loaded, runtime created: START_PENDING checkpoint 2
    every subsystem started: RUNNING, accepting STOP and SHUTDOWN
  STOP or SHUTDOWN (control handler, dispatcher thread):
    STOP_PENDING, wait hint = runtime grace (5 s) + 10 s; fipsd_runtime_shutdown()
    subsystems stop in reverse (the Wintun adapter, address and route go)
  STOPPED: exit code 0, or ERROR_SERVICE_SPECIFIC_ERROR with the daemon's code
```

INTERROGATE is answered; other controls are not implemented. The daemon body
is the console run's (`main.c` `run_daemon`) with console control events
off, since a service also receives `CTRL_LOGOFF_EVENT` whenever any user logs
off. A daemon that ends on its own (a fatal error) still goes through
STOP_PENDING to STOPPED with its exit code. Upstream reports RUNNING at once
and STOPPED at the end; the pending states, checkpoints and wait hints are
this port's, as the SCM documents them, and are pure data tested on every
host (`src/service.c`, `FipsdServiceStatusTest`). The configuration is `-c`,
else `FIPS_CONFIG`, else `C:\ProgramData\fips\fips.yaml`, the only directory
the service reads. The log is upstream's: the file
`C:\ProgramData\fips\fips.log` (or `--log-file`), rolled at
`node.log_max_size_mb`/`node.log_max_files` (10 MiB, 4); `node.log_file` is
ignored, and a log that cannot be opened leaves the service running without
one, as upstream. There is no event log source (upstream has none).

**Packaging** (`packaging/windows/`, upstream's scripts ported):

- `build-zip.ps1 -BuildDir build [-DllPath C:\msys64\clangarm64\bin]
  [-WintunZip <zip>] [-Arch arm64|amd64]` packages an MSYS2 build (it does not
  build) as `build\deploy\fipsd-<version>-windows-<arch>.zip`, flat:
  `fipsd.exe`, `fipsctl.exe`, `fipstop.exe` when ncurses was found, every
  non-system DLL they import (the import closure read with `llvm-objdump -p`
  through the build tree, `-DllPath` and `PATH`, never the Windows
  directory; for the CLANGARM64 build: jansson, ncursesw, nsync, secp256k1,
  sodium, winpthread, yaml), `wintun.dll` for the architecture from
  wintun.net's 0.14.1 zip, checked against the pinned SHA-256 and its
  WireGuard LLC Authenticode signature, with `wintun-LICENSE.txt`, the
  Windows example `fips.yaml` (no TCP transport and DNS off, which Windows
  does not build yet), `hosts`, the two service scripts and `README.txt`.
- `install-service.ps1` (Administrator, from the extracted ZIP; refuses while
  the service runs) copies the executables, DLLs and wintun.dll to
  `%ProgramFiles%\fips`; creates `%ProgramData%\fips` with an ACL of SYSTEM
  and Administrators only (upstream's hardening verbatim: created with the
  ACL in one step, links, foreign owners and sub-folders refused, existing
  files reowned and reset); seeds `fips.yaml`, `hosts` and empty
  `peers.allow`/`peers.deny` without overwriting; sets `FIPS_CONFIG` and adds
  the install directory to the machine `PATH`; and registers the service with
  `fipsd.exe --install-service`. Upstream's checks for files its earlier
  releases left in `\etc\fips` are omitted: fipsd never reads there.
- `uninstall-service.ps1 [-RemoveAll]` stops and deregisters the service and
  removes the install directory, the `PATH` entry and `FIPS_CONFIG`;
  `-RemoveAll` also removes `%ProgramData%\fips`.
- No firewall rule is added, as upstream: the control socket is loopback
  only. Windows treats each Wintun adapter as a Public network, so inbound
  traffic from the mesh (ICMPv6 echo, services) and, for peers that dial
  first, the UDP transport port need rules of the operator's choosing.
- All three scripts pass PSScriptAnalyzer with upstream's
  `PSScriptAnalyzerSettings.psd1`.

**Verification.** `FipsdControlTcpTest`/`FipsdControlTcpCapacityTest` (every
Windows build) drive the production listener and the production fipsctl
client on ephemeral ports: the protocol bounds, EOF rules, both deadlines,
no head-of-line blocking, the exclusive port, the connection cap, a stop
that cancels the pending accept and frees the port, the client's error
text, and `show_status` with and without the TUN keys.
`FipsdWindowsServiceSmokeTest` (`tests/windows_service_smoke.ps1`; skips 77
unless elevated with `FIPSD_SERVICE_PRIVILEGED_SMOKE=1`, refuses to run where
a fipsd service, `%ProgramFiles%\fips` or `%ProgramData%\fips` exists) is the
`nostrc-iq1n9.6` acceptance end to end: build-zip, install-service.ps1, the
ACL and registration, `Start-Service`, `fipsctl show status` over TCP 21210
reporting `running` with `tun_state` `active` on `fips0` and the `/128` on
the adapter, a second fipsd (a console process of the installed binary with
its own adapter and only the system directories on `PATH`) peering over UDP
on 127.0.0.1 and pinging the service node's fd00::/8 address from its own
(`ping -6 -S`, 3/3, each echo counted on both adapters), a session on the
service node, then Ctrl+Break and `Stop-Service` (STOPPED, exit code 0, a
clean shutdown in `fips.log`), `uninstall-service.ps1 -RemoveAll`, and no
service, adapter, route, firewall rule, directory, `FIPS_CONFIG` or `PATH`
entry left.

### Windows CI

`.github/workflows/fips-ci.yml` job `windows` (`windows-11-arm`, MSYS2
CLANGARM64 through `msys2/setup-msys2` with the packages of step 1):
libsecp256k1 built from a pinned tag into a cached prefix; PSScriptAnalyzer
over `packaging/windows`; the configure line of step 3 with no extra flag or
environment; the targets of step 3; wintun.dll fetched and checked as above
and placed beside `build/fipsd/` and `build/fipsd/tests/`; then `ctest -R
'^Go|^FipsCore|^FipsDiff|^Fipsd|^FipsTop'` with
`FIPSD_TUN_PRIVILEGED_SMOKE=1` and `FIPSD_SERVICE_PRIVILEGED_SMOKE=1` (hosted
Windows runners are elevated, so the privileged TUN, two-node and service
smokes run rather than skip), and the package as an artifact.

## Transport statistics and control parity

`transport/stats_common.h` owns the upstream `u64` counter names and relaxed
atomic storage shared by UDP, TCP and the test loopback. Packet/byte counts
record successful wire I/O (UDP receives include classifier/malformed discards;
TCP receives count complete frames). MTU rejection is `mtu_exceeded`, not a
successful send or a `send_errors` increment. TCP admission counts inbound
rejections only; `pool_inbound`/`pool_outbound` are current occupancy and return
to zero after join. UDP `kernel_drops` is the Linux wildcard socket's cumulative
`SO_RXQ_OVFL` value, not a sum of connected-socket or application drops; it stays
zero on macOS. Local classifier/framing/queue diagnostics remain separate.

`show_transports` uses the exact per-type upstream snapshot fields: eight for
UDP, fifteen for TCP, and `{}` for upstream's test loopback. All applicable zero
counters remain present; no Ethernet counters, aggregate `errors`/`dropped`, or
local diagnostics leak into UDP/TCP output. Transport rows are ordered by id,
use upstream lifecycle labels (`configured`, `up`, `down`, `failed`), and include
`local_addr` only when bound. An absent interface remains a started (`up`)
transport; presence and carrier are separate facts, and carrier never gates
binding. The `interface` object is omitted for transports without an interface.
Its seven upstream fields deliberately do not include OS index or detected
medium, which remain internal presence facts for path selection.

Carrier is refreshed through the existing presence/watcher notifications and
backstop, with **no additional polling**: Linux reads
`/sys/class/net/<interface>/carrier` (falling back to interface flags if unreadable),
and macOS uses `SIOCGIFMEDIA`'s valid/active bits with `IFF_RUNNING` as fallback.
Carrier-only changes update the diagnostic without tearing down the socket.
`test_control.c` cites the upstream snapshot sources; the empty/interface goldens
are verbatim upstream fixtures, and populated/zero UDP/TCP goldens derive their
layout from those fixtures and their fields from upstream `udp/stats.rs` and
`tcp/stats.rs`. Two-node UDP/TCP tests cover the real provider, and injected
presence tests pass carrier flips through the same projection and query path.
These initial-output parity corrections fold into **fipsd 0.1.0 Unreleased**.
## Packaging (`nostrc-iq1n9.5`)

`packaging/` mirrors upstream fips's `packaging/` for the C daemon: Debian
(`debian/`, a debhelper recipe for `fipsd`, `libfipsnative0` and
`libfipsnative-dev`), RPM (`rpm/fipsd.spec`), Arch (`arch/PKGBUILD`), the
systemd units (`systemd/*.service.in`), a macOS `.pkg` with a launchd daemon
(`macos/`), and the shared assets (`common/`: the example `fips.yaml`,
`hosts`, upstream's `fips.nft` baseline, the `fips` sysusers and `/run/fips`
tmpfiles fragments). Everything a package ships is declared by the install
rules at the end of `CMakeLists.txt` and staged with
`cmake --install <build>/fipsd`; `make -C fipsd/packaging deb|rpm|pkg` builds
each format and `.github/workflows/fips-packaging.yml` builds, installs and
smoke-tests all of them on a `fipsd-v*` tag or by dispatch. The version is
`declare_component_version(FIPSD ...)` here, read by
`packaging/common/version.sh` and tagged by `scripts/tag_release.py`;
`scripts/test-release-packaging.py` keeps the recipes in step with it.
Details, the install behaviour of each package and the smoke scripts are in
[packaging/README.md](packaging/README.md). The Windows ZIP and service scripts (`packaging/windows/`) are
described under "Windows service, control socket and packaging" above. The `fips-dns-setup`/`-teardown`
helpers behind `fips-dns.service` and `fipsctl` are packaged when the tree
has them (their own tasks); the unit is skipped, not failed, without them.

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
| control `snapshot.rs`/`queries.rs` rows | `provider.c` | 587 | Immutable snapshot arena (typed rows + JSON projections with upstream field names) published via GoSwap, sharing ref-counted stats-history views (`node/stats_view.c`); live commands enqueued to the dispatcher |
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
  `tun.name` selects the Linux and Windows names only.
- Windows: a Wintun adapter named `tun.name` and its IP Helper rows, with a
  receive thread and a doorbell socket as the descriptor; see "TUN on Windows
  (Wintun)" above. Its kernel calls are `fipsd_tun_ops.wintun`, and the
  platform moves frames itself (`fipsd_tun_platform.frame_read/frame_write`).

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
`host_drops` counts packets the shim dropped on a full host queue; failed/not-up devices
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
### IPv6 shim (`nostrc-9nxwk.2`)

`src/ipv6/` is the packet adapter between the TUN device's IPv6 frames and
the node's FSP sessions on port 256, ported from upstream `src/ipv6tun/`
(`ipv6_shim`, `inbound`, `outbound`, `queue`, `icmp`, `icmp_rate_limit`,
`ipv6_frag`, `exthdr`, `mss`, `tcp_mss`, `mtu`, `link`, `listening`,
`firewall_state`; `winnet.rs` and `dns.rs` are out of scope). `main.c` creates
one `fipsd_ipv6_shim` per enabled TUN and hands it to the TUN service, which
starts it once the device is up and stops it, cancelling (`gof_io_cancel_fd`)
and joining its fibers, before the device closes. Nothing in the shim blocks
the node's dispatcher: datagrams go in through the handlers' nonblocking
command mailbox (a full mailbox drops and counts, where upstream's reader
thread blocks on its channel) and the node's notices are read off their
bounded channel by the shim's own fiber.

| Module | Contract |
| --- | --- |
| `shim.{c,h}` | Format `0x00` header compression (addresses, version and payload length stripped; traffic class, flow label, next header, hop limit kept; packets travel 33 bytes shorter) and the adapter: the reader fiber (frames → `from_tun` → datagrams, answers, hairpins), the writer fiber (bounded host queue → frames, inbound SYNs clamped), the mesh-receiver fiber (port-256 datagrams delivered, path MTU changes and routable peers learned, every other notice passed through to an optional consumer or freed). |
| `outbound.{c,h}` | The host half as pure verdicts: not IPv6 → nothing; destination outside `fd00::/8` → Destination Unreachable; our own address → hairpin with the L4 checksum completed (macOS utun); otherwise the SYN clamp, then: a source other than ours → code 5 (source address failed policy), a prefix a key names → a datagram whatever its size, else Packet Too Big over the node-wide IPv6 MTU (1280 for a packet over 1280 on a path under it) or Destination Unreachable. `answer` turns a refusal (too big / table full / no route) into the answers owed; `can_split`/`split`/`reclamp` are the node-side steps. The C node holds datagrams opaque and has no refusal-with-payload path, so the shim's driver takes the node-side judgement itself with its replica of the node's limits before `fipsd_node_handlers_send`: a payload over the smaller of the node-wide limit and the stored path goes as fragments when it can, else the host is owed Packet Too Big. |
| `inbound.{c,h}` | A delivery rebuilt with the sender's `fd || NodeAddr[0..15]` as source and ours as destination, CE-marked only on an ECN-capable packet (the C node's delivery carries no CE mark yet). |
| `icmp.{c,h}`, `icmp_rate_limit.{c,h}` | RFC 4443 Destination Unreachable and Packet Too Big: never for a packet whose source is unspecified or multicast, whose destination is multicast, or that is (or cannot be shown not to be) an ICMPv6 error or Redirect behind any extension headers; a Packet Too Big's source is the packet's destination so the kernel honours it. Packet Too Big is limited per source by a token bucket (burst 10, one per 100 ms, sources dropped after 10 s or `max(10 s, 10 × interval)`; this host bounds the table at 256 sources, evicting the least recently checked). Destination Unreachable is limited by one bucket for the whole shim (burst 10, 10/s). A token is spent whether or not a queue is up. |
| `frag.{c,h}` | Ingress fragmentation below 1280: only a well-formed packet over the path MTU and at most 1280 bytes, with no Hop-by-Hop or Routing header, whose headers through a recognised upper layer (TCP, UDP, ICMPv6, ESP, No Next Header, behind Destination Options/AH) fit the first fragment, into chunks of at least 128 bytes (IPv6 MTU ≥ 176, so never more than ten); a host-made fragment is split further under its own identification, offset and final M flag. Reassembly is the destination host kernel's, as upstream. |
| `exthdr.{c,h}` | The RFC 8200 chain walk: steps over Hop-by-Hop, Routing, Destination Options, AH and a first Fragment header, bounds-checked; reports the upper-layer offset, whether AH/Fragment sealed it, a later fragment, or a truncated chain. |
| `tcp_mss.{c,h}`, `mss.{c,h}` | The SYN MSS clamp behind HBH/Routing/DestOpts (nothing taken off for them, RFC 9293 §3.7.1), refused behind AH/Fragment/ESP/unknown headers and for a zero ceiling, checksum updated incrementally (RFC 1624). Per-flow ceiling `min(global, tcp_mss(path limit))`, falling back to `min(global, 1143)` (the 1280 floor) when nothing is stored or the stored limit admits no payload byte; a locally seeded sub-floor limit is honoured. |
| `mtu.{c,h}` | `effective = transport − 77`, `mss_ceiling = effective − 60`, `ipv6_mtu(limit) = limit + 33`, `tcp_mss(limit)`; `_Static_assert`s pin 77 to `FIPS_OVERHEAD + port header − 33` and to the device's `FIPSD_TUN_IPV6_OVERHEAD`. |
| `queue.{c,h}` | The bounded queue toward the host (`node.buffers.host_channel`, default 4096): a send never waits; a full queue drops, counts (`host_drops` in `show_status`) and logs at the first drop and each doubling; only a reader that has gone is an error. |
| `link.{c,h}` | The link's message vocabulary (attached, synced, node MTU, path MTU, known/forgotten key, datagram, learned, refusal) and the shim's copy of the node's state: node-wide payload limit and MSS ceiling (atomics), the per-destination path table and the prefix → node-address key map (both mutex-protected, bounded). Peers are named by node address, which the C node's datagram API takes; upstream names them by x-only key. |
| `listening.{c,h}`, `firewall_state.{c,h}` | The fipstop "Listening on fips0" panel: `/proc/net/{tcp6,udp6}` listeners (kernel word order decoded) paired with their process via `/proc/<pid>/fd`, kept when bound to `::` or the TUN address; and the read-side classifier of `nft -j list table inet fips`'s `inbound` chain (accept / drop / unknown / no_firewall, the `iifname != "<tun>" return` scope, a foreign scope warned once at bring-up on Linux). It classifies what the kernel's firewall will do; connection state and timeouts are conntrack's. |

MTU and ICMP behaviour (upstream `docs/how-to/diagnose-mtu-issues.md`,
`docs/design/fips-mtu.md`): the shim's node-wide IPv6 MTU is the configured
transport budget less 77 (1357 → 1280, 1472 → 1395), seeded at creation; the C
node does not yet republish a bound-transport minimum at runtime. Per-destination
path MTU reaches the shim as a change-only `FIPSD_NOTICE_PATH_MTU` emitted by
the dataplane's clamp table (`fipsd_dp_mtu_set`/`release`: reactive
`MtuExceeded`, proactive `PathMtuNotification`, peer rebind, release on session
close), stored as a payload limit per `fd00::/8` address. The notice channel
is bounded and never blocks the dispatcher: a change it cannot take is counted
in `notices_dropped`, left pending on the clamp and re-sent at the next set
for that peer (each MMP notification applies one); a dropped release leaves
the smaller limit in place until then, which only fragments or answers Packet
Too Big more conservatively. In the daemon the shim is the notice channel's
only consumer; tests read what it passes through. A cold flow's first
SYN is clamped to the conservative 1143 until a path is stored; the shim does
not re-clamp a datagram the node holds while its session is set up. An
oversize packet to a known destination is answered Packet Too Big with the
path's IPv6 MTU, or 1280 when the packet exceeds 1280 on a path under it, since
a host ignores a smaller value; a packet of at most 1280 bytes on such a path
is fragmented instead. `tcpdump` on the device sees outbound SYNs before the
clamp and inbound SYNs after it. Keys the shim forwards to come from the
notices that prove a peer routable (link up, session up, a datagram) and from
`fipsd_ipv6_shim_learn` (the DNS responder's seam); nothing is forgotten on
link or session down, since the node's identity cache still routes to it. A
destination with no known key is answered Destination Unreachable at once; a
known destination the node cannot reach ends in a `SEND_FAILED` notice without
a payload, so no Unreachable is owed for it (upstream's `NoRoute` refusal
carries the payloads).

Tests (`fipsd_tests`, ASan/UBSan and TSan): `FipsdIpv6CodecTest`,
`FipsdIpv6IcmpTest` and `FipsdIpv6FirewallTest` port every upstream unit test
of the modules above as vector tests (MTU arithmetic at every `u16`, the MSS
clamp against rewrite-and-recompute over 7,000 SYNs, the Packet Too Big bucket
at exact instants on an injected nanosecond clock, every nft scope and
classification case). `FipsdIpv6TwoNodesTest` runs two nodes over the loopback
transport with fake devices injected through `fipsd_tun_ops` (a socketpair
stands in for the kernel) and proves, byte for byte: a ping-sized echo across
the mesh and back, Packet Too Big reporting 1280 for a 1400-byte packet, a
1280-byte packet on a payload-limit-1090 path carried as 1120+216-byte
fragments that reassemble, a SYN clamped to 1063 on the way out, a Hop-by-Hop
packet refused fragmentation (PTB 1123), an AH-sealed SYN left unclamped, a
truncated chain owed no answer, hairpin, off-mesh and unknown-key
Unreachables, the listening panel's accept/drop verdicts, and the ordered
stop. `FipsdIpv6PrivilegedSmokeTest` skips (77) unless `geteuid()==0` and
`FIPSD_TUN_PRIVILEGED_SMOKE=1`; then it brings one node up on a real device
with the shim, pings its own address and an unknown `fd00::/8` address, and
checks the shim read the kernel's packets and the kernel accepted its ICMPv6
Destination Unreachable:

```sh
sudo FIPSD_TUN_PRIVILEGED_SMOKE=1 FIPSD_WORKERS=2 ./fipsd/tests/fipsd_ipv6_privileged_test
```

On Windows the shim runs unchanged on the Wintun device; its privileged
smokes and the two-node ping/TCP proof on one host are in "TUN on Windows
(Wintun)".

A ping between two nodes on real devices needs two network namespaces (two
TUNs in one namespace would compete for the `fd00::/8` route):
`scripts/fipsd-ipv6-netns-smoke.sh` (Linux, root) starts two `fipsd` daemons
in namespaces joined by a veth pair over UDP, with each other as configured
peers and `tun.enabled: true`, pings across and runs `iperf3` when installed.

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
when those limits are declared, and no mandatory payment/PoW. An explicitly
incompatible document refuses the relay for the run and is logged. A failed
fetch is not a verdict: the relay stays out of the pool (never admitted
unverified) and is re-probed on its own 1-second-to-5-minute backoff ladder,
logging the failure and the later recovery once each; it joins the running pool
when a compatible document arrives, and an advert refetch treats it as offline
meanwhile. Startup fails only when every relay is refused. NIP-59 is client-side
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
and missing OK are failures. An advert `OK false` is classified by its NIP-01
prefix: `rate-limited:`, `error:` and `auth-required:` (or no OK at all) are
retried at that relay on its backoff ladder; `invalid:`, `blocked:`, `pow:`,
`duplicate:` and other verdicts are logged once and not retried until the
endpoint set changes. TCP/Tor listener bind and unbind (transport stop, rebind)
both invalidate the advert from the transport watcher, after the transport state
is published, so a stopped listener's endpoint leaves the republished advert.
This schedule republishes local state; it never queries relays for new messages.

#### Local advert endpoints (`nostrc-r4aeo`)

The endpoint set is upstream's `advert_transport_snapshot` +
`build_overlay_advert` over the transport registry, in registry order, with
each instance's `advertise_on_nostr` (`transports.<type>` or
`transports.<type>.<name>`; `outbound_only` UDP never advertises):

| Transport | Endpoint |
| --- | --- |
| UDP, `public: false` | `nat`, with `signalRelays` (`dm_relays`) and `stunServers` |
| UDP, `public: true` | `external_addr`; else a non-wildcard bound address; else the STUN-observed IP with the bound port; else omitted with upstream's warning |
| TCP | `external_addr`; else a non-wildcard listener address; else omitted with upstream's warning (no TCP STUN) |
| Tor (directory mode) | `<onion>:<advertised_port>` (default 443) once the onion is advertisable |

`external_addr` is parsed as upstream's `external_advert_addr`: a socket
address, or a bare IP taking the port of `bind_addr` (so TCP needs a
parseable `bind_addr`); hostnames, zone ids and leading-zero octets are not
accepted, and the text is Rust `SocketAddr` display. Publication then applies
`plan_publish`: only publicly usable endpoints remain (loopback, private,
link-local, CGNAT, documentation, port 0 and `localhost` are dropped), and
`signalRelays`/`stunServers` appear only beside `udp:nat`, which requires both
non-empty. A UDP transport contributes once its bound socket finished the STUN
advert round; TCP and Tor once bound. Tor deliberately diverges in one way:
with a control monitor (`control_addr` in directory mode) the onion waits until
Tor reports the descriptor published, `650 HS_DESC UPLOADED <service>` (the
monitor adds `HS_DESC` to `SETEVENTS`), or until `GETINFO
hs/service/desc/id/<service>` at (re)subscription shows Tor already holds a
current descriptor (fipsd restarted under a running tor). Without a monitor,
or when the monitor never completes a first session, the hostname file read at
start suffices, as upstream. The latch is set once; other onions, other
actions and further HSDir reports change nothing.

Republication is event-driven and change-only. The driver's watcher fiber
turns advertised TCP/Tor listener binds and the Tor latch into
`fipsd_nostr_runtime_refresh_advert`, as UDP observation already did. The
actor rebuilds and compares the canonical content with the last built advert:
a changed set is republished to every live relay; an unchanged one is sent
nowhere, except to a relay that just reached EOSE and by the refresh ticker
before NIP-40 expiry. An empty set publishes nothing; the old advert ages out.
No timer polls transports. Each new content gets a `created_at` strictly after
its predecessor's (at most a second ahead of the clock, within peers' skew
clamp): NIP-01 relays and peer caches keep the lowest id among equal-second
replaceable events, so a set changed within the same second would otherwise
be dropped as a "duplicate".

`FipsdNostrAdvertTest` runs this through the real registry, driver and actor
against two libsoup relays and a fake control port: `external_addr` overrides
a loopback bind, `advertise_on_nostr: false` and loopback-only TCP are omitted,
the onion appears only after `HS_DESC UPLOADED` with exactly one republish per
relay at a later `created_at`, and refreshes and further HSDir reports republish nothing (barriers:
publish OKs, a refetch EOSE behind earlier commands, control command lines).
`FipsdNostrEventsTest` checks upstream's advert content strings and filter,
precedence and `external_addr` rows; `FipsdTorTest` the latch.
`FipsdAdvertTorInteropTest` (`tests/advert_interop.py`, opt-in like
`FipsdTorInteropTest`) has the pinned Rust daemon discover fipsd from the
advert alone: fipsd advertises only its onion on a local stdlib relay
(`Testing/fips-chaos/services/relay.py`), starts before tor has a descriptor,
and the Rust peer is configured with only fipsd's npub (`via_nostr: true`).
Recorded on macOS over three runs (tor 0.4.9.17, Rust rev 6aa80504ba, ASan
fipsd): hostname 0.4-0.6 s, descriptor uploaded and fipsd's onion advertisable
(`HS_DESC UPLOADED`) both at 10.7-11.8 s, the advert stored at the relay in the
same tenth of a second, Rust discover-and-link 14.7-16.7 s after its start, one
advert publish, no SOCKS5 errors or connect timeouts, 26.9-27.9 s in all.

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

**Traversal and STUN (`8hvs6.3`).** `driver.c` attaches `traversal_io` through
prepare/start/stop/destroy callbacks on the existing rendezvous subsystem; there
is no second relay runtime or `main.c` registration. Its bounded command/packet
mailboxes feed one tracked fiber. The relay actor still owns authenticated
signal admission, publish OK, replay memory and failure/refetch policy. The
worker owns STUN transactions, candidate plans and pure traversal machines.
Classifiers register before UDP starts and borrow their owner through runtime
join. Failed construction removes registrations before freeing that owner.

`stun.{c,h}` implements the upstream 20-byte binding request (cookie
`0x2112a442`, 96-bit CSPRNG transaction id), MAPPED-ADDRESS and
XOR-MAPPED-ADDRESS for IPv4/IPv6, unknown-attribute padding and bounded decoding.
A reply must match both the resolved server endpoint and transaction id. Declared
lengths/truncation fail closed. The UDP transport resolves the configured server
using its family-aware DNS cache/blocking executor; every send uses its existing
bound listener. There is no private STUN socket and no competing receive drain.
The same id is retransmitted at 0/500/1500 ms for the upstream 2-second traversal
budget, plus 3500 ms for its 5-second advert budget. Production observation uses
the advert budget once per bound transport generation, then shares that mapping
with offers/answers; a rebind triggers a new observation. Servers are tried in
configured order, never taken from a peer's untrusted STUN hint. Exhaustion
leaves only opt-in local candidates and releases otherwise unusable pending
attempts. Neither a timeout nor a failed send establishes a mapped address.

Reflexive observations populate offers/answers. Adverts follow upstream (see
Local advert endpoints): `public: false` UDP publishes `udp:nat` with the
configured signal/STUN hints; only `public: true` publishes a concrete
address. Observation completion queues a local advert refresh, not a relay query.
`share_local_candidates: false` is the default. When enabled, interface addresses
of the bound family/port are collected on the blocking executor. Candidate
planning rejects zero ports, wrong families/protocols, multicast, unspecified,
link-local and CGNAT destinations; private destinations must match an actual
local interface prefix. Loopback is allowed only on an explicitly loopback-bound
transport (the local integration fixture uses this). At most 32 offered local
candidates are vetted and eight distinct targets are punched; local candidates
precede the reflexive candidate.

The UDP classifier registry now has a source-aware callback alongside its
unchanged legacy callback. STUN/punch datagrams are copied into a bounded mailbox;
ordinary FMP stays on the normal receiver path. `traversal.c` encodes upstream
24-byte `NPTC`/`NPTA` packets: four-byte magic, big-endian sequence, and the first
16 bytes of SHA-256(session id). Reserved prefixes remain discarded even with no
rendezvous owner. Only matching-session packets from a planned IP are actionable:
an exact endpoint wins immediately; a remapped source port waits up to 250 ms
for an exact match. Unplanned sources are neither acknowledged nor adopted.
The hash is a discriminator, **not authentication**.

`fipsd_established_traversal` carries identity, session and the existing local/
remote UDP tuples to `fipsd_nostr_node_driver_established`. It rechecks the local
tuple and enqueues identity learning plus the existing node connect command.
Noise IK still authenticates the peer; only the existing `udp.authenticated`
callback promotes it to a connected per-peer socket. No unauthenticated punch
allocates a connected peer. Late probes on the selected tuple are acknowledged
until the original attempt deadline, without repeating the handoff.

#### Pure traversal transition table

`traversal_machine` takes explicit monotonic milliseconds, never reads a clock or
performs I/O. Unlisted state/input combinations are inert; cancel terminates any
nonterminal state. Upstream `traversal_machine.rs` contains cross-session
initiator election/replay decisions, while `runtime.rs`/`traversal.rs` own the
async session loop: this table is the C sans-I/O expression of those decisions,
not a claim that upstream has an identical enum/table.

| State | Input/guard | Next state | Effects |
| --- | --- | --- | --- |
| Idle | outbound offer | Await answer | Send offer, answer deadline |
| Idle | incoming offer | Punching | Send answer, punch deadlines |
| Await answer | accepted answer + usable plan | Punching | Arm local punch cadence |
| Await answer | rejection / answer deadline | Cooldown | Failure, release reservation |
| Punching / Settling | initial publish failure / rejected plan | Cooldown | Failure, release reservation |
| Punching / Settling | late repair-publish failure | Unchanged | Answer already consumed reservation; ignore obsolete repair |
| Punching | cadence due, before send/end deadline | Punching | Send probes to plan |
| Punching | matching punch, exact source | Established | ACK probe, handoff once |
| Punching | matching punch, remapped port | Settling | ACK probe, arm 250 ms settle |
| Settling | another remapped source | Settling | ACK probe, update candidate; keep first settle deadline |
| Settling | exact source / settle deadline | Established | ACK probe if applicable, handoff once |
| Settling | cadence due | Settling | Send probes to plan |
| Punching / Settling | send window elapsed | Unchanged | Disarm cadence; wait for settle/attempt deadline |
| Punching | attempt deadline | Cooldown | Failure, release reservation |
| Established | matching selected-source probe | Established | ACK only |
| Established | original attempt deadline | Idle | Release retained punch state |
| Cooldown | deadline | Idle | Retry outbound only while budget remains |
| Any nonterminal | cancel / binding replacement | Cancelled | Release reservation |

One active attempt per canonical peer is retained. When both peers initiate,
the smaller NodeAddr's session wins; one successful offer re-send can repair an
offer published before the peer subscribed, but only while awaiting an answer.
Repair results are distinguished from initial publish results so an accepted
answer overtaking a queued repair cannot cancel a healthy punch. Rejected plans
include the nonempty reason required by the answer validator. Existing runtime
session replay memory is not evicted to make room. Defaults match upstream: 120 s answer wait,
10 s attempt, 2 s punch delay, 200 ms interval (minimum 20 ms), 10 s send window;
`node.retry` is 5 retries, 5 s exponential base, 300 s cap. Five failures apply
an 1800 s extended cooldown, also reported to the existing failure/refetch owner.
An exhausted retry budget does not create a periodic relay fetch.

#### Configuration and local evidence

All keys below already belong to the configuration schema; no new YAML keys:

- `node.rendezvous.nostr.stun_servers`: at most eight `stun:host:port` or
  `host:port` entries; IPv6 hosts use brackets. Default servers remain unchanged.
- `share_local_candidates`, `signal_ttl_secs`, `attempt_timeout_secs`,
  `punch_start_delay_ms`, `punch_interval_ms`, `punch_duration_ms`,
  `failure_streak_threshold`, `extended_cooldown_secs`: behavior/defaults above.
- `open_discovery_max_pending` and `max_concurrent_incoming_offers` reserve
  separate outbound/inbound worker quotas (nonzero sum, host ceiling 4096).
  Retained inbound cooldown entries count against the inbound quota, never the
  outbound reserve; existing runtime per-sender and replay limits still apply.
- `transports.{udp,tcp,tor}[.<name>].advertise_on_nostr`: opt-in per transport;
  `node.rendezvous.nostr.advertise` still gates publication globally. UDP
  `public` and `external_addr`, TCP `external_addr` and Tor `advertised_port`
  shape the endpoint (Local advert endpoints).
- `node.retry.{max_retries,base_interval_secs,max_backoff_secs}`: bounded retry
  policy; the base and attempt/answer lifetimes must be nonzero.

`FipsdNostrTraversal_{codecs,states,stun,traverse,guards}` are in `fipsd_tests` and link
the daemon implementation. They cover fixed upstream-layout IPv4/IPv6 vectors,
all transition rows/cancellation/inert inputs/cooldown, reserved punch versus
FMP classification, a real loopback STUN server (success, first-request loss,
bounded give-up, exact source and unchanged transaction/local port), and two
real nodes discovering adverts and exchanging offer/answer through
`nostr_relay_fixture` before punch → IK → connected UDP. No sleep-based waits or
mocked successful handoffs are used. Regression coverage also checks a valid
negative answer through the relay, outbound admission with a retained inbound
cooldown, late repair failure after answer/settle, and a scheduler jump beyond
the last send window. The no-deadline path calls `go_select`, not
an overflowing `go_select_timeout(UINT64_MAX)` pseudo-infinite wait. Concurrent
relay creation uses `pthread_once` for libnostr's cached debug flag. The TUN
runtime fixture reads errno through a post-resume accessor, matching the TUN
implementation's fiber-migration contract; its error assertions remain intact.

Local macOS/arm64 validation built the daemon and test targets with
warnings-as-errors, including the separately selected `fips_core_skeleton_test`.
Both complete `ctest -R '^Fips' --output-on-failure` runs (ASan+UBSan and TSan,
halt-on-error enabled) selected 144 tests: 141 passed, zero failed, three skipped.
The skips are `FipsdControlInteropTest` (upstream control checkout not configured),
`FipsdTunPrivilegedSmokeTest` (not root), and `FipsdUdpInteropSmokeTest` (pinned
Rust daemon not configured). All five traversal scenarios passed in both builds.
The direct unsequenced-arguments, GUI GDK-frame, Linux CI-package and
`git diff --check` static gates passed; no Beads or pre-push wrapper was run.

This is C↔C loopback proof, **not NAT or Rust interoperability proof**. Upstream's
NAT lab uses explicit namespace/veth SNAT+DNAT for cone emulation, randomized
MASQUERADE for symmetric NAT, and a same-LAN case. Those network topologies,
C↔Rust traversal, public STUN/relays and deployment are not verified here;
`f66sr.3` owns the formal simulated-NAT acceptance harness. The formal protocol review
(`8hvs6.4`) was completed 2026-11-16: docs/reviews/fips-nostr-protocol-review-2026-11-16.md.

### Protocol-smell checklist (A–J, module and local relay tests)

The formal Nostr protocol review (`8hvs6.4`) was completed 2026-11-16 — see
[docs/reviews/fips-nostr-protocol-review-2026-11-16.md](../docs/reviews/fips-nostr-protocol-review-2026-11-16.md).
It found no must-fix A–J protocol smell introduced by this module: every box below was verified
against source and the upstream Rust runtime, and the three nostr test binaries build and pass
out-of-tree under ASan/UBSan with warnings-as-errors. The report records one Medium pre-existing
**libnostr** concurrency defect — a flaky TSan data race in the relay connect→write handoff
(`relay.c:967`/`:1064`) that the rendezvous connect/retry churn exposes under load, which makes the
TSan gate not reliably green and is outside the CLOSED-backoff/AUTH code in scope — plus a Low
NIP-11 one-shot-probe follow-up (item H) and two informational notes (NOTICE surfacing; a
documented resend-in-AwaitAnswer divergence).

- [x] A: Long-lived REQ/event delivery; no periodic relay queries.
- [x] B: History and successful fetch completion are EOSE-driven, never timer-closes.
- [x] C: Publish OK/false, CLOSED reasons/backoff and NIP-42 challenge/OK are observed; initial and repair publish results stay distinct across accepted-answer races.
- [x] D: Explicit history-cache → EOSE → live effects; recovery uses a bounded freshness window; refetch uses until/limit.
- [x] E: Kind/identifier/authors and kind/recipient/since filters; wildcard authors only for explicitly open discovery.
- [x] F: Independent bounded signal IDs/session memory and replaceable advert idempotency, including reconnect replay; traversal adds canonical peer dedup, dual-init election, one-shot handoff and independent retained inbound/outbound quotas.
- [x] G: Channels are owned actor/worker mailboxes, not relay polling/queue wrappers; UDP classifiers never block the receive drain.
- [x] H: Every configured relay gets a cancellable NIP-11 probe; explicit refusal, multiple relays and authenticated retry. A failed fetch is re-probed on the relay's backoff ladder and the relay joins once a compatible document arrives (`nostrc-tukt2`, review Finding 2).
- [x] I: Timers are only advert publication, reconnect backoff, reservation expiry, failed-fetch deadlines, STUN retransmissions, bounded UDP punch cadence/settle (expired send windows disarm) and failure cooldown; none substitutes for Nostr event delivery or establishes history completion.
- [x] J: Grotto `bunker_test_relay` main-context/Soup fixture adapted to Jansson over real local WebSockets/HTTP, driven by main-context commands and channel barriers, no sleeps. Tests hold EOSE, replay duplicate history/live events across two relays, send AUTH with CLOSED, reject publishes, and exercise B6/session/freshness/failure contracts. Traversal adds a real loopback STUN server and relay-mediated two-node IK, with channel/select deadlines rather than sleeps, plus rejection/quota and late-repair/expired-cadence regressions.

The fixture uses only libsoup and Jansson already present in the FIPS CI package
lists; it does not require JSON-GLib. The older Testing mock server has startup
sleeps/volatile service polling and was not used for these event-driven gates.

The upstream open-discovery startup scenario seeds adverts before node startup;
the local test preserves that shape and additionally holds EOSE before any dial
effect is permitted. These are local native tests, not public-relay, hosted-CI,
STUN, traversal or deployment proof. The formal protocol review (`8hvs6.4`) was completed
2026-11-16: docs/reviews/fips-nostr-protocol-review-2026-11-16.md.

## mDNS LAN rendezvous (`nostrc-9nxwk.5`)

Peer rendezvous on the local link over mDNS / DNS-SD (RFC 6762 / RFC 6763): the
port of upstream `src/mdns/mod.rs` (an `mdns-sd` responder and browser) and the
LAN half of `src/node/lifecycle/mod.rs` and `src/node/peering/lan.rs`. It is a
self-contained implementation of the needed protocol subset on two multicast
UDP sockets under libgo fiber I/O. It adds no runtime dependency: no Avahi, no
`dns_sd`, no mDNSResponder client. The sockets share port 5353 with the
system responder through `SO_REUSEADDR`/`SO_REUSEPORT`.

Off by default, as upstream. With `node.rendezvous.lan.enabled: true` the
daemon publishes `_fips._udp.local.` on every up, non-loopback interface and
browses for the same service type. Two nodes on one broadcast domain with **no
configured peers** discover each other, dial the advertised endpoint and
authenticate it with the ordinary Noise IK link handshake, usually within a
second of starting. The advert is an unauthenticated routing hint: a spoofed
advert carrying another node's npub fails the handshake and is dropped. Like
upstream, the advert carries a subset of what `nostr.advertise` already
publishes, and it never leaves the link unless the LAN bridges mDNS.

### Configuration

| Key | Default | Meaning |
| --- | --- | --- |
| `node.rendezvous.lan.enabled` | `false` | Master switch (main.c registers the `mdns` subsystem only when true). |
| `node.rendezvous.lan.service_type` | `"_fips._udp.local."` | DNS-SD service type; must end in `.local.`. Override only to isolate test runs. |
| `node.rendezvous.lan.scope` | unset | LAN-only scope, carried as TXT `scope=`. Browsers with a scope surface only adverts with the same scope. When unset, the trimmed `node.rendezvous.nostr.app` is used, without any `fips-overlay-v1:` prefix. The default app `fips-overlay-v1` therefore gives scope `fips-overlay-v1`, as upstream. |

The legacy `node.discovery.lan.*` block still maps to `node.rendezvous.lan.*`
(config/normalize.c). The SRV record names the bound port of the lowest-id UDP
transport. With no UDP transport configured the subsystem logs a warning and
does not start. If the sockets cannot be opened it also warns and leaves the
host up, as upstream does when its daemon fails to start.

### Records

The instance and host label is `fips-` plus the first 16 characters of the
npub. Each node publishes:

- PTR `<service> -> fips-<npub16>.<service>`, TTL 4500 s;
- SRV naming `fips-<npub16>.local.` and the UDP port, TTL 120 s;
- TXT `npub=<npub>`, `scope=<scope>` when there is one, and `v=1`, TTL 4500 s;
- A/AAAA records for the host, TTL 120 s.

The TTLs are mdns-sd's defaults. Every record except the PTR carries the
cache-flush bit.

### Module map (`src/mdns/`)

| File | Role |
| --- | --- |
| `records.{h,c}` | mDNS codec. The decoder follows compression pointers (strictly backwards, so loops are impossible). The writer emits PTR/SRV/TXT/A/AAAA with the cache-flush and QU bits. It shares `dns/msg.h`'s constants; the `.fips` screen deliberately refuses pointers, so it is not reused for parsing. |
| `advert.{h,c}` | Upstream's pure half: `advert_label_of`, `host_is_advertisers` (any case, `-N` conflict suffix), `check_advert` (no-npub/own in any case/cross-scope/foreign-host/zero-port), `socket_addr_from_scoped_ip`, `arrival_interfaces`, the skip log lines, `lan_rendezvous_scope`. |
| `browser.{h,c}` | Sans-IO record cache for one service type: resolution, dedupe, TTL, goodbye, cache-flush and continuous querying. |
| `responder.{h,c}` | Sans-IO responder: probing, announcing, answering, conflict handling and goodbyes. |
| `lan.{h,c}` | Upstream `PendingLan`, `KnownLabels` and `lan_target_on_link`. |
| `socket.{h,c}` | Default I/O: IPv4 `0.0.0.0:5353` and IPv6 `[::]:5353` (V6ONLY). Multicast TTL/hops 255, multicast loop on, arrival interface from `IP_PKTINFO`/`IP_RECVIF`/`IPV6_PKTINFO`, `IP_MULTICAST_ALL` off on Linux. Membership and the outgoing interface are set per interface. |
| `mdns.{h,c}` | The `mdns` subsystem and its engine fiber. |

The only change outside `src/mdns/` is an additive `fipsd_udp_watch_bound`. The
transport's existing bound channel already belongs to the Nostr traversal
owner, so mDNS gets its own bind observer.

### Protocol subset

These are the parts of RFC 6762/6763 mdns-sd provides that LAN rendezvous needs.

**Responder**

- **Probing and announcing (§8).** Each interface waits a random 0-250 ms, then
  sends three probes 250 ms apart. The first probe has QU questions and every
  probe carries our records as authority. Two announcements follow, one second
  apart.
- **Conflicts.** A response claiming one of our names with data that is not ours
  on any interface renames us while probing (`<label>-2`, `-3`, …). After the
  names are won it resets that interface to probing (§9). After fifteen
  conflicts in ten seconds the responder waits five seconds.
- **Simultaneous probes (§8.2).** These are settled by the lexicographic
  comparison; the loser defers one second.
- **Answering.** Queries are answered only on the interface they arrived on and
  only after it has won its names. Known-answer suppression applies (§7.1). A
  legacy query (source port not 5353) gets a unicast reply echoing the ID and
  questions, TTL at most 10 s, no cache-flush (§6.7). A QU question is answered
  by unicast if the record was multicast within a quarter of its TTL (§5.4).
- **Timing and rate limits (§6).** Unique-only answers go at once; answers
  carrying the shared PTR wait 20-120 ms. No record is multicast on an
  interface more than once a second, or every 250 ms when defending against a
  probe.
- **Additional records.** A PTR answer carries SRV, TXT and the addresses as
  additional records; an SRV answer carries the addresses (RFC 6763 §12). The
  `_services._dns-sd._udp.local.` meta-query is answered.
- **Changes and shutdown.** An address change re-announces the interface,
  including goodbyes for the lost addresses. A port change re-announces every
  interface. Shutdown sends TTL-0 goodbyes for every record (§10.1).

**Browser**

- **Caching.** It caches only the service's PTRs, its instances' SRV/TXT records
  and the addresses of the SRV hosts. It never caches probe authority records or
  a query's known answers. Responses not sent from port 5353 are ignored (§6).
- **Lifetimes.** A record lives for its TTL. A goodbye takes it out of
  resolution at once and out of the cache one second later. A cache-flush record
  expires older records of its name and type after one second (§10.1/§10.2).
- **Continuous querying (§5.2).** The first query goes 20-120 ms after start,
  then at intervals doubling to one hour. Queries carry known answers with over
  half their TTL left. Records are refreshed at 80/85/90/95% of TTL plus up to
  2% jitter. An instance missing its SRV, TXT or addresses is asked for them
  at once, then 1 s and 3 s later.
- **Reporting.** A resolved instance (SRV, TXT and at least one address) is
  reported once per change, so repeated identical announcements are
  suppressed. An instance that stops resolving is reported as removed.

**Not implemented.** NSEC negative responses, truncation/TC continuation,
mDNS proxying and the reverse-mapping PTRs. None of these are needed to find
and dial a FIPS peer. Loopback interfaces are not used: upstream seeds
`127.0.0.1`, but mdns-sd sends an interface only the addresses in that
interface's subnets and does not run on loopback, so the seed is never sent
there either.

### Interfaces and link-local rules

These follow upstream and its `if-addrs` link-local handling.

- **Announced.** On each up, non-loopback interface with an address, the node
  announces only that interface's own IPv4 and IPv6 addresses, IPv6 link-local
  ones included. The interface is used as long as at least one family joins its
  group; utun/awdl-style interfaces join over IPv6 only, as with mdns-sd. This
  differs from mdns-sd in one way: mdns-sd filters by subnet, so other
  interfaces' `fe80::/64` addresses would leak onto an interface. Here they do
  not.
- **Arrival interfaces.** Every address record keeps its arrival interface. An
  IPv4 record collects every interface it was seen on. An IPv6 record is keyed by
  its arrival interface, which becomes its scope id. A scope-less IPv6
  link-local address is skipped, as upstream's `socket_addr_from_scoped_ip`
  does.
- **Dialled.** An address is dialled only when upstream's `lan_target_on_link`
  passes. An IPv6 link-local address passes when it carries a scope. Any other
  address must lie inside a prefix held by one of the interfaces its record
  arrived on, judged against the subsystem's own netmon snapshot (loopback and
  down interfaces excluded, as upstream's `read_system`). When the interface
  read fails, nothing is dropped or dialled until the next read.
- **Forged labels.** An advert whose host label is a configured or connected
  peer's under another npub is refused (`KnownLabels`).
- **Transport.** The dial goes over the lowest-id bound UDP transport of the
  address's family; IPv6 UDP sockets are V6ONLY, so families must match.

### Dial path and bounds

Accepted adverts become pending pairs held between dials. The bounds are
upstream's: 64 pairs, 4 addresses per node, 10 minutes, 8 unconfigured
handshakes in flight and 2 per IP address. Pairs are dialled configured first,
then shuffled. The dial uses the path the Nostr rendezvous uses:
`fipsd_node_handlers_learn_identity` and then `fipsd_node_handlers_connect`.
The node's `hs_dial` makes a dial to a connected or connecting peer a no-op.
Connected peers and in-flight handshakes are read from the dispatcher's
published control snapshot (peers, connections). A dial counts as over when the
peer connects, when the snapshot published a second after the dial no longer
lists its leg, or when the handshake timeout passes.

Not ported: upstream's multipath intake of a linked peer's advert as a path
candidate (`Feed::Mdns`).

### Runtime model

One engine fiber owns all state. It selects over four inputs: received
datagrams (two receive fibers hand them over a bounded channel), its own
`node/netmon.h` monitor (kernel notifications, with that module's backstop
read), UDP bind notifications, and stop/cancellation. The select timeout is the
nearest protocol deadline: probe, announcement, delayed answer, query, record
expiry, dial retry or pair expiry. Nothing runs on its own sampling timer. The
dial retry re-evaluates held pairs one second later, and only while some are
held (upstream does this on its node tick). Packets the node sent are
recognised by hash when they loop back, so its own announcements never look
like conflicts. The browser still reports them, and `check_advert` skips them as
its own. Stop sends the goodbyes, leaves the groups, cancels the receive fibers
(`gof_io_cancel_fd`), waits for them and closes the sockets.

### Verification

- `FipsdMdnsTest`: codec vectors; every upstream `src/mdns/tests.rs` case that
  needs no daemon; browser tables (TTL expiry, goodbye, cache flush, duplicate
  suppression, query schedule, known answers, refresh, own-announcement
  skipped); responder tables (probe/announce timing, aggregation delay, rate
  limit, known-answer suppression, legacy and QU unicast, re-announce with
  goodbye, conflict rename, tiebreak); the LAN candidate set.
- `FipsdMdnsTwoNodesTest`: four nodes on real UDP on 127.0.0.1 with **no
  configured peers**, whose mDNS engines share an in-memory multicast link (the
  `fipsd_mdns_io` seam):
  - A and B discover each other (scope, port, arrival interface) and establish
    an IK link both ways.
  - C (another scope) is skipped as cross-scope in both directions.
  - D (another service type) neither sees nor is seen.

  Absence is proven with sentinel adverts behind the FIFO receive queue, not by
  waiting. The seam replaces only the multicast sockets, because two engines in
  one process on real multicast depend on the host's loopback behaviour
  (upstream `#[ignore]`s that test for the same reason).
- Real sockets, by hand:

  ```sh
  # Two daemons on one host, or one per network namespace on a shared bridge,
  # each with node.rendezvous.lan.enabled: true, its own UDP port and no peers.
  fipsd --config a.yaml & fipsd --config b.yaml &
  # Each logs `lan: dialling mDNS-discovered peer` and `link established`;
  # `fipsctl show peers` lists the other.
  ```

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
add a relay actor; with `advertise_on_nostr` its `external_addr` or non-wildcard
listener address joins the node's Nostr advert (Local advert endpoints).
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
## Tor and Nym transports (`nostrc-gfi8h.4`)

Ports of upstream `src/transport/{socks5,tor,nym}` at the pin. Both are
stream owners layered on the TCP owner above, not copies of it: the inner TCP
instance (same id and name) keeps the pool, framing, writer queue, receive
ceiling, deadlines and teardown, and a host-only dialer hook in `tcp/conn.c`
dials the SOCKS5 proxy instead of the peer and then runs the handshake on the
connected socket under the same connect deadline.

### SOCKS5 client (`src/transport/socks5/`)

RFC 1928 CONNECT with no-auth or RFC 1929 username/password, IPv4, IPv6 and
domain (`.onion` included) targets, on a libgo fiber with one absolute
deadline over every read and write. The bytes are those tokio-socks 0.5.3
writes (`05 01 00` / `05 02 00 02` greetings, `01 ulen user plen pass`,
`05 01 00 atyp addr port`), and replies are checked in its order: version,
reserved byte, REP, then BND address type. Results keep tokio-socks' classes
(REP `0x05` refused, any other REP rejected, method `0xff`, auth failure,
protocol, I/O, timeout); owner-directed `gof_io_cancel_fd` ends a parked
handshake. Once a proxy address accepts the TCP connect its answer is final:
no other proxy address is tried, as in tokio-socks.

### Tor (`src/transport/tor/`)

```yaml
transports:
  tor:
    mode: directory                  # socks5 | control_port | directory
    socks5_addr: "127.0.0.1:9050"
    connect_timeout_ms: 120000
    mtu: 1400
    control_addr: "/run/tor/control" # optional in directory mode; default in control_port mode
    control_auth: "cookie"           # cookie | cookie:/path | password:secret
    cookie_path: "/var/run/tor/control.authcookie"
    max_inbound_connections: 64
    advertised_port: 8443
    directory_service:
      hostname_file: "/var/lib/tor/fips/hostname"
      bind_addr: "127.0.0.1:8444"
```

Operator flow (upstream `docs/how-to/deploy-tor-onion.md`, unchanged for the
C daemon):

1. In `torrc`: `HiddenServiceDir /var/lib/tor/fips` and
   `HiddenServicePort 8443 127.0.0.1:8444`; reload Tor and read
   `/var/lib/tor/fips/hostname`. For monitoring also enable a control port
   (`ControlSocket /run/tor/control` or `ControlPort 9051`) with
   `CookieAuthentication 1` or `HashedControlPassword`.
2. In `fips.yaml`: `transports.tor` as above, `bind_addr` equal to the
   `HiddenServicePort` target, `hostname_file` equal to `HiddenServiceDir` plus
   `/hostname`, `advertised_port` equal to the virtual port.
3. Restart fipsd: it reads the onion name, binds the listener, logs
   `directory-mode onion service active`, and `fipsctl show transports` shows
   `tor_mode`, `onion_address` and, with a control port, `tor_monitoring`.
4. Peers dial `<onion>.onion:8443` with transport `tor` (a `peers[]` address or
   `fipsctl connect <npub> <onion>.onion:8443 tor`).

Behaviour, as upstream: `socks5` mode is outbound only and ignores
`control_addr`; `control_port` mode adds the monitor and fails to start
without it; `directory` mode adds the onion listener (the only inbound
mode) and an optional, non-fatal monitor when `control_addr` is set; a
`directory_service` under `socks5` mode is refused. Every dial authenticates
as `fips` with the peer address as password, so Tor's `IsolateSOCKSAuth`
gives each destination its own circuit. Hostnames must be fully qualified;
`.onion` and clearnet names are resolved by Tor. REP `0x05` counts
`connect_refused`; every other proxy failure, an unreachable proxy included,
counts `socks5_errors`; the deadline counts `connect_timeouts`. Inbound onion
connections take a `max_inbound_connections` slot (all arrive from the local
Tor, so the per-source cap equals it) and keep TCP's first-frame and idle
deadlines. Links stay up between ends configured with different `mtu`
values (upstream 2026-11-12, `tor/tests/mtu.rs`): the send bound is the
local `mtu`, every receive is bounded by the 65535 ceiling the TCP owner
already uses. `show_transports` reports the 16 `TorStatsSnapshot` counters,
no `local_addr`, and upstream's Tor-only fields.

Upstream never creates onion services over the control port (no
`ADD_ONION`, which keeps Tor's `Sandbox 1` usable), and neither does this
port. The monitor differs from upstream's 10-second GETINFO poll by design:
one fiber per control connection authenticates (cookie hex or quoted
password), takes upstream's snapshot (`status/bootstrap-phase`,
`status/circuit-established`, `traffic/read|written`, `network-liveness`,
`version`, `dormant`; one GETINFO per key when Tor refuses the batch, a
refused key keeping its default), then `SETEVENTS STATUS_CLIENT
NETWORK_LIVENESS BW` and follows Tor's `650` events: bootstrap progress,
circuit (re)established, liveness, bandwidth totals. Status events refresh
the snapshot; nothing polls. The only timer is the one-shot 60-second
bootstrap-stall warning. A lost or refused control connection counts
`control_errors` and reconnects with backoff (1 s doubling to 30 s).

### Nym (`src/transport/nym/`, `-DFIPSD_ENABLE_NYM=ON`)

Built only with `-DFIPSD_ENABLE_NYM=ON` (default OFF); a daemon built without
it refuses a configured `transports.nym` at start rather than ignoring it.
Upstream's Nym is the "mixnet as proxy" pattern: outbound-only streams through
a local `nym-socks5-client`'s SOCKS5 listener with no authentication. It has
no websocket client protocol of its own, and this port has none. Keys:
`socks5_addr` (default `127.0.0.1:1080`), `connect_timeout_ms` (300000), `mtu`
(1400), `startup_timeout_secs` (120). At start a fiber probes the proxy port
with upstream's backoff (1 s doubling to 10 s); after the timeout it starts
anyway with a warning, and connects and sends are refused until the stream
owner has started. No connection holds a capped slot or an inbound deadline.
Every SOCKS5 failure, a REP `0x05` refusal included, counts `socks5_errors`,
and `show_transports` reports the 10 `NymStatsSnapshot` counters.

Local integration (not exercised here; no mixnet was used): run Nym's
`nym-socks5-client init --id fips --provider <network-requester address>`,
then `nym-socks5-client run --id fips` (it listens on `127.0.0.1:1080`), and
configure `transports.nym: {socks5_addr: "127.0.0.1:1080"}` with peers at
`{transport: nym, addr: "<host-or-ip>:<port>"}`, a FIPS TCP listener reachable
from the network requester. Mixnet connects can take minutes, which the
300-second default allows for.

### Verification and limits

`fipsd_tests` adds `FipsdSocks5Test` (tokio-socks byte vectors and live
fiber handshakes against the loopback fake: both auth modes and every ATYP,
auth failure, no method, refused, rejected, the deadline against a silent
server, cancellation), `FipsdTorTest` (configuration, isolated dials to
numeric and onion targets through the fake proxy, the onion listener, the
four mixed-`mtu` cases, error accounting, the inbound cap and first-frame
deadline, the control row, and the monitor against a fake control port:
password and cookie auth, a Unix socket, the GETINFO fallback, bootstrap,
circuit, liveness and bandwidth events, reconnect, never `ADD_ONION`),
`FipsdTorTwoNodesTest` (the two-node proof: IK link, tree, bloom, XK session,
datagrams, rekey and disconnect over directory-mode Tor through the fake
proxy) and `FipsdNymTest` (both build modes). Fakes run on their own
threads; waits are channels, I/O readiness and deadlines, never sleeps.

`FipsdTorInteropTest` (`tests/tor_interop.py`) is opt-in: with
`FIPSD_TOR_BINARY` and `FIPSD_UDP_INTEROP_RUST_BINARY` it starts two local tors
(one per daemon), waits for each one's `BOOTSTRAP PROGRESS=100` and
`HS_DESC UPLOADED` control events, then establishes the C<->Rust link over the
onions in both directions; otherwise it returns SKIP (77). Recorded on macOS
(tor 0.4.9.17, Rust rev 6aa80504ba): bootstrap 5.8 s and 9.8 s, descriptors
uploaded at 9.8 s and 11.6 s, C->Rust link 4.7 s, Rust->C link 5.8 s, with no
SOCKS5 errors or timeouts, 24.9 s in all. The fipsd row showed
`tor_monitoring` from the real daemon (bootstrap 100, circuit established).
Earlier runs on a fresh onion took 7.8 s / 110.9 s and once 164 s for the
first dial: Tor's rendezvous timing varies.

With `advertise_on_nostr`, a directory-mode onion joins this node's own Nostr
advert as `<onion>:<advertised_port>` once the onion is published (with a
control monitor, `HS_DESC UPLOADED` for this service; see Local advert
endpoints, `nostrc-r4aeo`). Tor endpoints in received adverts are dialled
through this transport. As with TCP, there is no separate path-priority lane.
Version decision: **fipsd stays 0.1.0 Unreleased**; the transports fold into
the initial daemon.

## BLE transport core (`nostrc-gfi8h.6.1`)

A stream transport over BLE L2CAP connection-oriented channels, ported from
upstream `src/transport/ble/` at the `libfips-core/UPSTREAM.md` pin. This bead
is the platform-independent core plus a simulated radio. The radios that make
it useful on hardware are separate backend beads: BlueZ on Linux
(`gfi8h.6.2`, [below](#bluez-backend-nostrc-gfi8h62)) and CoreBluetooth behind
a LaunchAgent (`gfi8h.6.3`). Built by default (`-DFIPSD_ENABLE_BLE=ON`, no
platform dependencies). On a build without a platform backend, a configured
instance is ignored with a warning, as upstream does:
`instance ble: BLE transport unavailable (this build has no Bluetooth backend ...)`.
The simulated radio is never used as a fallback: a transport over it would
start, report itself up and never peer (upstream's `DefaultBleTransport`
tripwire). With `-DFIPSD_ENABLE_BLE=OFF`, a configured `transports.ble` fails
start-up instead, like Nym.

```yaml
transports:
  ble:
    adapter: hci0           # "ble0" for a radio that hides link addresses (CoreBluetooth)
    psm: 0x0085             # requested listener PSM, and the dial fallback
    mtu: 2048               # used until a channel reports its own MTU
    max_connections: 7
    connect_timeout_ms: 10000
    advertise: true
    scan: true
    auto_connect: false     # dial discovered peers on their key hint
    accept_connections: true
    probe_cooldown_secs: 30
```

The keys, defaults and singleton/named-instance shapes are upstream's
`BleConfig`. The schema already carried them; `config/validate.c` keeps
upstream's macOS checks: a single instance, and `ble0/` peer addresses only.
Link addresses are `adapter/AA:BB:CC:DD:EE:FF`. A peer reached through a
CoreBluetooth identifier maps to a stable, locally administered unicast
address under `ble0` (`fipsd_ble_peer_from_identifier`, upstream
`io_macos::peer_addr`, pinned by a test vector).

### Module map (`src/transport/ble/`)

- `backend.h`: the backend contract (below), with a mapping table from
  upstream `io.rs`, through each operation, to BlueZ and CoreBluetooth.
- `ble.{c,h}`: the transport (upstream `mod.rs`). It covers lifecycle and
  adapter presence, the scan+probe loop with its retry book, PSM selection,
  the pubkey exchange, inbound admission with the in-flight budget,
  arbitration and pool admission, per-link owner/reader/writer fibers,
  `connect_async`/`send`/`close_connection`/MTU, DISCOVERED announcements,
  the node verification hooks and config registration.
- `pool.c`: the pool (`pool.rs`), arbitration (`arbitrate`/`newcomer_wins`),
  the retry book (`PendingProbes`) and the bounded address memory
  (`recall.rs`). All pure state over caller-supplied time.
- `psm.c`: the PSM wire format (`psm.rs`): 2-byte little-endian service data
  keyed on the 16-bit UUID `0x9C90`. The same bytes are served as the GATT
  PSM characteristic. The 27-byte legacy advert budget is a compile-time
  assertion.
- `stream_read.c`: one reader per channel across the exchange and the
  receive loop (`stream_read.rs`), over the backend's read/write ops or the
  fd fast path. Frame boundaries come from the shared FMP framer
  (`transport/stream.c`) with the channel's receive MTU as the ceiling, so
  SEQPACKET and byte-stream channels both work.
- `stats.c`: upstream's `BleStatsSnapshot` counters, kept in the shared
  stats block (`stats_common.h`, BLE-only names appended last).
- `radio.c`: the simulated radio (`io_radio.rs`) (below).
- `bluez.{c,h}`, `bluez_wire.c`: the BlueZ backend (`io_linux.rs`) and its
  pure half (advert properties, D-Bus property parsing, error mapping);
  [below](#bluez-backend-nostrc-gfi8h62).
- `ble_disabled.c`: the `-DFIPSD_ENABLE_BLE=OFF` stub.

### The backend contract (`backend.h`)

It uses the libgo idiom of fipsd. Operations park only the calling fiber;
each has an absolute deadline and returns promptly with `-ECANCELED` once
`stop` runs (owner-directed cancellation). Results arrive as values, never as
callbacks into transport state:

| op | semantics |
|---|---|
| `start(self, rt, sink)` / `stop` / `destroy` | launch tracked fibers or foreign threads and report the adapter state at least once / prompt idempotent cancellation of every parked op, no join / join threads after runtime join |
| `adapter` | current `{present, powered}`, cached |
| `listen(requested, deadline, &bound)` | (re)open the L2CAP listener and report the PSM actually bound (an OS-assigned one is advertised); called again after every power-on |
| `advertise(advert, deadline)` / `advertise_stop` | FIPS service UUID plus `0x9C90` PSM service data where the platform can carry it; re-advertising the same PSM must not restart the advertiser |
| `scan_start(deadline)` / `scan_stop` | passive scan for the FIPS UUID, reported as `PEER_DISCOVERED` |
| `read_psm(peer, deadline, value)` | GATT read of the PSM characteristic (raw bytes, decoded by the core); `-ENOTSUP` or NULL when the backend cannot |
| `connect(peer, psm, deadline, &stream)` | dial an L2CAP channel; `-ETIMEDOUT` (out of reach) and `-ECONNREFUSED` must stay distinct |
| `local_addr`, `adapter_name` | optional link address; adapter name |

Streams are `{ops, peer, id}`, where ops are `read`, `write`, `cancel`,
`close`, `mtu` and `fd`. `read` may return a fragment or several SDUs
coalesced. `write` sends a whole buffer and backpressures until its
deadline. `mtu` reports 0 when unknown, and the configured MTU is used.
`fd` returns a kernel descriptor or -1. With a descriptor the core owns
readiness through libgo (`gof_io_reset_fd`, `gof_recvmsg`/`gof_sendmsg` with
deadlines, `gof_io_cancel_fd`), and the backend only closes it.

Events go through a sink owned by the transport: `PEER_DISCOVERED`
(peer, RSSI, raw service data), `PEER_LOST`, `INBOUND_STREAM` (ownership
passes) and `STREAM_CLOSED`. Every sink call is thread-safe and
non-blocking. Events try-send into a bounded queue; drops are counted
(`fipsd_ble_event_drops`) and a dropped inbound stream is closed. Adapter
state is not queued: it is coalesced to the latest value, and up/down
transitions are counted so an off-on blip is still seen as one.

How the platform beads map onto it:

- **BlueZ (`gfi8h.6.2`).** GDBus to BlueZ, kernel sockets for channels.
  `listen` is a SEQPACKET L2CAP socket bound to the requested PSM. Inbound
  and dialled channels are fd streams on the fast path, so SDUs map to
  reads. `advertise` is an `LEAdvertisement1` carrying ServiceData
  `{0x9C90: psm}`. A FIPS device object maps to `PEER_DISCOVERED`, its
  removal to `PEER_LOST`, and `Adapter1.Powered` and removal to the adapter
  state. `read_psm` reads the GATT PSM characteristic, so it can dial macOS
  peers that advertise only their UUID.
- **CoreBluetooth (`gfi8h.6.3`).** The LaunchAgent owns
  `CBCentralManager`/`CBPeripheralManager`; the backend's control thread
  talks to it over its socket. `listen` is `publishL2CAPChannel`, which
  reports the OS-assigned PSM; `advertise` sets the UUID only and serves the
  PSM characteristic. `read_psm` runs discoverServices, then
  discoverCharacteristics, then a read. `connect` connects and then opens
  `openL2CAPChannel`. Channels come back as fds (SCM_RIGHTS) or as bridged
  ops streams, and the agent's absence or presence is the adapter state.
- **BlueZ (`gfi8h.6.2`).** bluer over D-Bus. `listen` is a SEQPACKET
  `StreamListener` bound to the requested PSM. Inbound and dialled channels
  are fd streams on the fast path, so SDUs map to reads. `advertise` is an
  `LEAdvertisement1` carrying ServiceData `{0x9C90: psm}`. `DeviceAdded` with
  the FIPS UUID maps to `PEER_DISCOVERED`, `DeviceRemoved` to `PEER_LOST`,
  and `Adapter1.Powered` and removal to the adapter state. `read_psm` starts
  as `-ENOTSUP`, and a later GATT read lets it dial macOS peers advertising
  only their UUID.
- **CoreBluetooth (`gfi8h.6.3`).** The per-user `fips-ble-agent` owns
  `CBCentralManager`/`CBPeripheralManager` and lends the radio to fipsd over
  its socket (see "BLE on macOS" below). `listen` reports the agent's
  `publishL2CAPChannel` PSM; `advertise` sets the UUID only and the agent
  serves the PSM characteristic. There is no `read_psm`: as upstream, the
  agent reads a UUID-only peer's GATT PSM inside the dial. Channels are
  relayed over the socket with per-channel credit (no descriptor crosses
  it), and the agent's absence or presence is the adapter state.

`radio.c` is the reference for that foreign-thread shape.

### Behaviour

- **Discovery.** The probe loop takes one advert at a time, as upstream. It
  learns the advertised PSM, skips addresses already pooled or resolved to a
  verified peer, and dials. Without a learned PSM it first tries the GATT
  read, then the configured PSM. A failed dial forgets the learned PSM; a
  timeout, cancellation or adapter loss keeps it. After the `[0x00][x-only key]` exchange (5 s per half) it
  arbitrates and pools the link, then announces it to the node as
  `DISCOVERED` with the 32-byte key as payload. Addresses that never
  conclude stay in the retry book (bounded to 32, exponential backoff capped
  at 2^5 cooldowns, rotation). On its cooldown the retry tick re-probes them
  and prunes the 64-entry address memories. This is a deadline in the probe
  loop's select, not a poll.
- **Node.** On an auto-connect transport, `DISCOVERED` with a key payload
  dials the peer (upstream `poll_transport_discovery`). Configured peers at
  the address dial as for every transport. `connect_async` on a pooled
  address publishes `CONNECTED` at once. Otherwise it dials, and reports
  `CONNECTED` when a link holds the address or `DISCONNECTED` when none does.
  A non-deliberate link loss reports `DISCONNECTED`. It is ordered before
  anything that replaces the link at that address: a newcomer waits for the
  old link's notification.
- **Pool.** `max_connections` with a verify grace of
  `max(connect_timeout_ms, 10 s)`. Verified links are never evicted; an
  unverified link past its grace is, a node's duplicates first. The refusal
  log is latched (one warning per run, the count at its end). The node marks
  a link verified on every authenticated frame through the new optional
  `peer_link` transport op, and clears it on peer removal or a path switch
  (upstream `mark_verified`/`clear_verified`). The maintenance tick calls the
  new optional `close_unproven` op to close channels that never carried a
  handshake within `node.rate_limit.handshake_timeout_secs` (FRC-1301 §6.5.4).
- **Inbound.** Inbound channels are registered in arrival order, at most 8 in
  flight, and the oldest pending exchange is aborted (`handshakes_aborted`).
  A settled link at the address makes a new inbound channel redundant.
- **MTU.** Per link, `min(send, recv)` of the channel (the configured MTU when
  unknown). `send` above it counts `mtu_exceeded`, not `send_errors`. A
  received established frame above the receive MTU is malformed and drops
  the link (`recv_errors`). The send queue is 16 deep and full means
  `send_errors`.
- **Adapter presence.** Power-off or removal makes the transport `ABSENT`
  (`BECAME_ABSENT`), drops every link (`DISCONNECTED` each) and cancels
  every exchange in flight. Power-on listens again (possibly on a new
  PSM), advertises it, rescans and emits `BECAME_PRESENT`. A listener that
  will not open retries on the bind backoff. The medium is `ble`.
- **show_transports.** A BLE row has `type`, `state`, `mtu` and `name`, with
  no `local_addr` or `interface`. Its `stats` are exactly upstream's 19
  `BleStatsSnapshot` fields in order.

Differences from upstream, each deliberate:

1. **Racing verified links.** Inside the tie window, a verified link that is
   still racing a newcomer at its own address, in the opposite direction
   (the two ends' crossing dials), is settled by the tie-break like an
   unverified one. Upstream declines the newcomer. But fipsd pushes
   discovery to the node at once instead of on a tick, so one end can verify
   the race's loser before the winner arrives while the other end has
   already replaced it, and both channels die. A newcomer's key claim is
   unauthenticated until Noise, so a claim from any other address never
   displaces a verified link, and outside the window a verified link
   declines every newcomer, both as upstream. The remaining exposure is a
   peer that spoofs the verified device's own address within the window,
   which a real controller refuses while that device is connected.
2. **Crossing swap and restart.** These no longer close the transport
   connection when the replacing link is at the same transport address. Both
   directions of a BLE pair share the device address. Upstream moves the
   session in place.
3. **No path lane.** There is no separate path-priority lane (upstream
   `send_lane`), because the fipsd node has no lane seam, as for TCP.

### Verification

`FipsdBleTest` (`tests/test_ble.c`) ports upstream's `psm`, `addr`/peer
mapping, `pool`, `arbitrate`/`newcomer_wins`, `PendingProbes`, `recall`,
`stream_read` and stats tests as pure state. Over the simulated radio it runs
these scenarios:

- the bridge semantics of `io_radio.rs`: round trip, MTU fallback,
  timeout/EOF/EPIPE, backpressure, cancel, refusal, concurrent and abandoned
  dials, adverts and PSM moves, `PEER_LOST`, isolated airs, power-off, stop;
- lifecycle: OS-assigned and honoured PSMs, and an absent adapter adopted
  later;
- discovery without a local key;
- a pair over SDU, 5-byte-fragment byte-stream and socketpair-fd channels,
  including MTU and stats;
- link loss with redial, and adapter off/on and removal;
- PSM learning, forget-on-refusal, keep-on-timeout and the GATT fallback;
- the exchange (bad prefix, coalesced frame, fragments, a silent peer, the
  send deadline), the inbound budget under a flood, and claims and
  verification with `close_unproven` and the drain on a deliberate close.

`FipsdBleTwoNodes{Sdu,Fd}Test` (`tests/test_ble_two_nodes.c`) is the node
proof. Two in-process nodes with no configured peers go from discovery
through PSM, L2CAP, key exchange and auto-connect to IK, then the tree, bloom
filters, an XK session and datagrams both ways, all checked against show_*
output. Then they recover from a node disconnect and redial, a link loss and
redial, and an adapter power cycle that rediscovery alone repairs. All waits
are channel selects on events, never sleeps. Every one of these passes
under ASan/UBSan and TSan. Interop with the Rust daemon over real radios
belongs to the backend beads. Version decision: **fipsd stays 0.1.0
Unreleased**; the BLE core folds into the initial daemon.

### BlueZ backend (`nostrc-gfi8h.6.2`)

`transports.ble` on Linux, ported from upstream `io_linux.rs` (bluer), in
`src/transport/ble/bluez.c` with its pure half in `bluez_wire.c`. Behind
`-DFIPSD_ENABLE_BLE_BLUEZ`, default ON on Linux when gio-2.0 (2.56 or later,
`libglib2.0-dev`) is found. It needs nothing from BlueZ at build time: the
kernel's Bluetooth socket ABI (`sockaddr_l2`, `BT_*`) is defined in
`bluez.h`, as bluer does, and the unit test checks it against
`<bluetooth/l2cap.h>` where that header exists. macOS and Windows builds do
not compile it.

At run time it needs `bluetoothd` on the system bus and either root or an
equivalent policy (the `bluetooth` group for D-Bus, `CAP_NET_RAW` for the
sockets), as upstream's how-to says. `transports.ble.adapter` names a
controller (`hci0`). The adapter is powered on whenever it appears, as
upstream does at construction. A later power-off is the operator's, and is
reported rather than undone.

**Control plane.** GDBus on a private system-bus connection, driven by a GLib
main loop on the backend's own thread. Because the connection is private,
closing it releases the advertisement and discovery session BlueZ holds for
it, and losing the bus never exits the daemon. A lost bus reconnects with
backoff (1 s to 30 s). Fiber-side operations hand a request to that thread
(`g_main_context_invoke`) and park on a reply channel with their deadline and
the stop channel. Advertising requests run one at a time, and so do scan
requests. The thread reports through the sink only.

- Adapter: the `org.bluez` name watch, ObjectManager `InterfacesAdded`/
  `InterfacesRemoved` and `Adapter1` `PropertiesChanged` give presence, power
  and the address. A vanished `bluetoothd` is an absent adapter.
- Advertising: an exported `LEAdvertisement1` at
  `/org/nostrc/fipsd/ble/<adapter>/advertisement0` (peripheral, the FIPS UUID,
  ServiceData `{00009c90-...: psm LE}`, 400-600 ms, no name). It is registered
  with `LEAdvertisingManager1`. The same PSM is not re-registered, and only
  BlueZ may call `Release`.
- Discovery: `SetDiscoveryFilter {Transport le, UUIDs [FIPS], DuplicateData
  false}`, then `StartDiscovery`. A FIPS device object is reported once per
  discovery session, and again when its PSM service data changes. It is a
  FIPS device when its `UUIDs` carry the FIPS UUID or its ServiceData carries
  the `0x9C90` key. The flag is sticky, because once BlueZ's GATT client
  resolves a device's services (it does on every LE link), `UUIDs` lists the
  GATT services and drops the advertised ones. At scan start, cached objects
  holding no user state are removed (upstream `clear_at_scan_start`) and the
  kept FIPS objects are reported. `InterfacesRemoved` is `PEER_LOST`.
- GATT: `read_psm` calls `Device1.Connect` unless the services are resolved,
  waits for `ServicesResolved`, then reads the PSM characteristic of the
  device's FIPS service, bounded by the caller's deadline. A failed read with
  `Connect` still in flight is aborted, so it does not hold the controller's
  initiator.

**Data plane.** Kernel L2CAP CoC sockets (`AF_BLUETOOTH`, `SOCK_SEQPACKET`) on
fibers through libgo. Each socket is bound to the adapter's address (LE
public), then set to `BT_SECURITY` low (no SMP pairing: FIPS authenticates the
peer with Noise IK) and `BT_RCVMTU` to `transports.ble.mtu`. The kernel takes
`BT_RCVMTU` only once the socket is bound to an LE address. `listen` runs a
tracked accept fiber. A re-listen waits for the old one to release its PSM.
`connect` is a nonblocking `gof_connect` bounded by the deadline, dialled as
LE random only when BlueZ reports the device so. Every channel is an fd
stream: the core reads and writes it with `gof_recvmsg`/`gof_sendmsg`, and
`BT_SNDMTU`/`BT_RCVMTU` are its MTUs. A dead channel is seen by the core's
own reader, so the backend sends no `STREAM_CLOSED`.

**Errors and the PSM rule.** D-Bus errors map to backend.h's errno
(`fipsd_ble_bluez_dbus_errno`/`gerror_errno`). One kernel behaviour needs
care. An unanswered LE dial (nobody there) ends with `ECONNREFUSED`, the same
errno as a real refusal, when the channel timer fires. On Linux 7.0 an absent
device fails after 41.04 s with the default 40 s timer, and after 5.07 s with
`SO_SNDTIMEO` 5 s: the timer is the socket's `SO_SNDTIMEO`. A dial therefore
sets it 2 s past its deadline, so the deadline (`-ETIMEDOUT`) ends an
unanswered dial first. An `ECONNREFUSED` at the timer still maps to
`-ETIMEDOUT` (`fipsd_ble_bluez_connect_errno`). `EHOSTDOWN` is a timeout too,
and a dial on a downed adapter (`EHOSTUNREACH`) is `-ENODEV`.

**Dials pause discovery.** While BlueZ discovers, the kernel starts no
passive scan of its own for a pending LE connection. The connection waits for
an advert report of its target, and discovery's scan reports a device once
per cycle (controller duplicate filtering, about 10.24 s for an LE-only
cycle). A dial is made *because of* that report, so whenever it became
pending after the report and its scan response, btmon showed no LE Create
Connection at all until the deadline. That was every retry, under TSan's
slower timing. A dial, and a GATT `Device1.Connect`, therefore pause
discovery: refcounted, ordered with the scan requests, and with the same
session and reports afterwards. The kernel's passive accept-list scan then
connects on the peer's next advert. bluer dials the same way.

**Crossing dials.** Kernels without f50b32aa233d and 15a80cc8263a (merged
2026-11-02; 7.0 lacks them) match an LE Connection Response to the first
channel carrying its signalling ident. An accepted channel keeps the ident of
the request that opened it, and both ends number a fresh ACL's requests from
1. A dial sent just as the peer's own request is accepted thus gets its
response wired into the accepted channel, and that channel's remote CID is
overwritten. btmon showed it on the VM: both requests carried ident 1, and
the IK msg2 went out on the dead CID, so the link never authenticated (about
one run in five). bluer dials the same way. The backend therefore:

- does not dial a peer that holds an accepted channel to us. It returns
  `-EISCONN`, which backend.h now defines; the core does not count it and
  keeps the PSM;
- breaks crossing dials by address: the larger address yields up to 1.5 s to
  a FIPS advertiser, whose own dial then lands first;
- shuts down an accepted channel whose remote CID (`getpeername`) changed
  during a dial to its peer, so the core recovers.

**Verification.** `FipsdBleBluezTest` (`tests/test_ble_bluez.c`) covers the
pure half: the socket ABI against `<bluetooth/l2cap.h>`, paths, the
advertisement and its 27-byte budget, `Device1`/`Adapter1` property parsing
and merging, the GATT lookup, the scan-start rule, the dial address type,
D-Bus/GError mapping and the connect-error mapping that carries the PSM rule.

`FipsdBleBluezSmokeTest` (`tests/ble_bluez_smoke.py`, opt-in) runs the real
thing on one Linux host. It brings up two virtual LE controllers (`btvirt -l2`
over `hci_vhci`, BlueZ's emulated air), or uses `FIPSD_BLE_BLUEZ_ADAPTERS=hciA,hciB`.
Then it runs two daemons, each bound to one controller with no other
transport, and requires discovery, the advertised PSM, L2CAP, IK, XK and
native datagrams both ways. The second node listens on a non-default PSM.
The scenarios are:

- C<->C, then a power cycle of one adapter that must drop and restore the
  link;
- C<->C with the PSM read over GATT from a UUID-only advertiser
  (`ble_bluez_gatt_peer.py`, CoreBluetooth-shaped);
- C<->Rust with each side starting the handshake, when
  `FIPSD_BLE_BLUEZ_RUST_BINARY` names the pinned `fips`.

```sh
sudo apt-get install -y bluez bluez-test-tools   # btvirt, btmgmt, bluetoothctl
cmake -B build -DFIPSD_BLE_BLUEZ_RUST_BINARY=/path/to/fips   # optional
sudo env FIPSD_BLE_BLUEZ_SMOKE=1 ctest --test-dir build -R '^FipsdBleBluezSmokeTest$' -V
```

Measured on Ubuntu 24.04 aarch64 (Linux 7.0, BlueZ 5.72, btvirt), from daemon
start, over five ASan/UBSan/LSan runs and three TSan runs of the whole suite:

| Scenario | IK link | XK session | datagram RTT |
|---|---|---|---|
| C<->C | 0.3-1.2 s | 0.4-0.9 s | 31-39 ms |
| C<->Rust, C starts | 0.2-1.3 s | 2.2-2.3 s | 30-34 ms |
| C<->Rust, Rust starts | 3.1-3.4 s | 4.2-4.3 s | 5-12 ms |

A power-off is seen in 57-64 ms, and the link returns 9-12.5 s after
power-on. The GATT scenario links after about 10.3 s. The probe loop sets
both times: a peer probed (or refused at a stale PSM) is retried at its
cooldown (`probe_cooldown_secs`, 5 in the smoke). btvirt reports no RSSI
(127, taken as unknown).

Distro GLib hands off between threads over futexes TSan cannot see, so a
TSan run of the smoke points the daemons, and only them, at an instrumented
GLib. Build one as `scripts/grotto-tsan-runtime.sh` does, with `CC=clang` and
`-Db_lundef=false` (clang links its TSan runtime only into executables).
Then pass `--fipsd` a wrapper that sets `LD_LIBRARY_PATH` to it; Python and
the Rust daemon keep the system GLib.
## BLE on macOS: CoreBluetooth through `fips-ble-agent` (`nostrc-gfi8h.6.3`)

macOS never lets a root launchd daemon use Bluetooth: CoreBluetooth answers it
`Unauthorized` whatever the privacy settings say. So, as upstream, the radio
lives in a per-user agent and the transport in the daemon:

- **`fips-ble-agent`** runs in each user's login session (the LaunchAgent
  `com.fips.ble-agent`). It owns `CBCentralManager` and `CBPeripheralManager`,
  publishes the L2CAP listener, serves the GATT PSM characteristic,
  advertises the FIPS service UUID, scans, dials and accepts channels.
- **fipsd** binds the agent socket `/var/run/fips/ble-agent.sock` (group
  `fips`, mode 0770, the control socket's policy). The agent connects to it,
  says `HELLO` with its listener's PSM, and lends the radio for as long as it
  stays connected. One agent holds the radio at a time; another one gets
  `BUSY` and stands by with backoff, so two users in the `fips` group never
  take the radio from each other. An agent that leaves is a radio switched
  off; one that comes back is a replaced radio, so the core listens,
  advertises and scans afresh.

The wire protocol is upstream's (`io_macos/agent_proto.rs`, version 2), byte
for byte, so either side interoperates with upstream's other side. The
daemon end is upstream's `agent_server.rs` and the agent end its `agent.rs`.
As upstream, the agent refuses a socket not answered by the daemon's uid
(root by default).

Built by default on Apple (`-DFIPSD_ENABLE_BLE_COREBLUETOOTH=ON`, which needs
`-DFIPSD_ENABLE_BLE=ON`). The option makes the agent backend this build's
`fipsd_ble_platform_backend` and builds `fips-ble-agent` (Objective-C with
`-fobjc-arc`, Foundation and CoreBluetooth). The protocol and the daemon
backend are portable POSIX, and are built and tested on every platform the
BLE core builds on.

### Module map

- `src/transport/ble/agent_proto.{c,h}`: frames (`len u32 BE | tag | payload`),
  encode/decode, length limits (`MAX_FRAME` 128 KiB, `MAX_HELLO_FRAME` 16),
  and the identifier-to-address mapping. No fipsd dependency; compiled into
  both fipsd and the agent.
- `src/transport/ble/agent_client.{c,h}`: the `backend.h` implementation in
  fipsd. It is the agent's client for the radio, and on the socket it is the
  listener, as upstream. It runs on fibers only: one accepts, one reads each
  connection (a 5 s budget for its hello, then frames) and one writes each live
  connection's queue.
- `src/ble-agent/session.{c,h}`: the agent's end of the socket, written against
  a radio vtable rather than CoreBluetooth. It handles reconnect with backoff,
  the peer-uid check, `BUSY` standby, and a session tied to the radio's PSM.
  It keeps a control queue and a data queue towards the daemon, grants each
  channel credit and closes it on overrun, and runs two pump threads per
  channel. Plain C and pthreads, so the tests run it on every host.
- `src/ble-agent/radio.{m,h}`: the CoreBluetooth radio (`corebluetooth.rs`,
  with `Listener`, `DialPlan` and `AdvertThrottle` from `mod.rs`). Both
  managers and every CoreBluetooth call stay on one serial dispatch queue. It
  reports to the session only through non-blocking calls, and nothing runs
  on a run loop.
- `src/ble-agent/main.m`: the CLI, logging, SIGTERM/SIGINT and `--probe`.
- `src/ble-agent/Info.plist.in`: linked into `__TEXT,__info_plist` with
  `-sectcreate`. It carries `NSBluetoothAlwaysUsageDescription`, without which
  CoreBluetooth kills the process.
- `packaging/macos/com.fips.ble-agent.plist`: the LaunchAgent.

### How the contract maps

| backend.h | over the agent |
|---|---|
| `start` | bind and accept; adapter reported absent until a hello |
| `adapter` | present and powered while an agent holds the radio |
| `listen` | the hello's PSM (OS-assigned by `publishL2CAPChannel`). An agent with no listener (PSM 0) reports the requested PSM, as upstream `RadioIo::listen`. The advert then asks the agent to publish again, and once it has a PSM it reconnects with it. |
| `advertise` / `advertise_stop` | `START_ADVERTISING` / `STOP_ADVERTISING`; the agent advertises the UUID alone (CoreBluetooth carries no service data) and serves the PSM over GATT |
| `scan_start` / `scan_stop` | `START_SCANNING` / `STOP_SCANNING`; each `SCAN` frame becomes `PEER_DISCOVERED`, its PSM as `0x9C90` service data |
| `read_psm` | none (NULL): the agent's dial plan reads a UUID-only peer's GATT PSM itself and opens on it, else on the PSM fipsd asked for |
| `connect` | `CONNECT` / `CONNECT_RESULT`; `ok=false` carries no reason and maps to `-EIO`, and an unanswered dial is `-ETIMEDOUT` at the deadline (its late answer is closed) |
| `INBOUND_STREAM` | an `INBOUND` frame |
| `STREAM_CLOSED` | the agent leaving (every channel resets) |
| `local_addr` / `adapter_name` | hidden / `ble0` |

**Streams** cross the boundary as upstream relays them: framed over the
socket, never as a passed descriptor (the stream `fd` op is -1). The agent
grants each channel a 32 KiB window when it opens and returns credit as its
radio takes the bytes. `write` sends a packet whole as soon as any credit is
left (a `SEND` of at most 64 KiB), and parks for `CREDIT` until its deadline.
`RECV` bytes queue per channel, 256 chunks deep. When a queue is full, the
reader waits for room rather than dropping bytes, because a hole would break
a byte stream's framing. So a peer that stops taking bytes stalls only its
own channel at the daemon; the agent's session never waits on a channel.
`CLOSED` is end of stream after the queued bytes; the core's `close` sends
`CLOSE`; cancel and stop latch `-ECANCELED`. The MTU each channel reports is
the agent's transports.ble default, 2048, because CoreBluetooth hides the
real one.

### Running it

```yaml
transports:
  ble:
    adapter: ble0      # CoreBluetooth hides link addresses (validate.c allows only ble0/ peers)
    auto_connect: true
```

The macOS package (`packaging/macos/build-pkg.sh`) installs
`/usr/local/bin/fips-ble-agent`. It checks that the stripped binary still
carries its `__info_plist`, installs `/Library/LaunchAgents/com.fips.ble-agent.plist`,
and stops and restarts the agent in every GUI session on install and
upgrade. `uninstall.sh` removes both. Users who log in later get the agent
from launchd. The user must be in the `fips` group (postinstall adds the
console user; that takes effect at the next login) to reach the daemon's
socket. By hand:

```sh
sudo cp fips-ble-agent /usr/local/bin/ && sudo cp com.fips.ble-agent.plist /Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) /Library/LaunchAgents/com.fips.ble-agent.plist
tail -f ~/Library/Logs/fips/ble-agent.log
```

- **Bluetooth permission (TCC).** The agent's first launch requests
  Bluetooth with the embedded usage string. It reports the permission as
  "not determined" until the request is answered. If no prompt shows, allow
  `fips-ble-agent` under System Settings › Privacy & Security › Bluetooth.
  macOS attributes a process to its *responsible* process. Under launchd the
  agent is its own, so the request names `fips-ble-agent`. Started from a
  terminal or IDE, it belongs to that app, and if the app's own Info.plist
  lacks `NSBluetoothAlwaysUsageDescription`, CoreBluetooth aborts the agent
  (SIGABRT, crash namespace TCC). An IDE's shell did exactly that. Run it
  through launchd (as below) when in doubt. Once allowed, rebuilt ad-hoc
  signed agents (same identifier, new code hash) were allowed again within a
  second, without asking.
- **Options.** `--socket PATH` (else `$FIPSD_BLE_AGENT_SOCKET`, else
  `/var/run/fips/ble-agent.sock`), `--daemon-uid UID` (default 0),
  `--log-file FILE`, `--verbose`, `--version`, `--help`, and `--ble-agent`
  (accepted for upstream's plist). Logs go to stdout on a terminal, else to
  `~/Library/Logs/fips/ble-agent.log` (0600, in a 0700 directory of the
  user's own; the previous log is kept as `.1` past 10 MiB at start).
- **`--probe SECONDS`.** A diagnostic with no daemon: power up, publish a
  listener, advertise and scan every advertiser for SECONDS. It then reports
  the permission, both managers' states, the PSM, whether advertising
  started, and what it heard. It exits 1 unless the radio came up
  advertising.
- **An unprivileged daemon, or a test.** Point both ends at another socket:
  `FIPSD_BLE_AGENT_SOCKET=/tmp/x/ble.sock fipsd ...` and
  `fips-ble-agent --socket /tmp/x/ble.sock --daemon-uid $(id -u)`. A throwaway
  LaunchAgent in `gui/$(id -u)` gives the agent its own TCC identity:

  ```sh
  launchctl bootstrap gui/$(id -u) /path/to/com.example.ble-agent.plist   # ProgramArguments: the agent and its flags
  launchctl bootout gui/$(id -u)/com.example.ble-agent
  ```

Differences from upstream, each deliberate:

1. **fipsd never tries CoreBluetooth in process.** Upstream's daemon first runs
   its own `MacRadio` and falls back to the agent when refused. That would
   bring Objective-C and an Info.plist into fipsd, only to work when it is not
   a launchd daemon. The agent is the only radio.
2. **A separate executable.** The agent is `fips-ble-agent`, not
   `fips --ble-agent` behind a hard link, because fipsd has no shared binary
   to link to. `--ble-agent` is accepted and ignored.
3. **`--daemon-uid`, and a daemon socket path overridable through
   `FIPSD_BLE_AGENT_SOCKET`.** Upstream always serves root, at the fixed path.
   The defaults are the same.
4. **Event-driven radio state.** The agent hears the radio come up, go down or
   change PSM from the radio itself, not by polling it every 200 ms. Dials
   reach the radio under agent-wide tokens, so a late answer can never land
   on a later session.

### Verification

`FipsdBleAgentTest` (`tests/test_ble_agent.c`) covers three layers:

- **Protocol.** The 18 frames of upstream's own `every_frame()` list, checked
  byte for byte against vectors the pinned upstream encoder printed
  (`agent_proto.rs` compiled verbatim). It also checks a frame stream,
  truncated, unknown and oversized frames, the hello limit, and the
  identifier mapping against `fipsd_ble_peer_from_identifier`.
- **The backend against a scripted agent** on its socket. This covers the
  hello, `BUSY` for a second agent, silent, wrong-version and oversized first
  frames, a malformed frame ending the session, a replaced radio and the PSM-0
  fallback. Then the commands, adverts with and without PSM and RSSI, dials
  (accepted, refused, timed out with a late answer closed), and accepted
  channels. Then credit: parking, spending a packet whole, splitting, and
  256+64 queued `RECV`s read back with no hole. Then `CLOSED` as EOF after
  data, cancel, the agent leaving (`-ECONNRESET`, `-ENODEV`, `STREAM_CLOSED`),
  `stop` (`-ECANCELED`), and a stream closed after `destroy`.
- **End to end.** Two BLE transports over their backends, each lent a radio
  by the real agent session over mock radios, one advertising like a Mac and
  one like BlueZ. They exercise discovery, the GATT/learned PSM, the key
  exchange, one link and frames both ways. Then a radio power cycle onto a
  new PSM, the transport going absent and present again, and the relink.

`packaging/testing/smoke-ble-agent.sh FIPSD FIPS_BLE_AGENT` is the real-radio
smoke on a Mac, run as a user. It launches the agent's `--probe` as a
throwaway LaunchAgent and checks that the radio comes up, publishes a PSM and
advertises. Then it runs fipsd with the agent socket in a temporary directory
and the agent connecting to it. The checks: fipsd's transport comes up on the
agent's PSM, and the agent advertises and scans. Both jobs are booted out at
the end. A real L2CAP link needs a second BLE device running FIPS.

Run on a MacBook's BCM4388 with Bluetooth on (2026-11-18), the smoke passed,
probe and fipsd half alike. The probe reported the permission allowed, both
managers powered on, the L2CAP listener published on PSM 0x00C0
(OS-assigned) and the radio up, advertising the FIPS service and scanning; in
12 s it heard 270 to 340 adverts from about 50 devices, none of them FIPS. In
the fipsd half, fipsd took the agent's hello, brought the BLE transport up on
ble0 at PSM 0x00C0 (`listen`), and the agent advertised (`advertise`) and
scanned. The same PSM came back on every run, which suggests the agent's exit
gives it back to bluetoothd. The smoke also found a duplicate advertising
start, since fixed: CoreBluetooth confirms a start before its own
`isAdvertising` turns true.

Open: the Mac↔Linux L2CAP acceptance (a channel, the key exchange and frames
over the air) needs a second BLE device running FIPS. None was available, and
the Linux VM has no adapter. Nor can a second agent on the same Mac stand in
as the peer. The agent advertised the FIPS service while scanning every
advertiser and never heard itself, and one machine's radio does not discover
its own adverts, so two agents sharing a radio cannot see each other. A
second agent would also be told `BUSY`, and upstream's Rust agent serves only
a root daemon and needs a Bluetooth grant of its own. It was not run.

`FipsdBleAgentBinaryTest` checks the built agent's embedded Info.plist and its
CLI. `FipsdBleTest` checks that the agent backend is the platform backend on
macOS. All of these, and the whole `^Fips` suite, pass under ASan/UBSan (Apple
clang) and TSan (Homebrew clang).

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

**C->Rust application datagrams** (closed by `nostrc-9nxwk.4`): the harness
runs `fipsd_native_client_tool`, a C client over libfipsnative, at fipsd's
native API holding the port the Rust flow targets. Each Rust->C datagram is
accepted and printed by that client (the bytes, not only C's counters), and
the client answers every flow with a datagram the harness reads off the Rust
daemon's flow descriptor while Rust's `show_native_flows`
`received_datagrams`/`received_bytes` count it. With
`FIPSD_UDP_INTEROP_RUST_ECHO` naming upstream's `native-echo` client built from
the pinned tree, the harness also runs that unmodified Rust client on fipsd's
native API and echoes a datagram through it from the Rust daemon's side. The
session layer is still exercised both ways by the C and Rust probes. Measured locally on macOS ARM64 (default build, pin
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
NAT traversal or deployment proof. The new local STUN/C↔C traversal tests are
separate evidence; the formal protocol review (`8hvs6.4`) was completed 2026-11-16
(docs/reviews/fips-nostr-protocol-review-2026-11-16.md).

## Linux netns/veth chaos harness (`nostrc-f66sr.3`)

`Testing/fips-chaos/` is the netns/veth subset of upstream's `testing/chaos`
and `testing/nat` labs rebuilt on `ip netns` without Docker (its README has
the layout and the per-scenario assertions). `run.sh` (root) drives
`chaos.py`, which builds a topology per scenario, runs `fipsd` and, when a
pinned Rust daemon is supplied, the `fips` daemon inside the namespaces, and
observes everything through the control sockets, the daemons' logs, the lab
relay/STUN logs and kernel counters under deadlines; teardown is a namespace
sweep in `finally` plus the wrapper's trap. The daemon lifecycle and control
helpers are imported from `tests/udp_interop.py`. Each scenario runs as `cc`
(fipsd only) and `cr` (fipsd with the Rust daemon):

| Scenario | What is asserted |
| --- | --- |
| `flap` | chain n1–n2–n3; n1's veth down: both ends drop the peer within the 30 s silence floor, n2–n3 re-converge; up: the configured retry re-links, the three-node tree and merged bloom filters converge, a two-hop n1→n3 probe completes (before and after) |
| `mtu` | veth at 600 from the start: link, tree and the 1 KiB bloom announce cross (`FragCreates` rises on both sides), probe and max-payload Rust→C datagram delivered; raised to 1500 and lowered to 900 live, traffic still flows; the configured transport MTU is recorded, not asserted to adapt (neither daemon tracks interface MTU) |
| `nat` | two nodes behind nftables `masquerade` routers (or `--nat-mode cone`), relay + STUN on a `wan` bridge: STUN from each wan tuple → adverts → signals → punch conntrack → healthy link to the peer's reflexive address, probe and datagram through both NATs; a stall names the stage |
| `netem` | `delay 40ms 10ms loss 5% 25%` on one leg plus iperf3 UDP load: link/tree form under impairment, FMP link rekey and (cr) FSP session rekey complete, traffic flows under the new keys, no mismatch/decrypt/replay counter |

CI: the `chaos-netns` job of `.github/workflows/fips-ci.yml` runs on
`schedule`/`workflow_dispatch` on ubuntu-24.04 with the `interop-udp` pin and
Rust-daemon cache steps, `iproute2 nftables iperf3 conntrack`, fipsd under
ASan/UBSan and `sudo -E run.sh`; evidence is uploaded on failure.

Measured on the Ubuntu 24.04 aarch64 VM (10 vCPU, clang 18, Debug, pin
`6aa80504`): all eight runs pass. NAT traversal C↔C links in 0.5 s and C↔Rust
in 2.1 s (the Rust side punches from a per-attempt socket, so fipsd sees it at
`198.18.0.11:<ephemeral>`; fipsd punches from its bound `:2121`), both with a
probe/XK session and a Rust→C datagram through both NATs; link flap: dead
declared at 30.8/31.9 s, link back 4–5 s after `up`; netem: link rekey 41/38 s
after link-up, session rekey 30 s (cr); MTU: 2 fragments per side at link
set-up at MTU 600. The harness also found and this task fixed, with
regressions in `FipsdNodeHandlersTest`:

- the diagnostic probe's bloom stage never learned its lookup-initiation
  outcome (`node/handlers/probe.c` discarded it), so any non-direct target
  failed as `no_response` after two ticks although the lookup had resolved
  (`hs_lookup_initiate_outcome` now reports miss/suppressed/deduplicated/
  fanout; a target no filter covers fails as `bloom_miss` at once);
- a responder never warmed its coordinate cache from the session setup's
  source coordinates (upstream `forwarding.rs warm_coord`), so its msg2 to an
  initiator two hops away was dropped `no_route` and the handshake timed out
  (`node/handlers/session.c` now inserts the hint; `show_cache` on the
  responder lists the initiator);
- fipsd did not compile as strict C11 on glibc (`clock_gettime`, `strnlen`,
  `SO_RCVBUFFORCE`): `_POSIX_C_SOURCE=200809L` on the fipsd targets and
  `_DEFAULT_SOURCE` in `transport/sockbuf.c`, which the Linux CI jobs needed
  to build at all.

Known and recorded, not fixed here: fipsd starts a new traversal toward an
already-connected peer on every advert refresh (`nostr/driver.c`, under
sibling review); a C probe's teardown is local only (upstream parity), so a
Rust peer left holding that session sends into it until its idle timeout —
the harness opens the Rust-initiated session first, as `udp_interop.py`
does; neither daemon adapts to a lowered interface MTU (kernel fragmentation
carries the traffic; an asymmetric MTU is a black hole at this layer).
## `.fips` DNS responder (`nostrc-9nxwk.3`)

`src/dns/` ports the daemon's `.fips` resolver: upstream `src/dnsmsg.rs`
(the message screen and reply builder), `src/hosts.rs` (the host map and its
mtime-driven reloader) and `src/ipv6tun/dns.rs` with the DNS child of
`src/ipv6tun/lifecycle.rs` (the responder and its socket), as the `dns`
runtime subsystem. `main.c` creates it when `dns.enabled` is true (the
default) and registers it after the TUN service, whose device it reads at
start for the mesh-interface filter, and before the control socket. The
configuration keys are upstream's, already in the schema: `dns.enabled`,
`dns.bind_addr` (default `::1`), `dns.port` (default 5354; `0` binds an
ephemeral port, which the tests use) and `dns.ttl` (default 300, clamped to
2147483647 as `floors.c` and `fipsd_dns_clamp_ttl` both do).

| Module | Upstream | Owns |
|---|---|---|
| `msg.{c,h}` | `dnsmsg.rs` | the 12-byte header, the 4096-byte receive bound (a datagram that fills it is dropped as possibly truncated), the screen (response, service source ports 0/7/13/17/19/37, counts that are not a query's, header-only NOTIMP/FORMERR, REFUSED with the question for a class other than IN/ANY), question parsing with no compression pointers or extended labels, 63-byte labels and 255-byte names, the reply builder (fresh header, opcode QUERY, RD/CD copied, caller AA/RA, the question byte for byte, owner pointers to it, no additional record so TC is never needed), AAAA and SOA records, `encode_query`, rcode mnemonics |
| `hosts.{c,h}` | `hosts.rs` | hostname validation (alphanumerics and hyphens, 1-63, no leading/trailing hyphen, not `npub1…`), the lowercase host map with reverse node-address lookup, the configured peers' `alias` fields, the hosts file (`#` comments, blank lines, `hostname npub` per line, bad lines warned and skipped, non-UTF-8 content a read failure as `read_to_string`'s is), the reloader: base aliases under the file, the file re-read when its mtime changes, a failed read keeping the names last read and retried on the next check even without an mtime change, one WARN per run of failures and one INFO when a read ends it, `set_base` for a replaced peer list |
| `resolver.{c,h}` | `ipv6tun/dns.rs` | `<label>.fips` recognition by wire labels (exactly two, the last `fips` in any case; a dot inside a label does not split it), host-map-then-npub resolution with the npub accepted in any letter case and bech32m/padded spellings refused, `answer` (AAAA with AA for a resolvable name, NODATA for any other type on it, NXDOMAIN otherwise), `handle_packet`, the mesh side (`MeshSide`/`Arrival`), `mesh_query` (by interface, or by destination/mesh-source address when the index is unknown), the reply-path table (`Routed`/`Pinned`/`Returning`/`Withheld`, strict pin on Linux only), `returns_by` over libfips-core's on-link prefixes, the dual-stack `IPV6_V6ONLY=0` + `IPV6_RECVPKTINFO` bind, `recvmsg` with PKTINFO (`fipsd_udp_decode_control`), routed/pinned (`IPV6_PKTINFO`, unspecified source, Linux)/returning (route probe + system prefix read through the blocking executor) sends |
| `service.{c,h}` | `ipv6tun/lifecycle.rs` (`start_dns`, `run_responder`) | the subsystem: config read, bind (an invalid address or a failed bind is a WARN that leaves the host up, as upstream), the TUN's kernel device name resolved to the mesh index with the "Mesh interface index unresolved" fallback warning, one receive fiber (`gof_recvmsg`), the identity sink (`fipsd_node_handlers_learn_identity`, called **before** the reply is sent so the cache is primed when the client connects), the peer-alias base rebuilt from a replaced configuration snapshot before the next answer, the hosts-file check on each query that passed the screen only, owner-directed cancellation (`gof_io_cancel_fd`) at stop with the fiber closing the socket on its way out, and test seams (hosts path, identity sink, mesh side, an event channel, the bound address) |

**Behaviour versus upstream.** Byte-for-byte on the wire for everything the
daemon's responder sends: the screen's drop and refuse rules and their
header-only replies, the AAAA reply exactly 28 bytes longer than the query,
NODATA versus NXDOMAIN, the flags copied and cleared, and the SOA that the
gateway's forwarder adds (ported with the codec; the gateway itself is not
this task). Three deliberate parity notes. Upstream's resolver has no PTR
handling and no per-source rate limiting, and neither is added here: a PTR
(or any non-AAAA) query for a resolvable `.fips` name gets NODATA, a reverse
`ip6.arpa` name is not under `fips` and gets NXDOMAIN, and admission is the
screen's drops (service ports, a response fed back, malformed counts, the
receive bound) plus the dispatcher's bounded command queue, which the
identity sink never blocks on (a full queue is logged at DEBUG and the answer
still goes out); a per-source limiter would throttle the one source every
host query comes from, the stub resolver on loopback. `node.buffers.dns_channel`
is accepted but unused: upstream's DNS→node channel is this daemon's handler
command queue. The hosts file is read at `/etc/fips/hosts` on Linux and
`/usr/local/etc/fips/hosts` on macOS/FreeBSD, the literals `acl.c` already
uses; the hosts `stat` per query and the file read on change run inline on
the fiber as upstream's do on its task.

**Resolver integration.** `packaging/common/fips-dns-setup` and
`fips-dns-teardown` are upstream's scripts verbatim (shellcheck-clean): the
setup tries, in order, a systemd `dns-delegate.d` drop-in (systemd ≥ 258), a
global `/etc/systemd/resolved.conf.d/fips.conf` drop-in (`DNS=[::1]:5354`,
`Domains=~fips`), a dnsmasq `server=/fips/::1#5354` drop-in, then the
NetworkManager dnsmasq plugin, records the backend in
`/run/fips/dns-backend` and prints manual instructions when none applies;
the teardown reverts whichever backend the state file names (or all of them)
and removes the state file. Both are idempotent. The packaging directories
that ship them (`debian`, `rpm`, `arch`, `systemd`) belong to the packaging
task. On macOS there is no script: create `/etc/resolver/fips` containing

```
nameserver ::1
port 5354
```

and `mDNSResponder` routes `*.fips` to the daemon (`scutil --dns` lists the
resolver; `dig` bypasses it, so test with `dig AAAA <npub>.fips @::1 -p 5354`
or `dscacheutil -q host -a name <npub>.fips`).

**Tests** (`ctest -R '^FipsdDns'`, no sleeps). `FipsdDnsMsgTest` ports every
vector of `dnsmsg.rs`'s tests (rule by rule: short, 4096/4095, response,
service ports, counts, NOTIMP/FORMERR header-only replies, pointer/extended
labels, 63/64-byte labels, 255/256-byte names, every truncation of a
question, CH refused with the question, Z ignored, raw unknown qtypes, an
OPT accepted and not echoed, the lossy text form, TTL clamp, rcode names,
`encode_query`, the three SOA placements, every reply dropped when fed back,
RD/CD/ID copied on error replies, the exact question echo, the AAAA parse,
the two-question datagram bound). `FipsdDnsHostsTest` ports `hosts.rs`'s
tests and the hosts-file cases of `tests/dns.rs` with explicit `utimensat`
mtimes (unreadable file keeps its names and its override of a peer alias,
retry without an mtime change, removal drops names, warn once per failed run
and log its end, a run not ended by the mtime returning). `FipsdDnsResolverTest`
ports the resolution, screening and `tests/dns.rs` tables: both reply-path
tables (strict and non-strict pin), the mesh-query table, `returns_by`, the
reflection bounds (511-byte twelve-SRV and 277-byte two-question datagrams),
loops, header handling, the last-label-`fips` rule, the owner pointer, npub
case variants and the bech32m/padded refusals, and the socket helpers on
loopback (PKTINFO destination and interface, routed and returning delivery,
a withheld reply never sent, the route probe; on Linux a pinned send to
loopback delivered and one to a missing interface failed). `FipsdDnsEndToEndTest`
(and `…V4Test` on `127.0.0.1`) starts a node, its handlers and the `dns`
subsystem on `dns.port: 0`, sends a real AAAA query over a UDP socket, checks
the derived address and the exact 28-byte growth, then issues a live provider
command as a dispatcher barrier (the learn command the responder queued
before replying has been applied and published when it returns) and finds
the npub and its address in `show_identity_cache`; it also covers the hosts
file and a configured peer alias (upper-case), NXDOMAIN, NODATA for A and
PTR, a header-only FORMERR, a response fed back and the next query answered,
a hot reload of the hosts file, the per-datagram event sequence, and a
cancellation-safe shutdown within the grace period. `FipsdDnsMeshFilterTest`
treats loopback as the mesh interface and observes the query dropped
unanswered. The root-only network-namespace tests of `tests/dns.rs` (forged
mesh sources through a veth pair) are not ported; the pure reply-path and
`returns_by` tables they rest on are.

**Verification (nostrc-9nxwk.3).** Out-of-tree builds with the recipe above
(`-Werror`, clang on macOS): the ASan/UBSan tree ran `ctest -R '^Fips'` as
173 passes and 4 environment skips (`FipsdControlInteropTest`,
`FipsdTcpInteropTest`, the two privileged smoke tests; the three Rust-binary
interop tests skipped and the soak is disabled), the TSan tree the same 173
passes with zero ThreadSanitizer reports; the known `relay.c:1064` race
(nostrc-xqwgo) did not fire. Acceptance was run for real against the daemon,
not the tests: on macOS, `dig AAAA npub1qmc3…zel98.fips @::1 -p 5354`
returned `NOERROR`, `aa`, one AAAA `fd00:dc7a:5def:9255:b47f:79a7:199:3d8e`
TTL 300 (the upper-case name the same, `A` NODATA, `unknown.fips` NXDOMAIN),
`show_identity_cache` over the control socket then listed that npub with the
same `ipv6_addr`, and SIGTERM exited 0. On the Ubuntu 24.04 aarch64 VM
(systemd 255, `systemd-resolved` active; the tree built with the FIPS CI
lane's clang/`GO_WARNINGS_AS_ERRORS=ON` recipe) the six `FipsdDns*` tests
passed, the same `dig @::1 -p 5354` answered, `sudo fips-dns-setup` chose the
`global-drop-in` backend (`/etc/systemd/resolved.conf.d/fips.conf` with
`DNS=[::1]:5354` and `Domains=~fips`, `/run/fips/dns-backend`,
`resolvectl status` showing both in the Global section, `/etc/resolv.conf`
still the `127.0.0.53` stub), after which `dig AAAA npub1qmc3…zel98.fips`
with no server, `resolvectl query` and `getent ahostsv6` all resolved through
systemd-resolved to the same address; `sudo fips-dns-teardown` removed the
drop-in and the state file and `resolvectl` showed the Global section as
before; both scripts ran twice without change. The raw
`-DCMAKE_C_FLAGS=-Werror` recipe is clang-only: gcc 13 on the VM stops in
untouched `libfips-core` (`-Warray-parameter` in `proto/fmp/wire.c`,
`-Wmisleading-indentation` in `proto/path/params.c`), and on that VM the
control subsystem refuses to start with `cannot resolve group fips:
Input/output error` (an NSS error where macOS only warns), so the VM
acceptance ran with `node.control.enabled: false`; neither is a DNS change.
Static checks (`check-unsequenced-args.py`, `check-gui-test-gdk-frame.py`,
`check-linux-ci-packages.py`, `git diff --check`, `shellcheck` on both
scripts) passed; build directories were deleted and the submodule
deinitialised afterwards. Version stays **0.1.0 Unreleased**.

## Native datagram API and libfipsnative (`nostrc-9nxwk.4`)

`src/native/` ports upstream `src/native/{mod,registry,seqpacket,protocol,
dgram_probe,link,fdpass}.rs` as the `native` runtime subsystem, registered
in `main.c` after the node (and the TUN service) and before the control
socket, and `src/native/client.c` replaces the Rust client module
(`src/native/client/{mod,codec}.rs`) with **libfipsnative**, a plain-POSIX C
library (static and shared, `include/fips-native-client.h`,
`libfipsnative.pc`) that any program can link without libgo or libfips-core.
The surface is upstream's `docs/reference/native-api.md`; the obligations in
`docs/how-to/write-a-native-api-client.md` are the library's rules.

| Module | Upstream | Owns |
|---|---|---|
| `protocol.{c,h}` | `protocol.rs` | command parsing (serde-shaped refusals), port tiers (0-255 protocol, 256-1023 standard, 49152+ ephemeral), errno names, the 512-byte message clip, serde-ordered reply encoding, the 8192-byte line cap |
| `registry.{c,h}` | `registry.rs` | port ownership, flows, pending flows, `deliver`/`hold`/`accept`/`reject`/`release`/`release_listener`/`expire` (5 s), `pending_per_flow`, `backlog`, `max_flows`, the per-peer quarter share, RFC 6056 ephemeral search |
| `seqpacket.{c,h}` | `seqpacket.rs` | the pair (`SOCK_SEQPACKET` on Linux, `SOCK_DGRAM` on macOS/FreeBSD), buffers, deadlines, the end-of-file rule (`ECONNRESET`, or zero bytes with `POLLHUP` and nothing queued), fiber-parked receive with the 250 ms Darwin close retry |
| `fdpass.{c,h}` | `fdpass.rs` | `SCM_RIGHTS` in the same `sendmsg` as the line; the whole control buffer walked, extras closed, close-on-exec |
| `link.{c,h}` | `link.rs` | delivery outcomes, the nine drop reasons and their `native` counters, `why()` for a failed hand-off write |
| `dgram_probe.{c,h}` | `dgram_probe.rs` | per-kernel measurements the tests assert (close signal, in-flight collection) |
| `api.{c,h}` | `mod.rs` | the socket (control socket's path order with `api.sock`, 0770/`fips`), one owner fiber per connection, reader+feeder fibers per flow, reader+arrivals fibers per listener, the node's datagram sink fiber, owner-directed cancellation at stop, the `show_native_flows` provider |

**Protocol compatibility.** Byte-for-byte with the pinned daemon: requests
`{"command":...,"params":{...}}`, replies `{"status":"ok","data":{...}}` and
`{"status":"error","data":{"errno":"..."},"message":"..."}` in serde struct
order with nested keys sorted, the arrival message with no newline, the same
refusal order for `connect` (reserved tier, bad npub, EMFILE, EADDRINUSE,
exhausted range, shutting down), the same texts for registry refusals and
drop causes, the debug gate refusing `stats`/`inject`/`arrive` by name with
the key that admits them, the 8192-byte command cap ending the connection
with nothing written back, the daemon's copy of a passed descriptor kept
until the next command or the connection's end, and a handed-over flow's
descriptor pinned until the client writes or the listener closes. The
`show_native_flows` rows and the `native` counter family are the existing
control projections, now populated; `received_*` count dispatch outcomes
(delivered, announced, held) and `drop_*` the refusals, as upstream's rx_loop
does.

**Host seam.** With the API running, `hs_native_deliver` hands an
authenticated datagram for an application port (above 1023) to the API's
bounded sink with the peer's x-only key instead of the notice channel;
datagrams for ports 0-1023 (port 256, the IPv6 shim) and every other notice
keep the notice channel, so the IPv6 adapter's path is untouched. Outbound
datagrams leave through `fipsd_node_handlers_send` from the flow's reader
fiber, which seeds the peer's identity at `connect` so no lookup is needed.
`max_payload` is the smallest registered transport MTU less the FIPS
encapsulation and the port header (`fips_framing_payload_limit`).

**libfipsnative.** `fips_native_connect`/`fips_native_listen`/
`fips_native_accept`/`fips_native_send`/`fips_native_recv` and the close,
address, deadline and non-blocking calls, each returning 0 or a negative
errno from upstream's table (`-EADDRINUSE`, `-EADDRNOTAVAIL`, `-EMFILE`,
`-EINVAL`, `-EMSGSIZE`, `-EPIPE`, `-ETIMEDOUT`, `-ECONNREFUSED`; `-EPROTO` for
a daemon not speaking the protocol). bech32 npub conversion and the flat JSON
reader are in the library itself. `examples/native-echo.c` and
`examples/native-send.c` are the two how-to programs in C.

**Tests** (`ctest -R '^FipsdNative'`, no sleeps): `FipsdNativeProtocolTest`
(parse vectors, errno table, exact reply/arrival byte layouts, clip, line cap),
`FipsdNativeRegistryTest` (table tests for the port rules, backlog, ceiling,
quarter share, expiry, ephemeral search), `FipsdNativeSeqpacketTest` (the
pair rule, kernel probes, `SCM_RIGHTS` over a socket pair inside a runtime,
libfipsnative against a fake daemon, bech32 differential against
libfips-core), `FipsdNativeApiTest` (the line-protocol scenarios on a live
node: hand-off, pins, kept descriptors, debug gate, caps, refusal sizes,
provider rows, a self-delivered datagram, cancellation-safe shutdown with
clients still attached), `FipsdNativeTwoNodesTest` (a C client on node A
sends to a C client on node B over the loopback transport and back).
`FipsdInteropUdpTest`/`FipsdInteropUdpV6Test` now also run
`fipsd_native_client_tool` (a C client over libfipsnative) at fipsd's native
API: each Rust->C datagram is accepted and printed by the C client, which
answers with a datagram the harness reads off the Rust daemon's flow while
Rust's `show_native_flows` counts it (the C->Rust application direction the
f66sr.1 gate could not verify). `FIPSD_UDP_INTEROP_RUST_ECHO` may name
upstream's `native-echo` client built from the pinned tree; the harness then
runs it on fipsd's native API and echoes a datagram through it from the Rust
daemon's side.

### Multi-node UDP convergence verification (nostrc-f66sr.2)

`FipsdNodeMulti{Line,Star,Ring,FullMesh}Test` runs independent in-process
nodes over **real loopback UDP**: respectively 3, 4, 5 and 5 nodes. This ports
the small explicit-graph/baseline/delivery subset of upstream's
`testing/chaos/sim/topology.py`, `scenarios/ethernet-mesh.yaml` and
`src/node/tests/spanning_tree.rs`, not netns, netem, Ethernet discovery or the
mesh-lab pressure profiles. Each connected component must elect its minimum
NodeAddr; parents must be real edges, depths and complete self-to-root
coordinate chains must agree, and neighbors' stored announcements must have
caught up. Filter cardinality gates aggregate readiness. Final assertions
check **exact positive membership on each correct tree edge**, using retained
immutable dataplane snapshots (never mutable dispatcher state); Bloom negatives
are not asserted because false positives are legitimate. Exact membership is
observed against the original convergence deadline, since directional aggregates
can trail the topology notice; application sends are never retried. Every ordered pair
resolves an address and delivers a distinct authenticated datagram with matching
source, ports and payload. The application payload is sent once, after both
endpoints report a proven session and the diagnostic has recorded its adoption
baseline; first-write queuing during XK is not asserted. A cold indirect control
probe also requires a real lookup result and nonzero bloom fanout.

A parent-edge removal must reconverge, including independent component elections
when line/star bridges are cut, followed by reconvergence after restoration.
Line/star/ring datagrams include nonadjacent pairs. The complete mesh's edge is
removed **before** opening sessions, so cold lookup and application delivery
across the missing direct edge prove two or more hops. Recovery of an already
established session's stale coordinate cache after a cut is a different
routing-error contract and is not claimed by this test.

`FipsdInteropMultiTest` extends `udp_interop.py`, reusing its daemon lifecycle,
control-socket protocol, native flow and deadline-bounded observation. It runs
line/star with 2 C + 1 Rust, and ring/full-mesh with 3 C + 2 Rust. Both daemons'
control sockets verify the graph/tree/filters, every ordered diagnostic pair's
address resolution and an authenticated echo, and Rust native datagram arrivals at
C (exact datagram/byte counter deltas), including a physically nonadjacent pair.
The session and payload stages are explicit: a diagnostic must observe the
proven session at both endpoints and record its traffic-adoption baseline
before the native payload is sent. A control session row's `npub` may be absent
before the identity is cached; readiness uses `remote_addr` and proven state.
Reciprocal diagnostics run together, rather than probing a remote half left
behind by an earlier diagnostic's intentional teardown.

C waits select all owners' notice channels against one absolute deadline; an
unrelated notice cannot extend it. Python control exposes snapshots rather than
a push subscription: observations wake on child output or a bounded condition
wait, and each socket operation shares the observation deadline. Neither
harness uses a sleep or elapsed duration as evidence of completion.

#### Bounds and fixes

At the pin `6aa80504baa0fa312b392009eea0e3e59fb6fe4c`, upstream
`docs/design/fips-spanning-tree.md` describes approximately **D × 0.5 s** for
tree announcements and approximately **one 1 s tick per hop** for Bloom
propagation (the 500 ms debounce is a floor). Its 30 s STP receiver-report
fallback is a delivery/retry parameter, **not a universal wall-clock convergence
guarantee**. `drain_all_packets` allows up to 200 fast 10 ms passes and 20 flush
cycles of 550 ms + 20 ms + up to twenty 10 ms subpasses (nominally 17.4 s,
excluding scheduling). These lossless local tests enforce a conservative
**30 s** tree/aggregate-filter observation window without reducing the default
500 ms pacing or 1 s tick, and the chaos delivery assertion's **60 s** per-pair
bound. Optional cost-based parent optimization has upstream's 30 s hold-down
and 60 s reevaluation cadence; it is not a root change or a link flap.

The integration keeps one host implementation per concern (core/wire
algorithms are unchanged):

* Master's `hs_lookup_initiate_outcome(h, target, &fanout)` and existing bool
  `hs_lookup_initiate(h, target)` wrapper are retained verbatim. The probe job's
  separate once-only outcome/fanout hand-off preserves null fanout on BloomMiss
  and Suppressed. Both the chaos harness's BloomMiss regression and the cold
  indirect multi-node lookup regression exercise this implementation.
* Master's responder `session.c::warm_coords()` remains the setup entrypoint,
  but delegates to shared `dataplane/coord_warm.c` hint intake. There is no second
  blanket endpoint warmer. Setup and transit hints share foreign-root/self
  guards, bounded cache storage and per-carrier funding (1024 weight/s, 4096
  burst, initially empty); unfunded hints remain in probation and do not replace
  verified entries. Transit setup/ack/CP intake and authenticated SessionAck or
  replay-admitted CP source statements are retained because master's setup-only
  path does not cover them. Authenticated statements use upstream's uncharged
  `insert_stated` path, preventing unrelated probation hints from evicting a live
  session's return route; they are not signed/verified lookup entries.
  `FipsdCoordWarmTest` covers decoder-buffer ownership, foreign-root/self and
  malformed/unauthenticated endpoint guards, preservation of an existing verified
  entry through the shared hint entrypoint, probation pressure and funded refill.
  Master's responder warming and lookup-outcome cases in `FipsdNodeHandlersTest`
  are preserved, not replaced by the new unit test.

#### Opt-in soak and local commands

Without `FIPSD_UDP_INTEROP_RUST_BINARY`, mixed tests return **77** (a skip, not a
pass). A matching echo explicitly classified as sub-millisecond (positive
`zero_samples` and `reports_seen`) proves a route but is not claimed as a
positive latency measurement; other failed diagnostic stages still fail.
Build the pinned daemon read-only into disposable Cargo output:

```bash
CARGO_TARGET_DIR=/tmp/fips-rust-target-multi cargo build --locked --release --bin fips \
  --manifest-path /path/to/pinned-fips/Cargo.toml
```

With `src` set to this checkout's absolute path, configure out of tree, initialize
nostrdb first, and build the complete FIPS test closure:

```bash
git -C "$src" submodule update --init --depth 1 third_party/nostrdb
cmake -S "$src" -B "$src/.build-multi-asan" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS=-Werror \
  -DBUILD_FIPS=ON -DBUILD_APPS=OFF -DBUILD_NOSTR_GTK=OFF \
  -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_GROUNDHOG=OFF \
  -DBUILD_TESTING_FRAMEWORK=OFF -DBUILD_MARMOT_GOBJECT=OFF \
  -DGNOSTR_ENABLE_ASAN=ON -DGNOSTR_ENABLE_UBSAN=ON \
  -DFIPSD_UDP_INTEROP_RUST_BINARY=/tmp/fips-rust-target-multi/release/fips
cmake --build "$src/.build-multi-asan" \
  --target fipsd_tests fips_core_host_test fips_core_skeleton_test -j 8
ctest --test-dir "$src/.build-multi-asan" -R '^Fips' --output-on-failure -j 1
# Separate TSan directory: same switches, replacing the two sanitizer switches
# above with -DGNOSTR_ENABLE_TSAN=ON. Never combine TSan and ASan.
```

`FipsdInteropSoakTest` is **DISABLED by default**, not part of the ordinary test
run. Enable it explicitly with `-DFIPSD_ENABLE_INTEROP_SOAK=ON`, then run
`ctest --test-dir "$src/.build-multi-asan" -R '^FipsdInteropSoakTest$' --output-on-failure`.
Alternatively, choose any duration/topology directly:

```bash
python3 "$src/fipsd/tests/udp_interop.py" \
  --fipsd "$src/.build-multi-asan/fipsd/fipsd" \
  --rust /tmp/fips-rust-target-multi/release/fips --topology ring --soak-minutes 10
```

The soak checks a stable minimum root, the complete expected live-link set and
unchanged link IDs/directions, valid parent paths, monotone cumulative counters,
and clean child exits/no sanitizer reports. Legitimate cost-based parent
optimizations are counted separately, not mistaken for root changes or flaps.
Clean up task-owned build/Cargo output and `git -C "$src" submodule deinit -f --all`
after local verification. This test/internal-fix change remains **fipsd 0.1.0
Unreleased**, with no core version, release tag or wire change.

#### Rebased integration verification

Rebased on local staging master **bdcab3ece**, including chaos-harness commit
**a9f774349**. There is one lookup implementation (master's enum outcome helper
and bool wrapper, unchanged) and one shared unverified hint implementation;
master's setup entrypoint delegates to it. Transit and authenticated statement
intake remain, since setup-only warming does not cover those paths. The explicit
verified-entry-preservation test exercises the shared setup/transit entrypoint.
Master's `FipsdNodeHandlersTest` regressions remain unchanged and passed alongside
`FipsdCoordWarmTest`.

Local macOS arm64 verification on 2026-11-16 used separate out-of-tree Debug
builds with `-Werror`, ASan+UBSan or TSan, and Rust release binary pin
`6aa80504baa0fa312b392009eea0e3e59fb6fe4c` built with Cargo `--locked`.
Both full suites ran **in the foreground, one sanitizer suite at a time, to
completion** using `ctest -R '^Fips' --output-on-failure -j 2`:

| Gate | Passed | Capability skips | Disabled soak | Failed | Elapsed |
| --- | ---: | ---: | ---: | ---: | ---: |
| ASan/UBSan | 158 | 4 | 1 | 0 | 285.58 s |
| TSan | 158 | 4 | 1 | 0 | 389.36 s |

The capability skips were `FipsdControlInteropTest`,
`FipsdTunPrivilegedSmokeTest`, `FipsdTcpInteropTest`, and
`FipsdIpv6PrivilegedSmokeTest`. The ordinary run left `FipsdInteropSoakTest`
disabled. `FipsdNodeHandlersTest` passed in 17.32/19.05 s (ASan/TSan),
`FipsdCoordWarmTest` in 0.19/0.24 s, and all four C topology tests and the mixed
matrix passed; the mixed matrix took 114.65/156.16 s. No sanitizer warning
appeared in the complete captured test outputs. In particular, the known
`libnostr/src/relay.c:1064` race **nostrc-xqwgo** did not fire in these runs;
it remains an independently tracked relay issue, not a claimed fix here.

ASan/UBSan convergence observations below include neighbor-announcement and
aggregate-filter readiness, in seconds. Each phase remains below the 30 s test
deadline and the upstream integration fixture's nominal 17.4 s pass budget:

| Nodes / topology | Initial | Parent edge removed | Restored |
| --- | ---: | ---: | ---: |
| 3 C line | 2.027 | 0.002 | 1.951 |
| 4 C star | 3.010 | 0.919 | 2.005 |
| 5 C ring | 5.008 | 3.881 | 1.010 |
| 5 C full mesh | 2.053 | 2.971 | 0.851 |
| 2 C + 1 Rust line | 1.999 | 0.953 | 1.884 |
| 2 C + 1 Rust star | 2.040 | 1.065 | 1.833 |
| 3 C + 2 Rust ring | 3.019 | 2.920 | 1.087 |
| 3 C + 2 Rust full mesh | 3.067 | 0.986 | 1.011 |

The duplicate audit found exactly one coordinate source entry, one declaration
of each new test/option, one multi-node README section and one decision line for
each task in the version manifest. The chaos/IPv6 build and documentation
additions were retained; one stale duplicate protocol-review sentence was
removed. All four direct static checks passed (`check-unsequenced-args.py`,
`check-gui-test-gdk-frame.py`, `check-linux-ci-packages.py`, `git diff --check`).
No beads command or push-hook wrapper was run. Build/Cargo output is disposable;
submodules are deinitialized after verification. Version remains **0.1.0
Unreleased**.

The original pre-rebase ASan/UBSan mixed-ring soak completed 600 seconds with
5,363 observations, a stable root/live-link set, monotone counters and two valid
parent optimizations. That soak was not rerun for this integration reconciliation;
it remains opt-in. These local loopback results are not Linux netns, hosted CI or
deployment proof. The staged payload tests do not qualify the separate
first-write-during-XK queue contract or stale established-session cache recovery
after a cut.

## fipsctl: the control client in C (`nostrc-iq1n9.1`)

`fipsd/src/fipsctl/` is upstream `src/bin/fipsctl.rs` in plain POSIX C over
jansson and libfips-core (identities, addresses, key files): no libgo and no
`fipsd_lib`. `fipsctl_lib` holds everything but `main()`, so
`fipsd_fipsctl_test` links the code the tool runs; the `fipsctl` executable
installs beside `fipsd`. The command contract is upstream
`docs/reference/cli-fipsctl.md`: `show status|peers|links|tree|sessions|bloom|mmp|cache|connections|transports|routing|identity-cache|native-flows`,
`acl show`, `stats list|metrics|peers|history <metric> [--peer] [--window]
[--granularity] [--plot]`, `keygen [-d DIR] [-f] [-s]`, `address [identity]
[-k PATH]`, `connect <peer> <address> <transport>`, `disconnect <peer>`, `path
show|pin|unpin`, `probe <target> [--json] [--timeout]`, `-s/--socket`, `-V`,
`--version`, `-h/--help`, and `profile tick on|off|status` only behind
`-DFIPSCTL_PROFILING=ON` (upstream `--features profiling`; a stock daemon
answers it `unknown command`).

| Module | Owns |
| --- | --- |
| `commands.c` | clap-shaped parsing (global options before the subcommand, per-scope `--help`, usage errors exit **1** as the reference documents; upstream's clap binary exits 2, see `libfips-core/UPSTREAM.md` I-2), version strings (`fipsctl <version> (rev <hash>)`, `--version` adds `target:`), request building, the driver. |
| `client.c` | Default socket resolution (`/run/fips` -> `/var/run/fips` on macOS/FreeBSD -> `$XDG_RUNTIME_DIR/fips` -> `/tmp/fips-control.sock`, by existence, as `config.c` does), the IPv6 adapter's `-ipv6tun` sibling (symlinks followed first), one request/one response with 5 s deadlines and upstream's error texts, and the `show_status` composition: the adapter is asked only when the daemon leaves out `tun_state`/`tun_name`/`effective_ipv6_mtu`, with `disabled`/`-`/`0` when nobody answers. |
| `render.c` | A serde_json-exact printer: lexical keys, 2-space pretty form, ryu's float layout (`18.0`, `1e+16`, `1e-6`), lowercase `\u00xx` control escapes, raw UTF-8; u64 tokens above `INT64_MAX` pass through jansson untouched (`fipsctl_json_load`); the `--plot` sparkline. |
| `npub.c` | `npub1` in any letter case, hosts-file lookup (`/etc/fips/hosts`, `/usr/local/etc/fips/hosts` on macOS/FreeBSD; `hostname npub` lines, later lines win, invalid lines skipped), the `fd00::/8` connect guard, `address` from an npub, an nsec/npub file or `fips.key`/`fips.pub` in the platform key directory, and `keygen` (0600/0644, `O_NOFOLLOW`, symlink refused, secrets wiped). |
| `path.c`, `probe.c` | The `path_*` requests checked against `control_golden/fipsctl_path_requests.json`; the probe client: start, poll every 250 ms under the daemon's budget plus 5 s (or `--timeout`), cancel on expiry, the live stage block (redrawn in place on a terminal, each row printed once as it settles elsewhere), the path/cleanup/overall sections and the closing notes, every string upstream's. |

Running the documented commands end to end exposed one fipsd defect, fixed
here: `hs_control_command` (`src/node/handlers/provider.c`) routed `path_unpin`
through the transport lookup, so `fipsctl path unpin <peer>` always failed with
`unknown transport`; `path_pin` read `addr` where the protocol and the client
send `address`, so a one-path pin silently pinned the whole transport; and the
numeric transport id `path show` prints was not resolved when it arrived as the
string the client sends. Two daemon-side shape differences remain, recorded in
`libfips-core/UPSTREAM.md` (I-4) and the bead rather than papered over by the
client: fipsd's `display_name` is the short node address where upstream prefers
the short npub, and fipsd's `path show` names a bare `transports.udp` instance
`"udp"` where upstream reports `null` and accepts only its numeric id.

Verification (`ctest -R Fipsctl`, all in `fipsd_tests`):

- `FipsdFipsctlTest` (`tests/test_fipsctl.c`): parsing, requests, the 23
  control goldens as byte oracles for the pretty printer (they are serde
  pretty output), floats/u64/escapes, composition against fake node and
  adapter sockets (upstream `tests/fipsctl.rs`, plus the symlink case), the
  path fixture (`tests/fipsctl_path.rs`), identities/hosts/key files/keygen
  (`tests/fipsctl_npub.rs` and the inline tests), the probe ceiling
  (`tests/fipsctl_probe.rs`), every probe rendering test upstream carries, and
  the full human report for `control_golden/probe_report.json`.
- `FipsdFipsctlE2eTest` (`tests/fipsctl_e2e.py`): every documented subcommand
  through the built `fipsctl` against two real fipsd daemons on loopback UDP
  (`node.path.enabled: true`): local commands, usage errors, the adapter
  composition over a fake status socket (directly and through a symlink),
  the link, `path pin`/`unpin`, per-peer history, probes both ways, the
  deterministic bloom-miss probe of an unannounced address, disconnect.
  Event-driven waits on the control socket; no sleeps.
- `FipsdFipsctlInteropTest` (`tests/fipsctl_interop.py`, SKIP 77 without
  `FIPSD_UDP_INTEROP_RUST_BINARY` and a Rust `fipsctl` from
  `FIPSD_FIPSCTL_EXECUTABLE` or a cargo build of `FIPSD_UPSTREAM_CONTROL_DIR`):
  the C `fipsctl` against the pinned Rust daemon and the Rust `fipsctl`
  against fipsd, every subcommand, each pair of outputs compared byte for byte
  (or parsed and compared after redacting volatile fields where the daemon's
  own state moved between the two reads), the unannounced-address probe
  rendering compared with timing figures masked, `path pin`/`unpin` and
  probes through both clients on both daemons, and keys from each client's
  `keygen` read by the other's `address`.

## fips-gateway: the LAN gateway in C (`nostrc-iq1n9.3`)

`fipsd/src/gateway/` is upstream `src/gateway/*` and `src/bin/fips-gateway.rs`.
The `fips-gateway` executable runs beside `fipsd` and reads the same
`fips.yaml` (`gateway:` block). It has two halves:
- **Outbound.** LAN hosts resolve `<npub>.fips` through its DNS proxy (default
  `[::1]:5365`) to a virtual IPv6 address from `gateway.pool`, and kernel
  nftables NAT carries the flow into the mesh through `fips0`.
- **Inbound.** `gateway.port_forwards[]` exposes LAN services on the gateway's
  mesh address.

It is Linux-only. Its portable modules (`fipsd_gateway_core`: pool,
conntrack, state, NAT rendering and worker, net, DNS proxy, control,
shutdown) build and are tested on every platform. `main.c` and the
libnftables backend `nat_nft.c` build on Linux when `pkg-config` finds
`libnftables` (`libnftables-dev`), and `-DFIPSD_BUILD_GATEWAY=OFF` skips
them.

nftables goes through **libnftables**, the library `nft` is built on, rather
than through libnftnl/libmnl directly:
- Each rebuild is one JSON document, sent as one netlink batch that the kernel
  applies as one transaction, the guarantee upstream's rustables batch gives.
- The rule shapes are golden-tested text (`tests/gateway_golden/`), and the
  goldens load in real nftables.
- The library does the buffer sizing and acknowledgement bookkeeping upstream
  does by hand.

The paths and limits are upstream's:

| What | Value |
| --- | --- |
| Control socket | `/run/fips/gateway.sock` (`show_gateway`, `show_mappings`; group `fips`, `0770`) |
| NAT table | `inet fips_gateway` |
| Pool state | `/var/run/fips-gateway/pool.json` (tmpfs only) |
| Pool route | `local <pool> dev lo` |
| Mapping ceiling | 1000 live mappings |
| New-name rate | a bucket of 50, refilled at 10/s |
| Tick | 10 s |

One case differs, because C cannot abort on an allocation failure:
- **Pool:** it reserves before it mutates, so a failure leaves the pool as it
  was and the query gets `SERVFAIL`.
- **NAT manager:** a failure stops the worker, and the gateway exits 1 for its
  restart.

[`docs/fips-gateway.md`](docs/fips-gateway.md) has the rest:
- the module map and concurrency;
- the DNS, pool, NAT, control and shutdown behaviour;
- the differences from upstream;
- the deployment recipe (`deploy-gateway.md`) step by step;
- the troubleshooting table (`troubleshoot-gateway.md`).

The unit template `packaging/systemd/fips-gateway.service.in` is generated
and installed with the executable. The packages do not ship the gateway yet:
`packaging/common/cmake-args.sh` turns it off.

Verification (`ctest -R FipsdGateway`, all in `fipsd_tests`):

- Eight unit suites in `fipsd_gateway_test`, one ctest each: `Pool` (the 56
  `pool.rs` tests), `Conntrack`, `State`, `Nat` (goldens and the worker),
  `Net`, `Dns`, `Control`, `Shutdown`. They are free of sleeps and run under
  ASan/UBSan and TSan.
- `FipsdGatewayPrivilegedSmokeTest` (`tests/gateway_netns_smoke.py`): root
  only, and it skips (77) unless `FIPSD_GATEWAY_PRIVILEGED_SMOKE=1` is set.
  It reproduces `deploy-gateway.md` in three network namespaces:
  - the gateway host, running fipsd A and fips-gateway;
  - mesh node B;
  - an unmodified LAN host.

  It checks every step of the how-to:
  - the start-up log, the pool route and the table;
  - outbound `dig`, ping, TCP and UDP through the NAT;
  - the control socket;
  - the inbound forwards, with and without the mesh firewall baseline;
  - a clean stop that removes every rule, route and NDP entry;
  - a restart that keeps the previous run's addresses held.


## fipstop dashboard

The optional ncurses dashboard installs alongside fipsctl. It has ten tabbed
panes, role-sorted peer/link views, independently scrollable internals,
historical graphs and a separate gateway view. Run **fipstop -s PATH** (default
refresh: two seconds); press **?** for keys and **q** to quit.

CMake enables FIPSD_BUILD_FIPSTOP when pkg-config finds ncursesw or ncurses.
Packaging needs **libncurses-dev** (Debian/Ubuntu) or **ncurses** (Alpine);
the workflow owner must add the dependency to CI package lists. The new
component does not change the daemon or fipsctl version/API. See
[CLI, panes, keys and regression modes](docs/cli-fipstop.md).

## Ethernet transport (`nostrc-gfi8h.5`)

`src/transport/ethernet/` ports the pinned upstream Ethernet wire and raw I/O:
`ethernet.c` owns binding, receive/beacon fibers and send leases;
`io_linux.c` owns AF_PACKET/SOCK_DGRAM; `io_macos.c` owns BPF;
`neighbor.c` owns codecs and the bounded discovery buffer; `stats.c` feeds
`stats_common`. Ethernet registration precedes the node registry seal.

Linux needs root or `CAP_NET_RAW` (`setcap cap_net_raw=ep /path/to/fipsd`).
macOS needs read/write access to `/dev/bpf*`; the daemon never invokes sudo.
An existing/up interface whose privileged open fails refuses transport startup
with state `failed` and a specific raw-socket/BPF error. The normal supervisor
policy keeps the host/control service available even when the node is not ready. An absent interface starts absent and binds on
network notifications when it returns. `optional` controls required-interface
health reporting, not privilege checks. Carrier loss alone is not absence.

```yaml
transports:
  ethernet:
    interface: eth0
    ethertype: 0x2121
    # mtu: 1357               # optional ceiling; default interface MTU - 3
    recv_buf_size: 2097152
    send_buf_size: 2097152
    listen: true              # receive neighbour beacons (alias: discovery)
    announce: false           # broadcast our identity immediately, then periodically
    auto_connect: false       # opt in to dialing valid discovered identity hints
    accept_connections: false # opt in to inbound IK
    beacon_interval_secs: 30  # minimum 10
    optional: false
    role: normal
```

Named instances use `transports.ethernet.<name>`. Explicit peer addresses and
control `connect`/`path_pin` addresses use `interface/aa:bb:cc:dd:ee:ff` (upstream syntax), not IPs.
The C transport also accepts a bare MAC when the instance already names its
interface; a supplied interface prefix must match that binding.
The wire is EtherType 0x2121 by default, data `00 || u16le(length) || FMP`,
and beacon `01 01 || xonly_public_key[32]`. Decode trims NIC padding before
AEAD. Group/multicast source MACs are dropped before either dispatch path;
zero/group destination MACs are never data peers. The 1024-entry MAC-keyed
neighbour buffer refreshes existing entries even when full, drains in last-
sighting order and clears on detach. Beacons are unauthenticated identity
hints; ordinary IK, ACL and admission rules still apply.

Binding/rebinding uses shared watcher/presence facts and network-change events,
including changed ifindex/MAC and a Linux live-socket bound-index probe for
unregister/return with the same index. Ethernet disables the watcher's periodic
interface-read ticker; only bind retry and health-stability deadlines remain.
Raw I/O failure also wakes the owner. Detach cancels I/O, joins receive/beacon
fibers and send leases, then resets/closes the descriptor. Medium comes from
`medium.c` at bind/change and feeds the existing path preference policy:
physical wired/Wi-Fi are distinguished; veth/bridge/VLAN remain unknown.

Linux kernel drop samples reset and are summed across bindings; BPF drop
samples are cumulative and converted to deltas. `show_transports` emits the
13 upstream Ethernet counters (`frames_*`, bytes/errors, `beacons_*`,
`kernel_drops`, `group_drops`) and the shared interface presence object.
Darwin uses upstream's 20-byte timeval32 BPF header, 4-byte record alignment,
512-KiB buffer request before bind, complete Ethernet headers on write, and
an EtherType kernel filter. Truncated captures are discarded, not passed
partially to AEAD (a stricter behavior than upstream's copying minimum).

### Focused gates

`FipsdEthernetTest` covers wire/padding/beacon vectors, group drops, neighbour
capacity/refresh/order, BPF ABI/batch/truncation and injected presence/index/
MAC/same-index-detached-socket transitions without privileges or sleeps.

```sh
ctest --test-dir "$build" -R '^FipsdEthernet' --output-on-failure
# Linux, root/CAP_NET_ADMIN+CAP_NET_RAW and iproute2; two netns joined by veth:
sudo env FIPSD_ETHERNET_PRIVILEGED_SMOKE=1 python3 "$src/fipsd/tests/ethernet_privileged.py" \
  --fipsd "$build/fipsd/fipsd" --native-tool "$build/fipsd/tests/fipsd_native_client_tool" \
  --rust /path/to/pinned/release/fips
# macOS, without sudo; requires accessible BPF and a peer emitting FIPS frames:
FIPSD_ETHERNET_BPF_INTERFACE=en0 "$build/fipsd/tests/fipsd_ethernet_test" --bpf
```

For Linux TSan, configure `-DGNOSTR_ENABLE_TSAN=ON
-DNOSTR_USE_SYSTEM_NSYNC=OFF` after initializing `third_party/nsync`. This
builds instrumented nsync with the repository's TSan semaphore backend rather
than treating an uninstrumented distro lock library as visible synchronization.

The registered privileged smoke returns 77 with a prerequisite explanation
unless explicitly enabled on Linux as root. It checks C/C and optionally
C/Rust in both IK/XK role assignments, exact native application datagrams in
both directions, interface-derived MTU, delete/recreate rebind with announce
**off**, then a beacon-driven auto-connect/rebind scenario with announce on,
clean shutdown and namespace cleanup. Snapshot observations are
bounded by deadlines and paced by replies; no sleeps stand in for completion.
BPF smoke returns 77 when BPF/interface access is unavailable, but fails if
open/bind succeeds and no complete FIPS frame arrives within ten seconds.

## Incremental control snapshot (nostrc-oetfk)

The dispatcher's control-snapshot build no longer copies the stats history or
sweeps the whole coordinate cache on each publish. Snapshots retain shared,
immutable, ref-counted history views that the dispatcher brings up to date by
replaying logged history updates. The cache rows and stats come from one
O(len) `fips_coord_cache_scan` pass. On the C→C loopback, `publish_snapshot`
went from a 2.3 ms average with tens-of-ms tails to about 70 us, with p99 at
or below 1 ms; see [docs/perf.md](docs/perf.md#incremental-snapshot-build-nostrc-oetfk).
The snapshot layout, the control output and the goldens are unchanged.
`FipsdStatsViewTest` checks that replayed views match the live history on
every query under churn, that held views stay immutable, the fallbacks, and
cross-thread release under TSan. Version decision: **fipsd stays 0.1.0
Unreleased**; the change folds into the initial daemon, with no manifest,
declared-version or release/tag change.
