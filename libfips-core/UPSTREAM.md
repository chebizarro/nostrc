# FIPS upstream compatibility baseline

Pinned commit: `6aa80504baa0fa312b392009eea0e3e59fb6fe4c`

- **Upstream:** <https://github.com/jmcorgan/fips>, Rust `master` as inspected
  on **2026-11-14**; commit date **2026-11-14 03:01:39 UTC**.
- **Upstream package version:** `0.6.0-dev` (`Cargo.toml`); this is a development
  snapshot, not a `v0.6.0` release tag. Historical tags `v0.3.0` through `v0.5.3`
  remain compatibility references, not the source pin.
- **Selection:** the port assessment measured this exact tree, after the
  `fips-core` sans-IO split and the IK/XK/shared-rekey work. Older estimates used
  the June 2026 tree and obsolete module names. Pin the immutable commit, not a
  moving branch or the latest release, so C/Rust differential results have one
  reproducible oracle. No upstream source is vendored by this document.
- **Owning task:** `nostrc-s50dj.5` (foundations `nostrc-s50dj`).
- **Assessment:** FIPS Rust-to-C port assessment, supplied separately from
  `docs/investigations/fips-rust-to-c-port-on-nostrc-2026-11-13.md`, especially
  sections 1, 2.2, 2.3, 5.5 and 6. It is dated November 13 but explicitly
  measures the November 14 upstream commit above.

The C core stays runtime/OS-independent: the host supplies entropy and time,
feeds inputs into state machines, and executes their returned actions. Root
Rust host modules belong in `fipsd`, not in `libfips-core`.

## Wire and host-interface invariants

Treat the pinned source and its `docs/reference/` corpus as the oracle. In
particular, reproduce these bytes and semantics, not merely equivalent APIs:

- **Noise patterns:** `Noise_IK_secp256k1_ChaChaPoly_SHA256` for FMP link setup;
  `Noise_XK_secp256k1_ChaChaPoly_SHA256` for FSP end-to-end sessions.
- **DH:** SHA-256 of the **32-byte x-coordinate** of the secp256k1 ECDH shared
  point; discard y/parity. Do not use libsecp256k1's default ECDH hash. A custom
  x-copy callback followed by SHA-256 matches `core/src/noise/handshake.rs`.
- **IK msg1:** 33-byte compressed ephemeral public key, then 49-byte encrypted
  static key (33 + 16-byte tag), then 24-byte encrypted startup epoch
  (8 + 16-byte tag): **106 bytes total**, excluding outer framing. Constants
  in `core/src/noise/mod.rs` and the FMP codec must agree.
- **Replay/counters/rekey:** preserve replay-window behavior, counters, K-bit
  epochs and session indices. Use `core/src/proto/rekey/README.md`,
  `docs/reference/wire-formats.md` and the deployed-0.5.x models under
  `core/src/proto/rekey/tests/legacy` as compatibility evidence. Zeroize secrets
  and preserve authenticated-failure behavior as well as successful output.
- **Nostr kinds:** **37195** is the parameterized-replaceable overlay advert
  (`d=fips-overlay-v1`, NIP-40 expiration, JSON `OverlayAdvert` with
  `endpoints`, `signalRelays`, `stunServers`); **21059** is ephemeral traversal
  signalling (`TraversalOffer`/`TraversalAnswer`, NIP-44 encryption inside a
  NIP-59 gift wrap); **10050** is the NIP-17 inbox relay list. The same
  secp256k1 key is the FIPS identity and the Nostr identity. Follow
  `docs/reference/nostr-events.md`; do not substitute generic gift-wrap kinds
  for FIPS's signalling kind.
- **Addressing:** `fd || SHA-256(xonly_pubkey)[0..15]` gives the 16-byte IPv6
  address in `fd00::/8`; `.fips` names resolve through the daemon's DNS.
  **IPv6 overhead is 77 bytes** (`FIPS_IPV6_OVERHEAD`): 110-byte base
  DataPacket overhead minus 33 bytes saved by compressing the IPv6 header
  from 40 to 7 bytes. This is the pinned constant's accounting, not every
  packet's size; explicit coordinates and outer transport headers add bytes.
- **Control socket:** line-delimited JSON on a Unix socket. Default path order
  from `docs/reference/control-socket.md` is:
  1. `/run/fips/control.sock` if `/run/fips` exists;
  2. on macOS/FreeBSD, `/var/run/fips/control.sock` if its private directory
     exists (a privileged macOS daemon selects it even before that directory
     exists and creates it at bind time);
  3. `$XDG_RUNTIME_DIR/fips/control.sock` when available;
  4. `/tmp/fips-control.sock` as fallback.
  Explicit `node.control.socket_path` and client `-s PATH` override defaults.
  Windows uses loopback TCP `127.0.0.1:21210` instead. Do not confuse this with
  the separate native datagram API (`docs/reference/native-api.md`), whose
  Unix `SOCK_SEQPACKET` setup passes a datagram FD with `SCM_RIGHTS`.

## Module ownership map at the pin

Paths below are **planned C homes**, not claims of an implemented port. A
Rust directory row covers every nested module, including inline tests; test
behavior becomes differential fixtures/simulations rather than a literal Rust
source translation. Explicit test-only rows have no shipped runtime role.

Counts reproduce assessment section 2.2: physical Rust lines after its path-based
exclusions, not statement counts (inline test blocks can remain). Shared totals
are printed as **shares**, not invented per-module measurements; do not add
those shares together. Modules omitted from the assessment's table are marked
**not itemized**, rather than silently omitted from this inventory.

### Sans-IO core (`core/src/*` → `libfips-core/src/`)

| Rust module(s) at pin | Assessment non-test LoC | Planned C home(s) | Owning bead epic/task |
|---|---:|---|---|
| `core/src/proto/path/**` | 6,783 | `libfips-core/src/proto/path/{wire,set,params,rules,candidate,capable,select,live,machine}.c`, matching public headers | `nostrc-f12v2.6`: bounded PathSet, codecs and runtime-gated Bundle; daemon integration and simulator vector export remain separate |
| `core/src/proto/rekey/**` | 6,575 | `libfips-core/src/proto/rekey/…` + `include/fips/proto/rekey/…` (shared machine/params/profile hooks implemented; [API and ownership](src/proto/rekey/README.md), [test mapping](tests/REKEY.md); twelve legacy-model compatibility traces captured/replayed) | `nostrc-f12v2.1` shared core; `nostrc-f12v2.3` legacy; `nostrc-f66sr` verification |
| `core/src/proto/settle/**` | 5,493 | `libfips-core/src/proto/settle/…` (port last; upstream not wired to callers yet) | `nostrc-f12v2` A4 |
| `core/src/proto/mmp/**` | 4,160 | `libfips-core/src/proto/mmp/…` | `nostrc-f12v2` A4 |
| `core/src/proto/fmp/**` | 2,186 | `libfips-core/src/proto/fmp/…` | `nostrc-82g9n` A3; real IK/XK profiles and bounded session binding implemented: `nostrc-f12v2.2` A4 |
| `core/src/proto/lookup/**` | 1,752 | `libfips-core/src/proto/lookup/…` | `nostrc-82g9n` A3 |
| `core/src/proto/stp/**` | 1,574 | `libfips-core/src/proto/stp/…` | `nostrc-82g9n` A3 |
| `core/src/proto/probe/**` | 1,220 | `libfips-core/src/proto/probe/{core,state}.c`, `include/fips/proto/probe/{mod,core,state,limits}.h` | `nostrc-f12v2.5`: 30 upstream tests ported; no wire codec/differential family; host owns job registry |
| `core/src/proto/fsp/**` | 1,461 | `libfips-core/src/proto/fsp/…` | `nostrc-82g9n` A3; real IK/XK profiles and bounded session binding implemented: `nostrc-f12v2.2` A4 |
| `core/src/proto/probe/**` | 1,220 | `libfips-core/src/proto/probe/…` | `nostrc-f12v2` A4 |
| `core/src/proto/bloom/**` | 1,162 | `libfips-core/src/proto/bloom/…` | `nostrc-82g9n` A3 |
| `core/src/proto/routing/**` | 969 | `libfips-core/src/proto/routing/…` | `nostrc-82g9n` A3 |
| `core/src/proto/{codec,error,math}.rs` | share of 1,105 (`proto/*.rs`) | `libfips-core/src/proto/{codec,error,math}.c` | `nostrc-u50yr` A1 primitives |
| `core/src/proto/{coord,link,framing,rate_limit}.rs` | share of 1,105 | `libfips-core/src/proto/{coord,link,framing,rate_limit}.c` | `nostrc-82g9n` A3 |
| `core/src/proto/mod.rs` | share of 1,105 | `libfips-core/src/proto/…` (module/API glue) | `nostrc-s50dj` foundations |
| `core/src/noise/**` | 2,322 | `libfips-core/src/noise/…` | `nostrc-pwo3h` A2 Noise |
| `core/src/identity/**` | share of ≈4,600 | `libfips-core/src/identity/…` | `nostrc-u50yr` A1 |
| `core/src/entropy.rs` | share of ≈4,600 | `libfips-core/src/entropy/…` | `nostrc-u50yr` A1 |
| `core/src/time/**` | share of ≈4,600 | `libfips-core/src/time/…` | `nostrc-u50yr` A1 |
| `core/src/utils/**` | share of ≈4,600 | `libfips-core/src/utils/…` | `nostrc-u50yr` A1 |
| `core/src/cache/**` | share of ≈4,600 | `libfips-core/src/cache/…` | `nostrc-82g9n` A3 |
| `core/src/node/**` | share of ≈4,600 | `libfips-core/src/node/…` (rate limits/stats, not host I/O) | `nostrc-82g9n` A3 |
| `core/src/transport/**` | share of ≈4,600 | `libfips-core/src/transport/…` (types/on-link prefixes only) | `nostrc-u50yr` A1; host use: `nostrc-gfi8h` B2 |
| `core/src/lib.rs` | not itemized | `libfips-core/src/core/…` (library assembly) | `nostrc-s50dj` foundations |
| `core/src/{faults.rs,testutil.rs,tests/**,proto/tests/**}` | excluded test-only support | `libfips-core/src/test_support/…` (test builds only) | `nostrc-f66sr` verification |

Assessment totals: `proto` ≈34,400; whole core ≈41,400 non-test / 128,298 total.
Directory-level test exclusions also apply inside all rows above.

### Host (`src/*` → `fipsd/src/`)

| Rust module(s) at pin | Assessment non-test LoC | Planned C home(s) | Owning bead epic/task |
|---|---:|---|---|
| `src/node/**` | 50,186 | `fipsd/src/node/…` (lifecycle, handlers, dataplane, workers, snapshots) | `nostrc-9keoz` B1 host |
| `src/transport/**` | 45,991 | `fipsd/src/transport/…` (UDP/TCP, Tor/SOCKS5, Nym, Ethernet, BLE and shared adapters) | `nostrc-gfi8h` B2 transports |
| `src/bin/**` | 13,616 | `fipsd/src/bin/…` (`fips`, `fipsctl`, gateway CLI, `fipstop`) | daemon: `nostrc-9keoz` B1; tools: `nostrc-iq1n9` B5; gateway: `nostrc-9nxwk` B4 |
| `src/ipv6tun/**` | 12,014 | `fipsd/src/ipv6tun/…` | `nostrc-9nxwk` B4 upper layers |
| `src/gateway/**` | 11,129 | `fipsd/src/gateway/…` (optional separate parity work) | `nostrc-9nxwk` B4 |
| `src/config/**` | 10,386 | `fipsd/src/config/…` | `nostrc-9keoz` B1 |
| `src/native/**` | 8,708 | `fipsd/src/native/…` | `nostrc-9nxwk` B4 |
| `src/nostr/**` | 6,554 | `fipsd/src/nostr/…` | `nostrc-8hvs6` B3 Nostr |
| `src/control/**` | 6,119 | `fipsd/src/control/…` | `nostrc-9keoz` B1 |
| `src/peer/**` | 5,996 | `fipsd/src/peer/…` | `nostrc-9keoz` B1 |
| `src/instr/**` | 1,629 | `fipsd/src/instr/…` (optional profiler) | `nostrc-iq1n9` B5 tools |
| `src/utils/**` | share of ≈3,900 | `fipsd/src/utils/…` | `nostrc-9keoz` B1 |
| `src/mdns/**` | share of ≈3,900 | `fipsd/src/mdns/…` | `nostrc-gfi8h` B2 |
| `src/time/**` | share of ≈3,900 | `fipsd/src/time/…` (host clock adapter) | `nostrc-9keoz` B1 |
| `src/dnsmsg.rs`, `src/hosts.rs` | share of ≈3,900 | `fipsd/src/dns/dnsmsg.c`, `fipsd/src/hosts/…` | `nostrc-9nxwk` B4 |
| `src/cache.rs`, `src/noise.rs` | share of ≈3,900 | `fipsd/src/{cache,noise}/…` (core adapters/re-exports, not duplicate engines) | `nostrc-9keoz` B1 |
| `src/identity/**` | not itemized | `fipsd/src/identity/…` (core adapter) | `nostrc-9keoz` B1 |
| `src/perf_profile.rs` | not itemized | `fipsd/src/perf_profile/…` | `nostrc-iq1n9` B5 |
| `src/version.rs`, `src/lib.rs` | not itemized | `fipsd/src/version/…`, `fipsd/src/host/…` (host assembly) | `nostrc-s50dj` foundations; `nostrc-9keoz` B1 |
| `src/{testutil.rs,packaging_tests.rs}` | excluded test-only support | `fipsd/src/test_support/…` (test builds only) | `nostrc-f66sr` verification |

Root total: ≈176,800 non-test / 306,157 total. Within `transport`, the assessment
breaks out BLE 16,837; UDP 6,281; Ethernet 5,623; TCP 4,918; Tor 4,149; Nym 1,652;
SOCKS5 1,343; shared ≈5,200. These are included in 45,991, not extra work counts.
Verification `nostrc-f66sr` owns the cross-cutting differential/interop gates for
all rows, even where only the implementation epic is shown.

## Pin-bump procedure

1. **Choose and resolve a candidate.** Record its full commit, date, package
   version, release-tag ancestry and reason for selection. Keep the Rust checkout
   read-only; compare committed trees, not its working files.
2. **Run the watch-set diff before changing the pin:**
   ```sh
   /path/to/nostrc/scripts/fips-upstream-diff.sh -C /path/to/fips <candidate>
   # Or compare two explicit baselines:
   /path/to/nostrc/scripts/fips-upstream-diff.sh -C /path/to/fips <old> <candidate>
   ```
   One revision reads the old commit from this file's `Pinned commit:` line.
   Exit **0** means no watched path changed; **1** means review is required;
   **2** means invalid usage/repository/revision. Save the per-file status,
   CHANGELOG hunk and generated checklist. A CHANGELOG-only edit also requires
   review. The watch set is a tripwire, not proof that changes elsewhere cannot
   affect wire behavior; inspect the full upstream delta too.
3. **Regenerate oracle fixtures with `tools/fips-vectors`.** See
   [`tools/fips-vectors/README.md`](../tools/fips-vectors/README.md) for invocation
   and gaps; regenerate with both old and candidate trees recorded in fixture
   provenance. Regenerate every
   checklist family. A wire-formats change requires the full protocol sweep;
   retain rekey legacy-0.5.x cases, malformed/authentication-failure inputs,
   replay cases, and host golden snapshots rather than overwriting history
   without comparison. Exported vectors alone do not prove the differential/interop
   gates.
   For legacy rekey, review/rebase
   `tools/fips-vectors/upstream-patches/<full-rev>-legacy-rekey-export.patch`
   onto this exact candidate pin; retain the actual upstream models and rename
   the patch/update its PIN. Run `tools/fips-vectors/scripts/capture-legacy-rekey.sh`
   with the read-only source repository and a fresh output directory under the
   nostrc worktree. It applies only to a disposable copy, runs the ignored
   `slow-tests,test-hooks` export twice, and requires identical JSON/sidecars
   before collecting. Compare the old/new matrix (currently 12 traces: FMP/FSP,
   both initiators, V050=0.5.0/0.5.1, V052, V053). Update the C loader's required
   matrix on a deliberate model-structure change, never silently drop a case.
   Run `FipsDiffRekeyLegacy` and its integrity negatives at the candidate pin;
   remove copied upstream/target/build directories after verification.
4. **Run differential sweep and interop.** Run the differential sweep:
   `ctest -R '^FipsDiff' -V` in the build directory (see `tests/diff/README.md`).
   Review checked/skipped counts; an all-SKIP family is not parity. Run all
   affected C/Rust comparisons under ASan/UBSan, then two-node C ↔ pinned-candidate Rust UDP interop covering
   FMP IK, FSP XK and at least one rekey cycle. Include the legacy rekey models.
   Do not advance to another transport on vector-only evidence. Record actual
   passes, failures, skips and unavailable environments separately.
5. **Update this file and make the VERSION_MANIFEST decision together.** Change
   the machine-readable pin, date/version/rationale, invariant notes and module
   map as needed. Assess `libfips-core` and `fipsd` public API, wire, CLI,
   configuration and storage impacts under `VERSION_MANIFEST.md`; update the
   owning authoritative versions for a bump, or record the explicit no-bump
   rationale. A pin-only documentation change is not itself a shipped feature.
6. **File a child task under `nostrc-f66sr.6`** for the candidate's tracking and
   differential/interop evidence. Link the diff/checklist, fixture provenance,
   compatibility decision, version decision and any remaining work. The
   orchestrator owns issue-status changes; do not silently move the baseline
   past failing or unavailable gates.

## Node bookkeeping port (nostrc-u50yr.4)

`proto/rate_limit` and `node/{rate_limit,stats_history}` now have C homes in
`include/fips/` and `src/`. Clock-generic operations take `fips_instant now`;
none reads a clock. Protocol millisecond intervals retain their exact integer
parameters, with the construction instant representing millisecond zero;
timestamps are quantized before subtraction, including sub-millisecond inputs.

Mirrored limits: 4096 per-address entries, sweeps at max(1 ms, max_age/8),
fail-open admission for unknown addresses at capacity; 100-token/10-per-second
handshake defaults, independent established bucket with a 1-per-second floor
and shared pending count; 300-second setup/signing idle expiry; signing
defaults 256 burst / 32 per second; 3600 fast and 1440 slow stats samples,
downsample every 60 pushes, peer eviction at 86400 seconds of silence.

Rust's node BTreeMaps have **no fixed numeric entry capacity**: their host
peer table / retention policy bounds the keys. C takes caller-owned sorted
arrays with explicit capacities, never grows them, and reports FIPS_ERR_RANGE
transactionally on a new-key capacity refusal (out/state unchanged). Normal
idle eviction matches Rust's successful-admission / end-of-tick boundaries;
explicit cleanup lets the host reclaim expired keys before a capacity retry.
Sorted-array insertion moves entries but performs no allocation. Pending
handshake guards become noncopyable-by-contract C slots with explicit,
idempotent release on every exit and a borrowed limiter lifetime. Non-finite or
unrepresentable token waits return FIPS_ERR_RANGE rather than Rust's panic
for a non-finite Duration conversion. Finite waits use exact binary64
rounding to nearest nanoseconds, ties to even, like Rust. Rates must be finite and nonnegative;
lookup signing retains the zero/negative-burst unlimited convention.

Stats queries expose the same Series fields and newest-last values through
caller buffers; the walk hook marks NaN and both infinities as JSON null.
Node counter decreases saturate to zero; peer decreases/absences rebaseline
and emit NaN. Duplicate peer snapshots retain upstream slice-order pushes.
The u64 tick counter is rebased at overflow while preserving full backfill
and modulo-60 cadence (rather than wrapping/panicking as Rust would).

All 18 tests in node/tests/rate_limit.rs, 2 in rate_limit_sign.rs, and 22 in
stats_history.rs are ported by name, plus C capacity/eviction/error edges.
The proto primitive has no attached upstream unit-test module at this pin;
C adds interval, capacity, sweep and backoff boundary tests. No `ratelimit`
or stats-history differential-vector family is provided yet: public APIs
are reachable, but a trace schema/exporter/replay is a remaining gap, also
listed in tools/fips-vectors/README.md. Existing diff/cases.c is unchanged.

Version decision: additive APIs and static math linkage fold into initial
libfips-core **0.1.0 Unreleased**; no release/tag change.

## Coordinate cache port (nostrc-u50yr.5)

`core/src/cache/{coord_cache,entry,mod}.rs` now have C homes in
`include/fips/cache/coord_cache.h` and `src/cache/`. Public API takes
`fips_instant now` and converts it once against the init epoch into the
cache-relative millisecond clock the entries record; TTLs stay plain
`uint64_t` milliseconds so a host maps `node.cache.coord_ttl_secs` (or the
`test-overrides` key `node.cache.test_coord_ttl_secs`) by multiplying by 1000.

Mirrored by name: `DEFAULT_COORD_CACHE_SIZE` 50,000, `DEFAULT_COORD_CACHE_TTL_MS`
and `VERIFIED_TTL_MS` 300,000, `ADDRS_PER_ENTRY` 16, `PROBATION_SHARE_DIVISOR`
32 (probation bound `max(1, max_entries / 32)`, address bound
`max_entries * 16`), `Funding`, `Segment`, `HintOutcome`, `Placed`,
`CoordSource`, `CacheStats` and `CacheError`. Eviction order, the parked
live-verified set, lazy restamping of touched keys, the per-cache insertion
sequence as tie-breaker, probation-only eviction for unpaid writes, promotion
charges and the stated-write exceptions follow `coord_cache.rs` line for line.

Structure substitutions: the hashbrown map keyed by `KeyedHasher` becomes an
open-addressing table over caller slots hashed with libsodium SipHash-2-4 under
a 16-byte key drawn from the host `fips_entropy` at init (upstream SipHash-1-3;
the hash is not wire-visible); the three `BTreeMap<(stamp, seq)>` indexes become
binary heaps with per-slot positions. Rust's map grows for verified writes past
`max_entries`; C takes `slot_capacity >= FIPS_COORD_CACHE_SLOTS(max_entries)`
plus any headroom the host grants and returns `FIPS_ERR_RANGE` transactionally
when a verified (or funded probation) write finds no free slot. `TreeCoordinate`
is held inline as `fips_cached_coord` (entries with address, sequence and
timestamp, at most `FIPS_CACHED_COORD_MAX_ENTRIES` = 80; upstream has no depth
constant beyond the u16 wire count and a frame's roughly seventy addresses)
until `nostrc-82g9n.1` lands the proto coord codec. `iter` is a visitor walk in
slot order; upstream order is random per process and nothing may depend on it.
The test-hooks accessors (`eviction_examined`, `segment_of`, `probation_len`,
`stored_addrs`) are always compiled.

All 66 tests in `cache/tests/coord_cache.rs`, 6 in `tests/entry.rs` and 4 in
`tests/mod.rs` are ported by name, plus C storage/geometry, TTL-boundary,
epoch and deletion-chain edges. No cache differential-vector family exists;
`tests/diff/cases.c` is unchanged. `bench/coord_cache.c` ports
`benches/coord_cache.rs` behind `FIPS_ENABLE_BENCH` (not a ctest).

Version decision: additive APIs fold into initial libfips-core
**0.1.0 Unreleased**; no release/tag change.

## Coordinate primitive (nostrc-82g9n.1)

`proto/coord.{h,c}` mirrors `core/src/proto/coord.rs`: metadata entries,
self-to-root paths, equality (including metadata), address-only suffix/LCA and
ancestor routing, exact UTF-8 Debug, required/optional address-only wire fields.
Entry wire encoding is addr(16) + sequence(u64 LE) + timestamp(u64 LE), as
consumed by `proto/stp/wire.rs`; address-only fields omit both metadata words.

There is **no upstream maximum-depth constant** and Rust constructors take an
unbounded Vec. `decode_coords`/`decode_optional_coords` read a **u16 entry
count**, so the complete wire-representable range is 1..65535 entries, depths
0..65534. C's inline capacity is 65535, not the cache's guessed 80. Constructors
reject larger paths with MessageTooLong instead of Rust encode's lossy u16
cast; empty constructors report BadCoord(EmptyCoordinate). Required zero-count
wire input reports Malformed("coordinate with zero entries"), optional reports
absence; truncated fields report MessageTooShort. C preflights whole fields to
preserve outputs and Reader/Writer cursors, with cumulative codec offsets.
Objects are about 2 MiB; hosts must use heap/static storage, not small stacks.

**Cache follow-up:** `fips_cached_coord` deliberately remains the existing
80-entry compact placeholder. Blindly substituting the full inline type in
`fips_cache_entry`/`fips_coord_slot` would inflate the 50,000-entry cache from
about 140 MB to over 100 GB. Replace the placeholder in a separate storage
change: introduce caller-owned bounded coordinate-entry arena/slices, adapt
`src/cache/entry.c` copy/equality/access helpers and `src/cache/coord_cache.c`
insert/update/get/visitor ownership, and port cache tests/bench to the arena.
Define explicit capacity refusal for paths larger than host storage, rather
than truncation or treating 80 as a protocol maximum. The placeholder entry
is now named `fips_cached_coord_entry` solely to avoid collision with the real
NodeAddr-typed `fips_coord_entry`; its layout and cache behavior are unchanged.

All 13 behaviors in `core/src/proto/stp/tests/coordinate.rs` are ported;
`coord.rs` has no inline unit tests at this pin. Additional C tests cover the
u16 maximum and excess constructor lengths, every truncated byte offset,
transactional errors, metadata entry LE encoding, suffix-vs-prefix semantics,
and exact Debug formatting/capacity. Proto diff checks coords and coords-empty,
plus both common Debug strings derived from common identity seeds. Other wire
cases/catalogs remain per-owner SKIPs, not parity. Fuzz seeds are the two
unmodified coordinate vector byte strings.

Version decision: additive APIs fold into initial libfips-core **0.1.0
Unreleased**; no published version/tag change.


### Bloom port boundary (nostrc-82g9n.3)

`core/src/proto/bloom/{core,wire,state,limits}.rs` maps to
`src/proto/bloom/{core,wire,state}.c` and `include/fips/proto/bloom/`.
The bounded bloom delivery instance in `src/proto/bloom/delivery.c` follows
the `proto::mmp::delivery::Acks` dependency of upstream `state.rs`; it is not
an MMP codec or runtime. C coalesces pending retries before spending another
credit, and returns caller-buffer actions/refusal evidence instead of IO.
The estimate's private C fdlibm logarithm is translated from Rust libm
0.2.16 `src/math/log.rs` (FreeBSD/Sun e_log.c, notice retained), the version
in upstream Cargo.lock; crate checksum
`b6d2cec3eae94f9f509c767b45932f1ada8350c4bdb85af2fcab4a3c14807981`.
The IEEE evaluation order and pinned reference bits are described in README.

### Routing port boundary (nostrc-82g9n.4)

`core/src/proto/routing/{core,wire,state,limits}.rs` maps 1:1 to
`src/proto/routing/{core,wire,state,limits}.c` and public headers.
RoutingView is a borrowed active-peer snapshot plus read-only cache/congestion
callbacks; the host retains direct/greedy-tree next-hop resolution. Coordinate
and bloom types/codecs are shared with STP/bloom, not duplicated. Outputs use
caller writers instead of Vec; optional decoded PathBroken coordinates borrow
caller storage. All 49 named upstream routing tests are ported. The exporter
has no routing error wire cases; this is unit-test parity, not differential
routing evidence. Version remains **0.1.0 Unreleased**.

## MMP port (nostrc-f12v2.4)

`core/src/proto/mmp/*` maps to `src/proto/mmp/` and matching public headers,
including the pin's path-MTU notification directionality, spin restart,
reference-relative OWD trend, saturating reorder count and once-per-report
ETX smoothing fixes. No host handler/serde config is ported. Caller-owned
OWD and peer storage replaces alloc collections. Tests group the 152 upstream
MMP test cases; Rust-only introspection/serde checks are adapted as documented
in README. Python binary64 pins are independently checked against the actual
read-only Rust algorithms at the existing pin. No pin change.

Version decision: additive APIs remain **libfips-core 0.1.0 Unreleased**;
no published version/tag change. `cases.c` dispatch remains coordinator-owned.

## Rekey profiles and session binding (nostrc-f12v2.2)

| Rust source at the pin | C home / contract |
| --- | --- |
| `core/src/proto/fmp/rekey/{mod,ik}.rs` | `include/fips/proto/fmp/rekey.h`, `src/proto/fmp/rekey.c`: real IK callbacks, authenticated node-level msg1 token and setup replay record, checked IN_Rekey attachment |
| `core/src/proto/fsp/rekey/{mod,xk}.rs` | `include/fips/proto/fsp/rekey.h`, `src/proto/fsp/rekey.c`: real XK callbacks, strict envelope ingress, 12-byte AAD and CoordsWarmup prompt |
| `src/node/session/{mod.rs,README.md}` | `include/fips/node/session.h`, `src/node/session.c`: caller-owned stable slots from opening through close, peer and transport/index lookups, explicit bounds/idle eviction, ordered output executor |
| `src/peer/active.rs` link binding | Completed IK sessions may be imported/bound; no automatic FMP rotation/polling or legacy link-host replacement is enabled |
| `tools/fips-vectors/src/rekey.rs` | `tests/diff/cases.c`: one checked trace, all 16 events, before/after/initial/final views, ordered outputs and authenticated deliveries; no SKIP |

Profiles fork retained Noise state on reads and erase every temporary. The
executor wraps XK sends with current coordinates on each send/resend, seals
prompts immediately with their reserved bare key/counter, preserves output order,
returns all Release leases even on send failure, and gates plaintext delivery
for all epoch paths. No timer thread, I/O, allocation or host clock is added.
The caller polls a due existing session before admitting a Setup, executes that
batch (including teardown), then looks up/admit again. The table does not hide
that ordering behind an internal side effect. Half-open accounting counts
unproved candidates, not this node's own attempts, as upstream does.

`FipsCoreRekeyProfilesTest` covers real IK/XK cycles in both roles, crossing,
byte-identical resend ladders with loss, changing coordinates around identical
XK bytes, cutover/drain/restart erasure and ordering, failed-send cleanup,
immutable failed-read forks, msg1 replay proofs and bounded table/lookup/eviction.
`fips_fuzz_rekey_profile` drives production profiles and envelopes with real
Noise, no test hooks or stub profile. Legacy compatibility and FMP/fault/restart
differential traces remain outside this one successful XK vector.

Version decision: additive APIs fold into **libfips-core 0.1.0 Unreleased**;
no published release or tag change. No fipsd/runtime wiring is claimed.
