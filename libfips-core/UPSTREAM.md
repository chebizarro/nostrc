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
| `core/src/proto/path/**` | 6,783 | `libfips-core/src/proto/path/…` | `nostrc-f12v2` A4 |
| `core/src/proto/rekey/**` | 6,575 | `libfips-core/src/proto/rekey/…` (including legacy compatibility fixtures) | `nostrc-f12v2` A4; `nostrc-f66sr` verification |
| `core/src/proto/settle/**` | 5,493 | `libfips-core/src/proto/settle/…` (port last; upstream not wired to callers yet) | `nostrc-f12v2` A4 |
| `core/src/proto/mmp/**` | 4,160 | `libfips-core/src/proto/mmp/…` | `nostrc-f12v2` A4 |
| `core/src/proto/fmp/**` | 2,186 | `libfips-core/src/proto/fmp/…` | `nostrc-82g9n` A3; rekey profile: `nostrc-f12v2` A4 |
| `core/src/proto/lookup/**` | 1,752 | `libfips-core/src/proto/lookup/…` | `nostrc-82g9n` A3 |
| `core/src/proto/stp/**` | 1,574 | `libfips-core/src/proto/stp/…` | `nostrc-82g9n` A3 |
| `core/src/proto/fsp/**` | 1,461 | `libfips-core/src/proto/fsp/…` | `nostrc-82g9n` A3; rekey profile: `nostrc-f12v2` A4 |
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
