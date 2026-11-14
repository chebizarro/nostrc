# FIPS fuzz harnesses

Enable `FIPS_ENABLE_FUZZERS=ON` in a top-level clang Debug build with
`GO_ENABLE_ASAN=ON` and `GO_ENABLE_UBSAN=ON`. Build targets `fips_fuzz_host` and `fips_fuzz_codec`.
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

`fips_fuzz_codec` treats input as commands selecting LE scalars, fixed arrays,
borrowed bytes or cursor advancement; high command bits select byte counts.
It checks exact truncation errors, cursor atomicity, writer round trips and
full-capacity rejection. Use a writable copy of `corpus/fips_fuzz_codec/` and
run the same 10-second command with target `fips_fuzz_codec`.

`fips_fuzz_link` treats input as a SessionDatagram body (the bytes after the
link msg_type). Short input must fail with exactly `MessageTooShort {35, len}`
and leave the cursor and output untouched; accepted input must expose the
borrowed payload at offset 35, obey the TTL hop rule, re-encode byte for byte
behind a `0x00` type byte, be rejected atomically by a one-byte-short writer,
and classify its first payload byte through both message-type catalogs. Use a
writable copy of `corpus/fips_fuzz_link/` with target `fips_fuzz_link`.

Pending harnesses, added when their APIs land:
- Noise msg1/msg2 and xk1-3;
- FMP/FSP frames;
- bloom/coord/lookup/MMP messages;
- control-socket JSON;
- native-API line protocol.

`fips_fuzz_aead` takes a zero-padded 32-byte key, LE u64 counter and one-byte
AD length, then that many AD bytes (clamped to available input), then arbitrary
ciphertext. It checks open rejection/zeroing and output-length atomicity,
seal/open round trips up to the transport limit, in-place tamper rejection,
short-capacity atomicity and key wiping. Seed `rfc-a5` authenticates through the
public API; other seeds cover empty, short and maximum payloads. Use a writable
copy of `corpus/fips_fuzz_aead/` with the same sanitizer command as above.
