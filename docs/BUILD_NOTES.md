# Build Notes, Hardening Flags, and Dependency Versions

This project enables hardening flags and sanitizer/fuzzing toggles to support a secure build by default.

## Compiler/Linker Hardening

Enabled globally in `CMakeLists.txt` (GCC/Clang):

- `-fstack-protector-strong`
- `-D_FORTIFY_SOURCE=2`
- `-fno-strict-aliasing`
- `-fPIE` (Linux)
- Linker (ELF): `-pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack`

Libraries are built with `CMAKE_POSITION_INDEPENDENT_CODE=ON`.

## Sanitizers (Debug builds)

CMake options (Debug builds):

- `-DGO_ENABLE_ASAN=ON` (AddressSanitizer)
- `-DGO_ENABLE_UBSAN=ON` (UndefinedBehaviorSanitizer)
- `-DGO_ENABLE_TSAN=ON` (ThreadSanitizer)

Usage example:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Fuzzing

Optional toggles in `tests/CMakeLists.txt`:

- `-DENABLE_FUZZING=ON`
- `-DENABLE_FUZZING_RUNTIME=ON` (Clang libFuzzer runtime)

Targets currently included:

- `fuzz_event_deserialize` / `fuzz_event_driver` (compact JSON event)
- `fuzz_envelope_deserialize` / `fuzz_envelope_driver`

Recommended additions (pattern exists; add on demand):

- Frame decoder harness (WS frame boundaries)
- Filter parser harness
- Verify wrapper harness

Run example with corpus:

```bash
./build/tests/fuzz_event_driver tests/fuzz_seeds/events
```

## Dependency Versions (recorded via pkg-config where available)

- OpenSSL: detected via `find_package(OpenSSL REQUIRED)`
- libsecp256k1: detected via `pkg_check_modules(libsecp256k1)`
- libwebsockets: detected via `pkg_check_modules(libwebsockets)`
- GLib/GIO (optional): detected via `pkg_check_modules(glib-2.0,gobject-2.0,gio-2.0)`

Capture versions at configure time (see CMake configure output). For SBOM, use `scripts/sbom.sh`.

## Reproducible Build Notes

- Pin dependency versions in your package manager or submodules.
- Prefer system-provided libraries with known versions for CI consistency.
- Record CMake configure logs (store as artifacts) to trace exact versions and flags.

## Systemd Hardening

A hardened user unit is provided at:

- `apps/gnostr-signer/daemon/packaging/systemd/user/gnostr-signer-daemon.service`

It enables `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, `MemoryDenyWriteExecute`, `SystemCallFilter`, and more.

## TLS Posture (libwebsockets/OpenSSL)

- TLS 1.3 preferred, TLS 1.2 AEAD-only fallback
- Groups: X25519, P-256
- 0-RTT disabled, session tickets limited

See `docs/security.md` for details.
