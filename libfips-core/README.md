# libfips-core

The C11, sans-IO protocol core of FIPS. This component inherits the boundary
and rules of upstream FIPS `core/README.md`, `core/src/README.md` and
`core/src/lib.rs`: the same protocol computation should work unchanged in a
daemon, an application or an embedded host. This is a component skeleton,
not a working protocol implementation or a Rust FFI wrapper.

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
- `src/core.c`: version implementation only; no protocol code yet.
- `tests/`: a CTest placeholder checking the linked library's version.
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
Host-interface vtables and protocol modules are intentionally left to later
tasks. Upstream provenance will be maintained separately in `UPSTREAM.md`.
