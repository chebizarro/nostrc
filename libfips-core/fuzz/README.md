# FIPS fuzz harnesses

Enable `FIPS_ENABLE_FUZZERS=ON` in a top-level clang Debug build with
`GO_ENABLE_ASAN=ON` and `GO_ENABLE_UBSAN=ON`. Build the `fips_fuzz_*` targets listed below.
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
or the separate cryptographic handshake target.

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

Future surfaces (not present wire codecs): MMP messages, control-socket JSON,
and the native-API line protocol.

`fips_fuzz_aead` takes a zero-padded 32-byte key, LE u64 counter and one-byte
AD length, then that many AD bytes (clamped to available input), then arbitrary
ciphertext. It checks open rejection/zeroing and output-length atomicity,
seal/open round trips up to the transport limit, in-place tamper rejection,
short-capacity atomicity and key wiping. Seed `rfc-a5` authenticates through the
public API; other seeds cover empty, short and maximum payloads. Use a writable
copy of `corpus/fips_fuzz_aead/` with the same sanitizer command as above.


## Tier A3 proto wire coverage

All names in the harness column have the prefix `fips_fuzz_`; corpus paths are
relative to `fuzz/corpus/fips_fuzz_<harness>/`. No new fuzz target is needed.
`sync_proto_corpus.py` copies the **actual `.bin` sidecars** referenced by
`tests/vectors/proto.json`, checking their length/hex first. Run it with `--check`
to detect stale/missing seeds; without that flag it regenerates them. Full wire
seeds retain their original bytes. `.input` seeds strip dispatch prefixes (and,
for STP, prepend the harness selector), so those exact bodies reach the decoder.
The raw bloom hash count is out-of-band: a leading zero is not a zero hash count.

| Codec / public wire pair | Harness | Corpus seed(s) |
| --- | --- | --- |
| Reader/Writer LE u8/u16/u32/u64, array/bytes/advance | codec | `scalars`, `bytes`, `truncated`, `zero-length` (command grammar) |
| CoordEntry metadata | coord | `proto-coord-entry.bin` (32-byte entry from TreeAnnounce sidecar) |
| Required / optional / empty address coordinates | coord | `proto-coords.bin`, `proto-coords-empty.bin` |
| LinkMessageType / SessionMessageType from_u8 | link | `link_catalog-*`, `session_catalog-*` (exact JSON values at payload offset 35) |
| SessionDatagram | link | `proto-session-datagram.bin{,.input}` |
| ParentDeclaration signing preimage / decode | stp | `proto-parent-declaration-signing.bin{,.input}` (selector 0) |
| TreeAnnounce (metadata ancestry) | stp | `proto-tree-announce.bin{,.input}` (selector 1) |
| BloomFilter raw bytes | bloom | `proto-bloom.bin` |
| FilterAnnounce | bloom | `proto-filter-announce.bin{,.input}` |
| LookupRequest | lookup | `proto-lookup-request.bin{,.input}` |
| LookupResponse / proof preimage | lookup | `proto-lookup-response.bin{,.input}` |
| RoutingSignalType from/to byte | routing | `signal-type-*` plus mutations |
| CoordsRequired | routing | `coords-required`, `reserved-flags`, `truncated` |
| PathBroken (None / Some coordinates) | routing | `path-broken`, `path-broken-coords` |
| MtuExceeded | routing | `mtu-exceeded` |
| FMP Disconnect / reason from_byte | fmp | `proto-disconnect.bin{,.input}` |
| FMP handshake type / common prefix / msg1 parse+build | fmp | `proto-fmp-msg-1.bin` |
| FMP msg2 parse+build | fmp | `proto-fmp-msg-2.bin` |
| FMP established header / encrypted assembly | fmp | `proto-fmp-encrypted.bin`, `empty-tag`, `short-pending` |
| FMP inner header strip/prepend | fmp | all FMP seeds are also treated as plaintext |
| FSP common prefix / encrypted header / assembly | fsp | `proto-fsp-encrypted.bin`, `fsp-encrypted-body` |
| FSP SessionSetup lenient+strict | fsp | `proto-fsp-setup.bin{,.input}` |
| FSP SessionAck lenient+strict | fsp | `proto-fsp-ack.bin{,.input}` |
| FSP SessionMsg3 lenient+strict | fsp | `proto-fsp-msg3.bin{,.input}` |
| FSP encrypted optional coordinate views | fsp | `encrypted-coords-some`, `encrypted-coords-none` |
| FSP port header | fsp | `ports-exact`, `ports-ipv6` |
| FSP inner header strip/prepend; session/outer/inner flag byte pairs | fsp | all FSP seeds are also treated as plaintext/flag bytes |

Routing signals, port headers and inner headers have **no dedicated differential
row at this pin**. Their independently authored seeds are not Rust oracle vectors.
Framing constants and payload-limit math have no wire codec; unit/differential
tests cover them. The eight unimplemented report/MTU/probe rows stay Tier A4 skips,
not claimed fuzz/property coverage. Other existing targets are `host`, `identity`,
`aead`, and `handshake` (their existing corpora are unchanged).

`FipsCoreProtoPropertiesTest` separately checks 64 SeededEntropy structures per
adapter, meaningful-field equality, every applicable sidecar byte, every
truncation offset, one-byte XOR corruption at every body offset, and full versus
one-short Writer capacity (including a nonzero starting cursor). Prefix-only
Option parsers and Reader/Writer primitives get another 64 structured rounds.
Truncation is not always an error: raw bloom and opaque datagram/inner tails are
valid shorter payloads; header-only Option parsers return None below their minimum;
an FMP established envelope shortened after its minimum reports Malformed
`payload_len`. Unknown disconnect reasons canonicalize to Other. These are the
public codec contracts, not suppressed property failures. ASan input allocations
end exactly at each truncation boundary.
