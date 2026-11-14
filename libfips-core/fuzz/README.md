# FIPS fuzz harnesses

Enable `FIPS_ENABLE_FUZZERS=ON` in a top-level clang Debug build with
`GO_ENABLE_ASAN=ON` and `GO_ENABLE_UBSAN=ON`. Build target `fips_fuzz_host`.
Run `build/libfips-core/fuzz/fips_fuzz_host -max_total_time=10 corpus/` with a
writable copy of `libfips-core/fuzz/corpus/fips_fuzz_host/`. CI uses 10 seconds
per PR smoke target and 60 seconds nightly. Sanitizer findings fail the job;
crash/timeout/oom artifacts are retained. macOS disables unsupported LSan only.

`fips_fuzz_host` consumes seven little-endian uint64 fields (short input is
zero-padded): milliseconds, current instant, base seconds/nanoseconds, cap
seconds/nanoseconds, exponent. Nanoseconds are normalized. It exercises duration
conversion, capped/saturating backoff, deadline overflow/halving, and instant
comparison/difference/add/sub through the deterministic test host. Invariants
abort on violations even in optimized builds. This tests helpers, not a codec
or a cryptographic handshake that does not exist yet.

## Template for later public surfaces

For each new surface, add `fuzz_<surface>.c` with
`int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)`, and a target using
the same warnings, sanitizer and libFuzzer options as `fips_fuzz_host`. Instrument
the implementation with `fuzzer-no-link`. Check in minimal valid and malformed
seeds at `fuzz/corpus/<target>/`; add the target to both CI fuzz build/run lists.
Bound allocation and parsing work by input size. Treat invalid wire data as an
ordinary rejection; assert round-trip/canonicalization and state invariants for
accepted inputs. Use deterministic entropy/clocks, no network or sleeps. Never
turn a rejected packet into an assertion merely because it is malformed.

Pending harnesses, added when their APIs land:
- proto Reader/Writer;
- Noise msg1/msg2 and xk1-3;
- FMP/FSP frames;
- bloom/coord/lookup/MMP messages;
- control-socket JSON;
- native-API line protocol.
