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
# Run from this directory to select rust-toolchain.toml. From elsewhere, use
# cargo +1.94.1 <command> --manifest-path ...; --manifest-path alone does NOT select it.
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

The seven ordinary JSON documents and twelve `rekey-legacy/*.json` traces have `schema_version`, full `pin`, `generator` and
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
| `rekey-legacy` | Twelve real current-node versus upstream legacy-model rotations: FMP/IK and FSP/XK, each initiated by the current node and by the legacy peer, for V050 (shared 0.5.0/0.5.1 model), V052 and V053. Four traces per model, 792 total current-node events, authenticated data both ways, cutover and old-key drain. Captured separately with the test-only upstream patch below. |
| `nostr` | Four verbatim deployed v0.44 captures from the root crate: advert 37195, inbox 10050, offer/answer 21059; real fips-mesh payload parsing, signature verification and NIP-44 unwrapping. |
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

- Routing error signals (CoordsRequired/PathBroken/MtuExceeded) have no `proto.rs` wire vectors at the pin; routing uses upstream unit-test ports, not differential parity (nostrc-82g9n.4).

- No utils/math differential vector family yet (nostrc-u50yr.3); upstream unit-test ports, SipHash reference vectors and local Rust powi pins are not harness-family parity.

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
- Legacy 0.5.x models remain private test/slow-tests modules, so the ordinary
  exporter cannot call them. `scripts/capture-legacy-rekey.sh` now captures the
  actual models through a pinned, test-only upstream patch (not a transcription
  of their behaviour). The ordinary successful FSP trace and the legacy matrix
  do not cover the full loss/retry/restart/first-handshake/explorer suites.
- This is representative protocol wire coverage, not an exhaustive malformed
  corpus or the routing/probe/path/settlement state-machine differential suite.
  New cases can be added as those C port tasks land. The host IPv6 module's
  `FIPS_IPV6_OVERHEAD` is outside `fips-core`; core `FIPS_OVERHEAD` is 106 bytes
  (110 with the 4-byte port header). Do not reinterpret the host's 77-byte IPv6
  compression accounting as the core constant.
- No C differential test, sanitizer lane, hosted CI or two-node host interop is
  established by running this tool. Those gates remain separately owned.

- Diagnostic `proto/probe` has no differential family or wire codec at this pin; unit tests are its specification. `path-probe`/`path-ack`/`path-close` (0x52–0x54) are `proto/path/wire.rs` cases, owned by nostrc-f12v2.6, not probe.

## Legacy capture (nostrc-f12v2.3)

```sh
# From anywhere; output must be an absolute directory under this nostrc worktree.
/absolute/nostrc/tools/fips-vectors/scripts/capture-legacy-rekey.sh \
  /absolute/read-only/fips /absolute/nostrc/libfips-core/tests/vectors/rekey-legacy
```

The script reads the full Cargo dependency pin and Rust toolchain (1.94.1),
checks agreement with `UPSTREAM.md` for the default pin, makes a disposable `git clone --shared`
checkout at exactly that pin under `build-legacy-capture/`, and applies
`upstream-patches/<full-rev>-legacy-rekey-export.patch`. It never changes the
source checkout, registers worktrees there, or checks out its branch. The patch
is zero-context (applied with `--unidiff-zero` at the checked exact pin) to
avoid trailing-space context lines in the stored patch. It adds one ignored
`slow-tests` unit test inside `tests/legacy`; the only change
to a model is making FMP's `initiate` visible to that sibling test. No shipped
API or model behaviour changes. The bridge calls the real `FmpPeer`/`FspPeer`
and production `FmpIk`/`FspXk`, not copies of their rules. Existing upstream
MIT licensing is retained in `LICENSE.upstream`.

It runs the exact ignored test twice in fresh processes with
`cargo test -p fips-core --features slow-tests,test-hooks,ring`, a worktree-local
`CARGO_TARGET_DIR`, and an explicit `FIPS_LEGACY_EXPORT_DIR`. It requires all
12 documents and validates every sidecar's hash/hex/length/pin/version, rejects
orphan files, and requires both runs to be byte-identical before publishing.
The trap removes the copied checkout, both captures and Cargo target even on
failure. The empty `build-legacy-capture/` parent may be removed afterwards.
Fault-injection environment variables are refused, not silently unset.

Fixtures use seeded identities 0/1, the ordinary initial Noise handshake seeds
(494b/584b), reserved initial FMP indices 10/20, default parameters and zero
jitter. Both peers authenticate old-key traffic at 59 s, then a trigger is
forced at 60 s (`start_now`, the model's IK initiation, or the model's XK message
trigger). **Trigger-policy coverage is not claimed.** Deliveries advance a
synthetic clock by 10 ms; the legacy shell ticks each second, the current node
also polls at its next wake (or immediately for a wake made due during channel
delivery). Data goes both ways each second through 80 s; capture asserts current
and legacy authentication, convergence, one rotation, drain completion and no
leaked leases. No wall-clock waits or networks select the trace.

Each document keeps the `rekey.json` events/outputs/initial/final-state schema,
with `legacy_release`, `legacy_initiates` and an `entropy_seed_hex` on every
current-node event. Entropy is reseeded at the event boundary so C can replay
just that node without needing to emulate the peer's entropy consumption.
FMP uses the real index allocator. All Send/Prompt/ApplicationFrame bytes have
provenance-stamped `.bin` sidecars; authenticated Deliver plaintext/ranges and
ordered Bind/Unbind/Release effects are also compared. Initial/final state arrays
contain just the current node: the legacy model is the oracle peer, **not** a
second instance of the current machine. C replay needs no Rust runtime.

## Regeneration / pin bump

Start with `scripts/fips-upstream-bump.sh` from the repository root (see
`libfips-core/UPSTREAM.md` for the full clone/rehearsal/build commands). It copies
this crate into worktree-local scratch, changes only that copy's Cargo rev,
lockfile source and `src/main.rs` PIN, resolves candidate dependencies when the
pin changes, exports twice, captures legacy twice, and compares all eight
families without overwriting the baseline. `--keep` retains the reported vector
root for the separate C sweep; otherwise scratch is removed even on failure.
The same pin needs no source/provenance/patch rename. A future candidate needs a
reviewed candidate-stamped legacy patch (`--legacy-patch /absolute/patch`).
Direct capture also accepts optional third/fourth arguments: full candidate
commit and absolute reviewed patch, without changing the committed pin.

Prerequisites: Git with HTTPS access, Python **3.11+** (`tomllib`), Rustup, Cargo,
and Rust **1.94.1** with rustfmt/clippy (`rustup toolchain install 1.94.1 --component
rustfmt --component clippy`). Cargo is not installed by CMake; do not confuse a
successful C build with exporter availability. Candidate toolchain/MSRV or
private `testutil` changes require review before running. All downloaded Cargo
sources and targets from the wrapper are scratch-local. The wrapper reports
watch/fixture changes with exit 1, operational failures with exit 2; exit 0 is
byte-identical export evidence, **not** C differential or host interop proof.
A new pin changes provenance even when wire bytes do not: review the saved
`fixtures.diff`, never dismiss binary/state differences as a pin-only update.


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
4. Review/rebase the legacy export patch onto the candidate pin, retaining the
   actual legacy models and the MIT notice. Rename the patch to the full new
   revision and update its PIN/generator version if needed. Run the capture
   script to a fresh worktree-local directory; its two-run comparison must pass.
   Compare all old/new legacy JSON and sidecars before replacing fixtures, and
   update the C loader's required matrix if upstream's release structure changes.
   Never silently drop a legacy release/profile/role on a pin bump.
5. Run fmt, clippy and tests above. The fixture test intentionally fails until
   checked-in fixtures match. Reassess C header agreement rather than changing
   its constants to hide drift; coordinate the separate C consumer update.
6. Check JSON, raw files, source, Cargo.lock and provenance docs into one commit.
   Record the component version decision (test-only/no shipped bump), then run
   separately owned C differential and host interop gates before advancing the
   compatibility baseline. Remove this crate's `target/` after verification.

## Nostr rendezvous family (`nostrc-8hvs6.1`)

`nostr.json` adds the root crate's four deployed v0.44 signed captures to the
ordinary deterministic export and hash/sidecar tests. `fips-mesh` is a git
dependency at the same full pin (compile-only, no service startup); its public
payload types parse the advert, offer and answer. Direct `nostr = 0.44.8`
verifies all four signatures and decrypts the seal/rumor layers. The private
`cfg(test)` fixture helpers are not callable from a dependency: captures under
`fixtures/nostr/v044` are **verbatim transcription**, not freshly generated
randomized events. See `fixtures/nostr/README.md` for file/line provenance and
licensing. Exact captured bytes and decrypted canonical payloads have binary
sidecars; URL normalization/equality cases use real `RelayUrl`. This is the
seventh ordinary JSON family; existing six families are unchanged. No version
bump: test-only tooling, still 0.1.0. The root dependency is intentionally
heavy; Cargo never runs a daemon or opens relay connections for this export.
