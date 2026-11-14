# libfips-core

The C11, sans-IO protocol core of FIPS. This component inherits the boundary
and rules of upstream FIPS `core/README.md`, `core/src/README.md` and
`core/src/lib.rs`: the same protocol computation should work unchanged in a
daemon, an application or an embedded host. This is an incremental C port,
not a Rust FFI wrapper or a complete working protocol implementation.

## Boundary and rules

- **No I/O or OS calls.** No sockets, devices, files, configuration parsing,
  logging, signal handling or platform-specific branches. Those belong to
  `fipsd/` or another host.
- **No threads or runtime dependency.** Calls are synchronous and return
  promptly. Return actions (bytes to send, deadlines to schedule, events or
  errors), rather than waiting, polling or starting background work.
- **Time comes from the host.** Accept timestamps as inputs; never read a
  clock or run a timer. Tests use synthetic timestamps, not sleeps.
- **Entropy comes from the host.** Never read an OS, global or thread-local
  RNG. Keys, nonces and context-blinding seeds must use host-supplied
  cryptographically secure bytes. Merely linking a crypto library does not
  authorize its random-source APIs or implicit initialization side effects.
- **Pass state explicitly.** Avoid mutable global state, locks and platform
  atomics that exclude smaller targets. Hashes keyed by peer-controlled input
  must be bounded and, when appropriate, keyed with host-supplied entropy.
- **Peer input is untrusted.** Check lengths, arithmetic and buffer bounds;
  return errors instead of aborting. Bound allocations by local policy, never
  solely by a peer's length field. Wipe secrets on all error and release paths.
- **Keep dependencies narrow.** Only libsecp256k1, libsodium and OpenSSL
  `Crypto` (EVP SHA-256/HKDF) are wired here. No libgo, libfiber, libnostr,
  GLib or host integrations. Current dependencies target hosted builds;
  bare-metal portability is an architectural goal, not yet verified.

## Layout and build contract

- `include/fips/core.h`: public version function; generated export and version
  headers are installed alongside it.
- `include/fips/host.h`, `src/host.c`: host interfaces and time arithmetic;
  caller-owned time and entropy.
- `include/fips/proto/`, `src/proto/`: protocol errors and bounds-checked codec.
- `tests/`: linked version, host-interface, codec and byte-compatible fake tests.
- `libfips-core.pc.in`: installed pkg-config contract, including private crypto
  requirements for static consumers.

Enable with the top-level `BUILD_FIPS` option (default ON). The `fips_core`
target follows `BUILD_SHARED_LIBS`: OFF produces an archive, ON a versioned
shared library. Install headers under `include/fips` and query pkg-config as
`libfips-core`. `CMakeLists.txt` owns the component's initial version, 0.1.0.
Warning flags honor nostrc's shared `GO_WARNINGS_AS_ERRORS` option; sanitizer
flags use its shared `GO_ENABLE_*` helpers.

Future protocol tests must drive real state machines with fixed entropy and
synthetic time, checking returned actions and upstream differential vectors.
Higher-level protocol modules are left to later tasks. Upstream provenance
is maintained separately in `UPSTREAM.md`.

## Host interfaces and ownership

`fips/host.h` mirrors the pinned Rust `EntropySource`, `Instant` and `Clock`
traits. The host reads `fips_clock.now(ctx)` and passes copyable `fips_instant`
values into core calls; the core never reads a clock or RNG itself. An instant
holds two opaque words and an immutable arithmetic vtable (comparison,
saturating difference, checked add/subtract). Only instants from one clock
domain may be compared or combined. Duration is normalized seconds plus
nanoseconds, retaining Rust's full `u64`-second range. `fips_deadline` caps at
100 years, then halves an overflowing step until it fits; `fips_backoff`
saturates its doubling factor at `UINT32_MAX`, its product at `Duration::MAX`,
then caps the result. Neither helper reads the clock.

This establishes the convention for later core APIs: caller-provided buffers
with explicit capacities and lengths, borrowed caller-owned contexts, and
status-code returns with out-parameters unchanged on failure. Copying a vtable
does not transfer ownership. Retained contexts/vtables must outlive their core
users, and callers provide synchronization. Future allocating constructors must
provide matching explicit destroy functions; allocations must never cross
allocator boundaries. These host interfaces allocate nothing and need no destroy.
Required pointers, normalized durations and compatible clock domains are caller
preconditions. Entropy fill and clock reads are infallible trait operations;
a host unable to fulfill them must fail at its boundary, never invent a value.

`fipsd/src/host_os.c` supplies `CLOCK_MONOTONIC` (including modern macOS) and
libsodium `randombytes_buf`. Construction performs no I/O; actual entropy use
initializes libsodium and actual clock/RNG failures terminate at the host edge.
The skeleton main links the adapters without reading them. Test-only adapters
in `tests/fakes/` use whole milliseconds starting at zero (advance explicitly),
fixed bytes, and seeded SplitMix64. Seeded fills emit little-endian words and
discard the final partial word's unused bytes on each call, just like Rust.
They are predictable and must never be used as production entropy.

## Differential oracle

The test-only [differential harness](tests/diff/README.md) validates the pinned
Rust fixtures and replays implemented families. Run `ctest -R '^FipsDiff' -V`
from the build directory; explicit SKIPs are not protocol parity.

## Proto codec

`fips/proto/error.h` mirrors every pinned Rust proto Error variant and payload,
including nested coordinate/bloom errors; `fips_proto_error_name()` returns the
Rust variant name. `fips/proto/codec.h` supplies allocation-free LE Reader/Writer
primitives. Reader borrows input and reports cumulative `MessageTooShort`
offsets. Writer borrows a fixed-capacity destination rather than growing a Rust
Vec, returning `MessageTooLong` on insufficient capacity. Failures preserve the
cursor and result buffers; success preserves the optional error out-parameter.
`advance` is checked in C, and overflowing required offsets reject with a
saturated `SIZE_MAX` diagnostic. Byte operations have no implicit length prefix.
