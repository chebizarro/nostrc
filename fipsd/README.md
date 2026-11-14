# fipsd

The host daemon for the C FIPS port. `libfips-core` owns pure protocol
computation; this component will drive it using libgo concurrency, the
`go_fiber` runtime and libnostr. Sockets, clocks, secure randomness,
configuration, logging and platform-specific work belong to this host, never
to the core. Fiber bodies must use fiber-parking I/O or dispatch blocking
work to libgo's blocking executor.

This is only a skeleton: `src/main.c` starts the background fiber runtime,
prints the daemon and core versions, requests shutdown and joins the runtime.
It does not yet run a node, open sockets, publish Nostr events, parse
configuration or implement TUN/control interfaces.

`include/` is reserved for private host interfaces. `tests/` runs the real
executable and checks its version output and successful startup/shutdown.
There are no sleeps or retries in the placeholder test.

The top-level `BUILD_FIPS` option defaults ON. `fipsd` is configured only on
platforms with a `go_fiber` target (currently not Windows); `fips_core` remains
available independently of that runtime. Install the daemon under `bin`.
`CMakeLists.txt` owns the component's initial version, 0.1.0. Warning flags
honor nostrc's shared `GO_WARNINGS_AS_ERRORS` option and sanitizer flags reuse
its shared `GO_ENABLE_*` helpers.

Host-interface vtables (`fips/host.h`), protocol implementations and operational
daemon behavior are separate follow-up tasks.
