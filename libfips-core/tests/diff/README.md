# Pinned Rust → C differential runner

`fips_diff` consumes the schema-v1 contract in `tools/fips-vectors/src/*.rs`.
It is test-only: Jansson, file I/O and predictable entropy never enter
`fips_core`. No Rust toolchain, relay, clock delay or exporter invocation is
needed to replay the checked-in fixtures.

## Build and run

From a configured top-level build with `BUILD_FIPS=ON` and `BUILD_TESTING=ON`:

```sh
cmake --build /absolute/build --target fips_diff
ctest --test-dir /absolute/build -R '^FipsDiff' -V
/absolute/build/libfips-core/tests/diff/fips_diff entropy
# Override the compile-time source vectors path, e.g. for an independent export:
/absolute/build/libfips-core/tests/diff/fips_diff entropy /absolute/vectors
```

Use `-V` to see every PASS/FAIL/SKIP and the checked/skipped/failed summary,
including successful all-SKIP families. Regular CTest hides passing test stdout.
The tests are `FipsDiffEntropy`, `FipsDiffIdentity`, `FipsDiffCodec`,
`FipsDiffNoise`, `FipsDiffProto`, `FipsDiffRekey`, and `FipsDiffSelfTest`.
They have `fips;diff` labels and selection metadata. Existing FIPS CI explicit
build targets depend on the runner, including the host-only TSan build; no
workflow edits are needed. This does not qualify a sans-IO core for threading.

For warnings-as-errors and combined ASan/UBSan, configure Debug with
`-DGO_WARNINGS_AS_ERRORS=ON -DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON`, then build
FIPS targets and run `ctest --test-dir /absolute/build -R '^Fips' -V`.
The runner uses the shared `apply_warnings`/`apply_sanitizers` helpers.
On macOS, use `ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:halt_on_error=1`
(the platform does not support LeakSanitizer) and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`, as in existing FIPS CI.
Fresh worktrees need the repository's nostrdb submodule initialized before
configuring even with storage disabled. Keep build and self-test scratch files
out of the source fixture directory.

## Loader and integrity

`loader.c` checks family, generator, schema version, generator version,
provenance, and every raw-byte descriptor recursively **before dispatch**.
Sidecars are restricted to `.bin` basenames relative to the supplied vector
directory. Every descriptor must match its sidecar's length and SHA-256; its
inline hex must also match exactly. Inline-only hex fields are decoded too.
Failures print the record path, and byte differences print the first differing
offset and expected/actual hex rows covering up to 16 bytes on either side.
No invalid fixture is counted as skipped or checked. Files are capped at 16 MiB
and decoded nesting at 64 levels; missing, duplicate-key or malformed records
fail rather than silently disappearing from coverage. Exporter gaps are echoed.

The full pin is read from `UPSTREAM.md`'s `Pinned commit:` line by CMake, which
reconfigures when that file changes. Every document and byte descriptor must
match it. A pin mismatch exits 1 with expected/actual pins: there is no override,
auto-update, warning-only path, or fallback to another fixture set. Generator
versions must be nonempty and agree within a document, not match a second
hardcoded constant. Unknown schema versions fail; schema changes need review.

Jansson has signed 64-bit JSON integers, but the exporter includes `u64::MAX`.
The loader lexes integer tokens outside strings and quotes only values above
`INT64_MAX` with a private marker before Jansson parses them. It then converts
them with checked integer arithmetic to `uint64_t`, never a floating-point
intermediate. Literal source strings using that marker are rejected, including
escaped spellings. Small and negative integers retain their native types.

## Adding real family checks

- `schema.c` transcribes the exporter shapes: basic array cases; codec
  `kind`/optional scalar `value`/truncation errors plus the separate cursor
  record; Noise patterns/messages/steps/frames and replay steps; proto common
  inputs/wire variants/catalogs/constants; rekey profile/initial states/events/
  outputs/authenticated deliveries/final states. Keep this aligned with reviewed
  changes in the Rust exporter, not Rust Debug string parsing.
- `diff.h` defines the owned, tagged C record tree. It preserves all fields,
  nullable states, signed/unsigned integers and arrays; schema decoding turns
  byte descriptors/hex fields into `diff_bytes` and hex words into `uint64_t`.
  Jansson objects do not escape the loader. Family-specific borrowed views are
  built in `cases.c`; their nested records use that C tree rather than `json_t`.
- Replace `check_identity`, `check_codec`, `check_noise`, `check_replay`,
  `check_proto`, or `check_rekey` in `cases.c`. Drive the actual C entrypoints
  with the decoded inputs and compare bytes with `diff_compare`, plus typed
  errors, offsets, actions and state transitions. Return `CHECKED` only after
  all checks in that case succeed, `FAILED` on any mismatch. Extend borrowed
  views as APIs land; no CMake registration changes are required.
- `check_entropy` already replays stream bytes, all SplitMix64 words, sequential
  chunk fills (including discarded word tails and zero-length fills), combined
  chunk bytes, and seeded secret/compressed/x-only keys with libsecp256k1.
  Keypair derivation follows `support.rs`: fresh seed XOR
  `0x6b65797061697273`, four LE words, then direct secp secret interpretation.

## SKIP policy and counting

A loaded fixture is **not** a protocol pass. Unimplemented dispatchers return
`SKIPPED` and print `SKIP <case>: not implemented (owner <beads id>)`:

| Family | Checked today | Skipped today | Owner |
| --- | ---: | ---: | --- |
| entropy | 3 | 0 | implemented test adapters |
| identity | 0 | 3 | nostrc-u50yr |
| codec | 0 | 15 | nostrc-u50yr |
| noise | 0 | 18 | nostrc-pwo3h |
| proto | 0 | 28 | nostrc-82g9n |
| rekey | 0 | 1 | nostrc-f12v2 |

Counting units: entropy/identity are per seed; codec includes all truncations
within its parent case; Noise has two whole transcripts (including their frames)
and 16 replay steps; proto has 25 wire cases, two catalogs and one constants
case (common inputs are context); rekey has one whole FSP cycle with 16 events,
initial/final states and authenticated deliveries. Implementing a whole-case
check means checking its nested assertions too, not just one output byte.
All-SKIP families exit 0 with `0 checked, N skipped, 0 failed`, so future ports
can replace skips without CMake churn. Any failure exits 1. An empty family is
not coverage and fails. These results are not an interop or Tier A parity claim.

`FipsDiffSelfTest` makes a private entropy-fixture copy in the build directory,
proves that copy passes, then independently flips one sidecar byte and one JSON
hex digit. It requires exit **1** (not a crash) with the SHA-256 mismatch and
case/offset/hex-dump diagnostics respectively. It additionally rejects wrong
pins and sidecar lengths, and removes its temporary copy. No checked-in vector
is modified. The internal `--mutate` entrypoint is only for this private copy.

Legacy rekey models, broader malformed corpora, future state-machine fixtures,
C/Rust UDP interop, Linux/hosted validation and protocol implementations remain
separately owned; see exporter gaps and `UPSTREAM.md`.
