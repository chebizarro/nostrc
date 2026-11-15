# fipsd

The host daemon for the C FIPS port. `libfips-core` owns pure protocol
computation; this component drives it using libgo concurrency, the `go_fiber`
runtime and libnostr. Sockets, clocks, secure randomness, configuration,
logging, signals, privileges and platform-specific work belong to this host,
never to the core.

## What exists today (process skeleton, nostrc-9keoz.1)

| File | Role |
| ---- | ---- |
| `src/main.c` | Entry point: parse flags, open the log first, umask, bring the runtime up, run until a stop, map the result to an exit code. |
| `src/cli.{c,h}` | `getopt_long` parsing of exactly the documented flags; version and help text; `fipsd_options` (`--config`, `--log-file`) captured for the configuration loader (nostrc-9keoz.2). |
| `src/log.{c,h}` | Structured logging: levels, UTC timestamps, module tags, injectable sink, size-rolled file sink. |
| `src/runtime.{c,h}` | Fiber runtime bring-up, signal fiber, root `GoContext`, reload channel, subsystem vtable, orderly bounded shutdown. |
| `src/privilege.{c,h}` | umask, `fips` group resolution, socket/directory ownership and mode helpers, no-op privilege-drop hook. |
| `src/host_os.{c,h}` | `fips/host.h` adapters: `CLOCK_MONOTONIC` clock, libsodium entropy. |
| `cmake/BuildInfo.cmake` | Build-time capture of the git revision and target triple for the version strings. |

Not implemented yet: YAML configuration (`9keoz.2`), the control socket
(`9keoz.3`), transports, the node. `-c FILE` is accepted and recorded but the
file is not read; the daemon logs that and runs the bare runtime.

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

Environment (interim, until the configuration loader lands): `FIPSD_WORKERS`
(scheduler worker count, 1..64), `FIPSD_LOG_LEVEL` (`error|warn|info|debug|trace`).

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
