# Pinned FIPS Rust oracle vectors

Test-only, standalone binary crate for `nostrc-s50dj.2`. It calls the real
`fips-core` protocol/identity/crypto implementations from
<https://github.com/jmcorgan/fips> at
`6aa80504baa0fa312b392009eea0e3e59fb6fe4c`, with **`test-hooks` and `ring`**.
It is not part of nostrc's shipped build and is not a C implementation or FFI.
All keys, signatures, epochs and entropy here are public **test fixtures**;
never use them in a deployment.

## Run and verify

From the repository root:

```sh
cd tools/fips-vectors
# This directory selects rust-toolchain.toml, including when using --manifest-path.
rustc --version                         # 1.94.1, matching upstream
cargo run --locked -- export            # default: libfips-core/tests/vectors/
cargo run --locked -- export --out /absolute/path/to/empty-output
# Or, after building:
./target/debug/fips-vectors export --out /absolute/path/to/empty-output
cargo fmt --check
cargo clippy --locked --all-targets -- -D warnings
cargo test --locked
```

Rustup may download 1.94.1, rustfmt and clippy on first use. The git dependency
resolved successfully on the development machine; **no path fallback** is in
use. Cargo.lock is checked in, locking the full dependency resolution as well
as the git revision. After an initial fetch, `cargo run --offline --locked --
export --out ...` works with that toolchain and dependency cache. Do not silently
substitute a newer compiler or upstream working tree when offline.

The `export` command accepts only the optional `--out <dir>`. It creates the
directory if needed and overwrites its generated names. Use **fresh directories**
for comparisons/pin bumps: it does not delete unrecognized files or stale names
from an older generator. Unset `FIPS_NODE_FAULTS` and `FIPS_REKEY_FAULTS`;
the exporter refuses these upstream mutation switches rather than capturing
fault-injected behavior as the oracle. No clocks, OS entropy, sockets or sleeps
are used to choose fixture values. Rekey `Poll` inputs are synthetic events at
`next_wake` deadlines, not a host polling loop.

## Format and coverage

Six JSON documents have `schema_version`, full `pin`, `generator` and
`generator_version`. Every raw-byte descriptor repeats pin/version and includes
`file`, lowercase `hex`, `length` and `sha256`. Its `.bin` sidecar is **exactly
the protocol/entropy bytes**, without a provenance header that would alter the
oracle. The containing JSON is its provenance sidecar. Integers are unsigned
JSON integers unless named otherwise; consumers must not round u64 values
through a JavaScript double. `seed_hex`/word strings are fixed-width hexadecimal
integers; raw bytes are in upstream wire order. Errors include the Rust variant
(and expected/got for codec truncation). Debug strings are diagnostic summaries,
not a promised Rust parser format; use typed inputs and binary/hex fields.

| Family | Captured cases |
| --- | --- |
| `entropy` | Three seeds: 0, `0123456789abcdef`, `ffffffffffffffff`. Each has 128 SplitMix64 words / 1 KiB SeededEntropy output, sequential fills of 1/7/8/9/17 bytes (42 bytes, **discarding** each partial word's tail), and `seeded_keypair` secret/compressed/x-only pubkeys. |
| `identity` | Three seeded identities: x-only key, npub, nsec, NodeAddr, IPv6 bytes/text. Each signs/verifies an auth challenge at timestamp 1700000000 with FixedEntropy (challenge byte a5, signing aux byte 5a), plus wrong-challenge failure. |
| `codec` | 15 cases: u8/u16/u32/u64 little-endian zero/max/mixed-bit values, array, bytes, empty bytes and cursor/require/rest/advance. 54 short-input errors cover every truncation length of each nonempty primitive case, including cumulative-offset require. |
| `noise` | Full IK (2 messages) and XK (3 messages), both roles' per-step progress/h and final h; seeded keypairs 0/1, one shared sequential entropy stream per pattern (IK seed 494b, XK seed 584b), fixed startup epoch `0123456789abcdef`. Eight frames **per direction per pattern** (32 total), alternating empty/nonempty AAD, opened at the peer, replay failures, corrupted frames at the correct unused counters followed by successful original frames (failed auth does not consume the counter). ReplayWindow has 16 accept/reject steps including stale/window-boundary counters. |
| `proto` | 25 cases: coordinate/absent coordinate, bloom bits/announce, signed parent declaration/tree announce, lookup request/response, link and session sender/receiver reports, MTU notification, path probe/ack/close (52/53/54), disconnect, real IK-framed FMP msg1/msg2, real XK FSP setup/ack/msg3, authenticated FSP/FMP frames and their encapsulated session datagram. Includes decoder offsets, empty-input errors, all 12 link catalog and 5 session catalog values, core framing constants and a payload-limit example. |
| `rekey` | One real two-peer FSP/XK rekey: established sessions from the Noise fixture, default Params::fsp, forced start, Setup/Ack/Msg3, sealed prompts, evidence-based cutover, data both ways, scheduled polls and old-key drain. 16 state-after-event records, initial/final states, actions, exact authenticated plaintext delivery assertions and raw emitted messages/frames. No loss or randomized scheduling. |

Most encoded proto cases are immediately decoded and re-encoded with equality
assertions. Encrypted frames are opened by the peer with their exact AAD.
Framing `payload_len` for established FMP/FSP messages is plaintext length,
**excluding** the AEAD tag. Decode offsets distinguish link-type bytes (1), FSP
handshake prefixes (4), and body-only records (0). `src` is seed 0's root with
sequence `0123456789abcdef`, timestamp 1700000000; `dst` is seed 1 -> seed 0,
zero metadata. Address-only encodings intentionally drop coordinate metadata.

The integration tests run the actual CLI in fresh processes, compare two full
exports and all checked-in fixtures, verify every binary descriptor/hash and
that no raw files are orphaned, and compare all three 1 KiB streams with the
unchanged Python-derived constants in `libfips-core/tests/seeded_vectors.h`.
They also check the chunk boundaries against word-aligned stream offsets and
reject invalid CLI/fault-switch invocations. The streams **agree byte for byte**.
The header's constants are retained for the existing C tests; the Rust fixtures
are now the authoritative regeneration source (C consumer work is s50dj.3).

## Private adapters, test hooks and gaps

- No `ratelimit` or stats-history differential family yet (`nostrc-u50yr.4`).
  The public upstream APIs are available, but a deterministic trace schema and
  exporter/replay family remain unimplemented. The C port uses all 20 upstream
  node limiter tests and 22 stats-history tests plus capacity/eviction edges;
  these are unit-test ports, not differential oracle evidence.

- Upstream `core/src/testutil.rs` is **private `cfg(test)`**, not exported by
  `test-hooks`. `src/support.rs` transcribes its SplitMix64, SeededEntropy,
  FixedEntropy, seeded_keypair and millisecond TestClock adapters at the pin.
  Protocol computation and key derivation use the git dependency; we do not
  pretend to call upstream's private fake functions. Review this small adapter
  against testutil.rs on every pin bump. Shared thread-local entropy is consumed
  in the recorded initiator/responder call order, not independently per role.
- Test hooks actually used: `HandshakeState::handshake_hash` and
  `Rekey::start_now`. `start_now` forces only the initial trigger; the successful
  exchange, cutover, send reservations, receive authentication and draining run
  on the normal machine surface. This is **not** a production trigger-policy
  test. `Fork` methods, ReplayWindow, NoiseSession, Params and the wire codecs
  are normal public APIs. All mutation switches stay off.
- The Noise chaining key `SymmetricState.ck` is private with no getter at this
  pin. JSON records `ck: null` and the gap rather than inventing a value. The
  handshake hash **is** reachable and captured for both roles.
- Legacy 0.5.x rekey models under `core/src/proto/rekey/tests/legacy` (0.5.0/0.5.1,
  0.5.2, 0.5.3) are private test/slow-tests modules, not reachable through the
  dependency's public/test-hook API. **`nostrc-f12v2.3` must capture/run their
  compatibility traces** via an upstream export hook or separate test harness.
  This exporter does not claim legacy interop. Its one successful FSP trace
  also does not cover FMP rekey, loss/retry, restart or all rejection paths.
- This is representative protocol wire coverage, not an exhaustive malformed
  corpus or the routing/probe/path/settlement state-machine differential suite.
  New cases can be added as those C port tasks land. The host IPv6 module's
  `FIPS_IPV6_OVERHEAD` is outside `fips-core`; core `FIPS_OVERHEAD` is 106 bytes
  (110 with the 4-byte port header). Do not reinterpret the host's 77-byte IPv6
  compression accounting as the core constant.
- No C differential test, sanitizer lane, hosted CI or two-node host interop is
  established by running this tool. Those gates remain separately owned.

## Regeneration / pin bump

1. Follow `libfips-core/UPSTREAM.md`'s watch-set and compatibility procedure.
   Preserve an export at the old pin before changing anything.
2. Change Cargo.toml's **full git `rev`**, `src/main.rs`'s PIN, the test's expected
   pin, the toolchain if upstream changed it, and this README's provenance.
   Review support.rs against pinned testutil.rs. Update generator version for
   changed generator behavior/schema and regenerate Cargo.lock. Never use a
   moving branch or a committed local `path` dependency.
3. Export to two fresh directories, e.g. under this crate's ignored `target/`:
   ```sh
   cargo run --locked -- export --out "$PWD/target/export-a"
   cargo run --locked -- export --out "$PWD/target/export-b"
   diff -ru "$PWD/target/export-a" "$PWD/target/export-b"
   ```
   A nonzero diff is a failure. Compare old/new families, review wire/error and
   gap changes, then replace only the owned files in tests/vectors with the
   reviewed fresh export. Do not leave old `.bin` files behind. Never overwrite
   older compatibility evidence without comparison.
4. Run fmt, clippy and tests above. The fixture test intentionally fails until
   checked-in fixtures match. Reassess C header agreement rather than changing
   its constants to hide drift; coordinate the separate C consumer update.
5. Check JSON, raw files, source, Cargo.lock and provenance docs into one commit.
   Record the component version decision (test-only/no shipped bump), then run
   separately owned C differential and host interop gates before advancing the
   compatibility baseline. Remove this crate's `target/` after verification.
