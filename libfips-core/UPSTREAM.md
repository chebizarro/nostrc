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

### Executable rehearsal (no baseline edits)

Use a fresh network clone, not an assumed-current local developer checkout.
Requirements are Git/HTTPS, Python 3.11+, Rustup and the crate's pinned Rust
1.94.1 toolchain with rustfmt/clippy; see the exporter README for installation.
Set `ROOT` to the **absolute nostrc worktree**, and keep all scratch beneath it:

```sh
ROOT=/absolute/nostrc-worktree
mkdir -p "$ROOT/build-pin-bump"
git -C "$ROOT" clone --filter=blob:none https://github.com/jmcorgan/fips "$ROOT/build-pin-bump/upstream"
# For a previously created disposable clone instead: git -C "$ROOT/build-pin-bump/upstream" fetch origin --tags
NEW=$(git -C "$ROOT/build-pin-bump/upstream" rev-parse origin/master)
git -C "$ROOT/build-pin-bump/upstream" log -1 --format='%H %cI %s' "$NEW"
git -C "$ROOT/build-pin-bump/upstream" tag --sort=-creatordate
"$ROOT/scripts/fips-upstream-bump.sh" -C "$ROOT/build-pin-bump/upstream" "$NEW" --keep
```

Save the full wrapper output. It chains the watch diff, scratch-crate candidate
rev/PIN/lock update, two ordinary exports, two legacy captures, and fixture
comparison. It never commits, updates issues, or rewrites source fixtures.
Exit 0 = byte-identical; 1 = watched/fixture changes requiring review;
2 = operational failure, **not** a pass. `--keep` prints its unique scratch path;
without it scratch is removed automatically. Repeated runs use isolated paths.
For a changed candidate, first prepare the reviewed patch below, then add
`--legacy-patch "$ROOT/tools/fips-vectors/upstream-patches/$NEW-legacy-rekey-export.patch"`.
No automatic fuzzy patch application or silent compatibility-case removal.

**Legacy patch rebase:** clone the disposable source once more into a directory
under `build-pin-bump`, detach at `NEW`, then `git apply --check --unidiff-zero`
and `git apply --unidiff-zero` the old patch there. If the check fails, port its
three test-only changes against the candidate models (new `capture.rs`, sibling
module declaration, FMP initiate visibility), resolving changed APIs explicitly.
Review the actual candidate legacy models and `testutil.rs`, update the bridge's
`const PIN` to the full `NEW`, and retain the MIT notice. Use
`git add -N core/src/proto/rekey/tests/legacy/capture.rs` in that disposable clone
so the new file is included, then save
`git diff --no-ext-diff --unified=0 -- core/src/proto/rekey/tests/legacy`
to the candidate-named patch in this crate. Check/apply the saved patch to a
second clean candidate clone before capture. The capture script now accepts
`SOURCE OUTPUT NEW PATCH`, so testing a candidate no longer requires prematurely
editing Cargo.toml or the baseline pin. Same-pin runs reuse the existing patch
unchanged. Review/rebase is an intentional human gate, not an automated claim.

For the unchanged-pin dry run, configure and replay without replacing fixtures:

```sh
git -C "$ROOT" submodule update --init --depth 1 third_party/nostrdb
cmake -S "$ROOT" -B "$ROOT/build-pin-bump/asan" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DGO_WARNINGS_AS_ERRORS=ON \
  -DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON -DBUILD_FIPS=ON \
  -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF -DBUILD_LIBHANAMI=OFF \
  -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF \
  -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF -DSIGNET_ENABLE=OFF \
  -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF
cmake --build "$ROOT/build-pin-bump/asan" --target fips_diff
# macOS (Linux supports detect_leaks=1):
export ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:halt_on_error=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
ctest --test-dir "$ROOT/build-pin-bump/asan" -R '^FipsDiff|FipsProtoSweep' -V
# Set VECTORS to the wrapper's printed Generated vector root, not the source fixtures.
for family in entropy identity codec noise proto rekey rekey-legacy; do
  "$ROOT/build-pin-bump/asan/libfips-core/tests/diff/fips_diff" "$family" "$VECTORS" || exit 1
done
```

A different candidate's provenance must match the C loader's compiled pin. Build
an isolated **source copy** with only its test baseline set to NEW to replay it;
never relax the loader or overwrite the real baseline to make a candidate pass.
Run Cargo fmt/clippy/test from the original crate (or with explicit `+1.94.1`),
and review candidate API/schema changes in scratch before baseline acceptance.
Delete the reported scratch path, disposable clones, Cargo targets (including
`tools/fips-vectors/target`, where integration tests create scratch), and C build
when finished; `git -C "$ROOT" submodule deinit -f --all` removes initialized
submodule work files. Never prune unrelated worktrees/caches.


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

## First executed rehearsal — 2026-11-15 (`nostrc-f66sr.6`)

Fresh HTTPS clone of the real upstream: **NEW =
`6aa80504baa0fa312b392009eea0e3e59fb6fe4c`**, commit date
**2026-11-14 03:01:39 UTC**, package **0.6.0-dev**. Newest tag by creation
(and newest stable version) is **v0.5.3**, targeting
`f4a5b4a8e61e340e6be7551608109a5c399f8353` (2026-10-16 18:30:51 UTC).
Master is **0 commits beyond the baseline**, **493 commits beyond v0.5.3**;
the full old→NEW tree diff is empty, not just the watch set.

**Decision:** successful same-pin dry run; no distinct commit exists to bump to.
Cargo rev/lock, exporter PIN/test expectations, README provenance, legacy patch
filename and all fixtures deliberately remain at the same immutable pin. No
Pending bump or port tasks are warranted: no watched-file or wire change and
no failing family. This is the first executed procedure instance, not a claim
that a newer upstream release was qualified. No issue commands were run; the
orchestrator owns tracking/status. No source/API/ABI/wire change: **libfips-core
0.1.0 no bump** in VERSION_MANIFEST; fipsd untouched.

Exact watch-script output (exit **0**):

```text
FIPS upstream diff: 6aa80504baa0fa312b392009eea0e3e59fb6fe4c -> 6aa80504baa0fa312b392009eea0e3e59fb6fe4c
Repository: /Users/bizarro/Documents/Projects/nostrc-wt/fips-pin-bump/build-pin-bump/upstream

unchanged CHANGELOG.md
unchanged core/src/noise/cipher.rs
unchanged core/src/noise/mod.rs
unchanged core/src/proto/bloom/tests/wire.rs
unchanged core/src/proto/bloom/wire.rs
unchanged core/src/proto/coord.rs
unchanged core/src/proto/fmp/tests/wire.rs
unchanged core/src/proto/fmp/wire.rs
unchanged core/src/proto/framing.rs
unchanged core/src/proto/fsp/tests/wire.rs
unchanged core/src/proto/fsp/wire.rs
unchanged core/src/proto/link.rs
unchanged core/src/proto/lookup/tests/wire.rs
unchanged core/src/proto/lookup/wire.rs
unchanged core/src/proto/mmp/tests/wire.rs
unchanged core/src/proto/mmp/wire.rs
unchanged core/src/proto/path/tests/wire.rs
unchanged core/src/proto/path/wire.rs
unchanged core/src/proto/routing/tests/wire.rs
unchanged core/src/proto/routing/wire.rs
unchanged core/src/proto/stp/tests/wire.rs
unchanged core/src/proto/stp/wire.rs
unchanged docs/reference/configuration.md
unchanged docs/reference/control-socket.md
unchanged docs/reference/native-api.md
unchanged docs/reference/nostr-events.md
unchanged docs/reference/wire-formats.md

CHANGELOG hunk:
(unchanged)

Differential-vector regeneration checklist:
(none: watch set unchanged)

Summary: 0 changed, 27 unchanged watched files.
```

The new wrapper ran against NEW using only a scratch crate. Both ordinary exports
were byte-identical; the existing exact-pin legacy patch applied without rebasing.
Two fresh legacy test processes each passed the ignored export test (1779 other
upstream tests filtered out), yielding **12 traces / 285 sidecars**, with all
hash/hex/length/pin/version descriptors verified. All seven families, JSON and
binary sidecars including provenance, were **byte-identical to checked-in fixtures**.
The compiled C runner was also invoked directly against this regenerated root:

| Family | Checked | Skipped | Failed |
| --- | ---: | ---: | ---: |
| entropy | 3 | 0 | 0 |
| identity | 3 | 0 | 0 |
| codec | 15 | 0 | 0 |
| noise | 83 (IK 31, XK 36, replay 16) | 0 | 0 |
| proto | 28 | 0 | 0 |
| rekey | 1 | 0 | 0 |
| rekey-legacy | 12 | 0 | 0 |

Local macOS arm64 verification: Debug, clang, warnings-as-errors, ASan+UBSan;
`ctest -R '^FipsDiff|FipsProtoSweep' -V` **10/10 passed**, including both integrity
negative suites and the locked `proto: 28 checked, 0 skipped, 0 failed` gate.
Original exporter `cargo +1.94.1 fmt --check`, clippy `--locked --all-targets --
-D warnings`, and `cargo test --locked` passed (**5 integration tests**, zero
unit tests). Shellcheck on the wrapper and capture script, all three requested
static Python checkers and `git diff --check` passed. These are local results,
not hosted CI, release or two-node C/Rust UDP interop qualification. Host interop
was not run in this test/tooling-only same-pin rehearsal; the existing interop
gate below remains required before advancing a changed baseline.

Procedure corrections: remove the developer-checkout default; allow explicit
candidate/patch capture without editing the baseline; scratch all Cargo sources
and targets; select Rust with `+toolchain` (manifest path does not select it);
resolve changed candidate dependencies in scratch; document prerequisites,
submodule setup, candidate patch rebasing, loader pin constraints and cleanup.
The wrapper stops visibly for missing tools/patches or fault-injection variables,
checks determinism before comparison, and retains evidence only on `--keep`.
A second full run without `--keep` also passed and removed its run directory;
invalid revision, fault switch, missing patch and omitted capture source were
separately confirmed to refuse with exit 2.

## Upstream findings to report

Carried from `docs/reviews/libfips-core-noise-audit-2026-11-15.md` (N-2, N-4,
N-11, N-14), all against **6aa80504baa0fa312b392009eea0e3e59fb6fe4c**.
These are tracking-ready notes, **not** reports already submitted upstream:

- **N-2 — bound explicit-counter decrypt inputs.** Upstream
  `core/src/noise/cipher.rs:297-315,346-365`
  (`decrypt_with_counter[_and_aad]`) checks minimum tag size but not the maximum
  message size. C `src/noise/cipher.c:64-67`, `aead.c:44` refuses >65535 bytes
  (and insufficient output capacity) as MessageTooLarge rather than attempting
  authentication/DecryptionFailed. Accepted stricter divergence; conforming
  u16-framed/UDP peers cannot send it. Report an upstream bound/error-contract
  clarification; do not alter valid-wire bytes.
- **N-4 — harden ReplayWindow::accept against out-of-window counters.**
  Upstream `core/src/noise/replay.rs:66-87` indexes `bitmap[diff / 64]` without
  checking `diff < 8192` on the older-counter branch; misuse through
  `NoiseSession::accept_replay` can panic. C `src/noise/replay.c:16-18` re-checks
  and leaves the window unchanged for old/seen/MAX counters. Report a guarded
  accept/refusal contract and regression for a counter older than the window.
- **N-11 — retain unauthenticated-msg1 DoS mitigation at the host.** Audit
  `src/noise/handshake.c:196-213` / `read_xk_message_1`: valid-point forged IK
  msg1 costs one DH before static authentication fails; XK msg1 costs one DH
  and accepts any valid point, without authentication. Same upstream design;
  `docs/reference/security.md:413-414` identifies handshake msg1 as the
  unauthenticated surface. Track host admission/rate limiting, not a C wire fix.
- **N-14 — empty handshake AAD means no transcript/channel binding.**
  Upstream `core/src/noise/handshake.rs:53-57` and
  `docs/reference/security.md:88` ("Deviation: Empty Associated Data"); audit
  C `src/noise/symmetric.c:100,123`, `include/fips/noise/handshake.h:103-105`.
  Only `h` sees parity/prologue/negotiated metadata; empty AEAD AAD does not
  authenticate it. Accepted 02↔03 parity flips demonstrate malleability.
  Do not build channel binding/exporters on `h`. Restoring Noise transcript
  binding requires a **versioned upstream interoperability change**, never an
  uncoordinated C-only correction.
- **H1 — restart replay evicts a live link before freshness is proved**
  (`docs/reviews/fips-port-security-review-2026-11-15.md`, `nostrc-f66sr.7`).
  Upstream `core/src/proto/fmp/core.rs` (`establish_inbound`) maps any known,
  *different* startup epoch to `RestartThenPromote`, and
  `src/node/handlers/handshake.rs` removes the existing connection before
  answering msg2, gated only by an idle/dampener heuristic. IK msg1 decryption
  authenticates the recorded static identity, not current possession: a captured
  msg1 from an earlier epoch displaces the healthy current link, and recordings
  from two epochs can alternate it. Epochs are opaque random values, so numeric
  ordering is not a fix. The C core decision and wire bytes are deliberately
  **unchanged**; fipsd holds the candidate (own index, msg2 answered, bounded,
  one per peer) and evicts only once a frame authenticates under the fresh
  msg2's keys. Report upstream: defer connection replacement to key
  confirmation/first authenticated frame, keep legitimate restart and
  retransmission semantics, and treat retired-epoch memory as supplementary.
- **I-1 — `show_links.created_at_ms` is always 0.** Found by the C<->Rust UDP
  interoperability gate (`fipsd/tests/udp_interop.py`, `nostrc-f66sr.1`) run
  against the pin on loopback: the Rust daemon's `show_links` reports
  `created_at_ms: 0` for the inbound and for the outbound UDP link while every
  other field is populated. `src/transport/mod.rs:603` (`Link::new`) starts
  `created_at` at 0, `set_created_at` (`:683`) is called only from a unit test
  (`:1997`), and the control snapshot (`src/node/mod.rs:2916`,
  `created_at_ms: link.created_at()`) publishes that 0;
  `docs/reference/control-socket.md:120` lists the field without
  qualification. fipsd reports the link's authenticated-at wall time
  (`fipsd/src/node/handlers/provider.c`, `show_links`) and keeps doing so; the
  gate does not compare the field. Report as a control-output defect (stamp
  `created_at` when the link is promoted); no C change.

- **I-2 — `fipsctl` exits 2 on a usage error where its reference says 1.**
  Found porting the client (`fipsd/src/fipsctl/`, `nostrc-iq1n9.1`).
  `docs/reference/cli-fipsctl.md` ("Exit Codes") documents `1` for an
  argument parse failure, but `src/bin/fipsctl.rs` parses with
  `clap::Parser::parse`, whose usage errors exit with clap's `2`
  (`fipsctl bogus` -> 2, verified on the pinned binary). The C client follows
  the documented contract and exits 1; `fipsd/tests/fipsctl_interop.py`
  prints both codes and does not compare them. Report as a doc/behaviour
  mismatch upstream (either exit code is defensible; the reference should say
  which).
- **I-3 — `show_tree` can publish a `u64::MAX` field while the tree converges.**
  Found by the same gate against the pinned Rust daemon: during the seconds
  between link establishment and convergence its `show_tree` reply carried
  `18446744073709551615` (a `u64::MAX` "no cost yet" sentinel). serde_json
  reads it as a u64, but every JSON reader limited to i64 (jansson, many
  shells' `jq` builds) refuses the whole reply. The C client carries such
  tokens through verbatim (`fipsctl_json_load`, `FIPSCTL_U64_TAG`) rather than
  failing the read; fipsd never emits them. Report as a control-output
  defect: publish `null` (or omit the field) for an unmeasured cost.
- **I-4 — probe/tree `display_name` differs between the two daemons, not the clients.**
  Not an upstream bug, recorded so the gate's output is not misread: upstream
  `Node::peer_display_name` prefers a hosts alias, then a configured peer
  alias, then the peer's short npub (`npub1fu64...4eg9`) and only then the
  short hex, while fipsd's control projections print the short hex
  (`126ef405...`) for every peer. Both clients print what their daemon sends;
  `fipsctl_interop.py` masks no names because each comparison is against one
  daemon. Likewise `path show` names a bare `transports.udp` instance `"udp"`
  in fipsd where upstream reports `transport: null` and resolves a pin only by
  the numeric id (`resolve_transport`, `src/node/handlers/path_control.rs`);
  the gate pins by id on both daemons and by name only where one is reported.
  fipsd follow-ups, outside the client port.
- **I-5 — a sub-millisecond path can report `bad_timestamp_echo`, which the docs call an anomaly.**
  Found by the loopback probe tests (`fipsd/tests/test_node_handlers.c`,
  `nostrc-jchpe`). The wire carries only whole milliseconds: the receiver
  report's `timestamp_echo` (u32) and `dwell_time` (u16)
  (`core/src/proto/mmp/wire.rs`), so no sub-millisecond RTT can be computed
  from it and fipsd cannot do better without a wire change. The sample is
  `our_timestamp_ms - (echo + dwell)` (`core/src/proto/mmp/metrics.rs`,
  `process_receiver_report`), where our time is the session clock truncated at
  receipt, the echo is the same clock truncated at send, and the dwell is the
  peer's own clock truncated at both ends; each truncation errs by under 1 ms,
  so a path whose true RTT is well under 1 ms yields -1, 0, 1 or 2 ms. Exactly
  0 is counted in `rtt_zero` (probe reason `sub_millisecond`); -1 fails the
  `checked_sub` and is counted in `rtt_arith_fail` (reason
  `bad_timestamp_echo`, which `core/src/proto/probe/core.rs`
  `classify_rtt_failure` ranks above `sub_millisecond`, so one -1 among many 0s
  decides the verdict). `docs/how-to/diagnose-reachability.md` calls
  `sub_millisecond` "every sample measured under 1 ms ... expected on
  loopback" and `bad_timestamp_echo` "an anomaly worth investigating", but
  upstream's tests only pin the exact-0 case
  (`test_report_counters_discriminate_the_rtt_outcomes`) and a far-future echo
  (`test_ignores_future_rtt_sample`); none treats -1 as sub-millisecond.
  Reproducer: two same-host nodes, `fipsctl probe <peer>` repeatedly, or one
  report with `echo + dwell == our_timestamp_ms + 1`; the verdict flips
  between `sub_millisecond` and `bad_timestamp_echo` with the clocks' phase.
  fipsd keeps byte and verdict parity (`libfips-core/src/proto/mmp/metrics.c`,
  pinned by `FipsCoreMmpTest`); it does not fold -1 into `sub_millisecond`.
  Report upstream: count an echo that lands within 1 ms ahead of our clock as
  `rtt_zero`, and keep `rtt_arith_fail` for overflow or a larger regression.
  The loopback interop harness (`fipsd/tests/udp_interop.py`
  `quantized_echo`, `nostrc-61t79`) therefore reads both reasons, with reports
  seen and no positive sample, as a quantized echo: the multi-node gate accepts
  it as it accepts `sub_millisecond`, and the two-node probes, which need a
  sample, retry it once as they retry `no_echo`.
- **I-6 — the native API drops datagrams at a slow client's full buffer on macOS and FreeBSD.**
  Found porting the native API (`nostrc-6ljk8`). Flows there are `AF_UNIX
  SOCK_DGRAM` (`src/native/seqpacket.rs` `pair`), and a full receiver refuses
  the daemon's write with `ENOBUFS` and raises no level-triggered writability
  event when it drains, so `feed()` (`src/native/mod.rs`) drops the datagram
  with a debug log even while the per-flow queue has room; on Linux
  (`SOCK_SEQPACKET`) the same client is lossless because the send waits.
  Reproducer: a flow whose client has shrunk its receive buffer to 4 KiB and
  reads nothing until a burst of `pending_per_flow` (16) datagrams has been
  dispatched; on Linux every datagram is read afterwards, on macOS all but the
  few the buffer held are gone (`fipsd/tests/test_native_burst.c`). fipsd counts the drop (`native.drop_client_buffer_full`)
  and, for clients that ask, avoids it: `connect`/`listen` accept an optional
  `"framing":"stream"` (C-only) answered on macOS/FreeBSD with an `AF_UNIX
  SOCK_STREAM` pair carrying a 4-byte big-endian length before each datagram,
  whose writer parks on writability (`fipsd/README.md`, "Lossless delivery to
  a slow client"). Upstream's daemon ignores the field (its params structs have
  no `deny_unknown_fields`) and libfipsnative then uses its datagram pair, and
  upstream's client never sends it, so both directions interoperate unchanged.
  Proposal: adopt the same field and framing in the daemon and in
  `fips::native::client` (reading the kind off `SO_TYPE`, as libfipsnative
  does), defaulting to stream on macOS and FreeBSD. Also stale:
  `examples/native-echo.rs` says the client module is built on Linux and
  FreeBSD only and gates `main` accordingly, but `src/native/mod.rs` builds
  `client` on macOS too; an ungated copy runs there against fipsd.
  Since `nostrc-e3ps5` fipsd also serves unframed datagram clients on macOS
  without the drop: on `ENOBUFS` its feeder parks on `EVFILT_WRITE` with
  `NOTE_LOWAT` equal to the datagram's cost in the client's buffer (XNU
  measures a connected `AF_UNIX` writer against its peer's receive space and
  wakes it on every peer read), with a queue-overflow fallback to the counted
  drop (`fipsd/README.md`). The same change would fix upstream's `feed()` on
  macOS without a protocol change; FreeBSD offers no such wake and needs the
  framing.
- **I-7 — the daemon's receive path collapses under unpaced overload.** Found
  in the 0.1.0 optimization pass (`nostrc-u19cz`). On loopback with two
  daemons and a native-API client sending 1200-byte datagrams unpaced (CI
  `perf_loopback.py`), Rust↔Rust delivers only about 55–80 Mbit/s at about 97–98%
  loss on hosted ubuntu-24.04 runners and on an aarch64 VM. The same receiver
  delivers about 420 Mbit/s at 0.6% loss when offered just under that rate: the
  pre-optimization C sender, hosted run 44700102649 reference. Once offered
  more, goodput falls to the collapsed level rather than plateauing, with the
  Rust daemon at about 200–290 µs CPU per delivered datagram.

  This is an overload collapse, not a capacity limit: work is spent on
  datagrams that are later dropped. The C daemon under the same offered load
  plateaus, at 905–935 Mbit/s with 28% loss on the same runner.

  The likely place to look is admission when the decrypt-worker queue
  (32,768 per worker, `src/node/decrypt_worker.rs`) and the native delivery
  path are saturated: shed before decrypting rather than after. Not yet
  reported upstream.

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

## Coordinate-cache scan (nostrc-oetfk)

`fips_coord_cache_scan` is C-only and has no upstream counterpart. It visits
exactly what `fips_coord_cache_iter` visits, in the same storage order, and
returns `fips_coord_cache_stats`, in one pass that finds resident slots
through the three eviction heaps rather than every slot. With too little
scratch, or heaps that do not account for `len`, it falls back to the sweeps.
`FipsCoreCoordCacheTest` cross-checks it against iter and stats under random
churn. No pin change. Version decision: additive API, **libfips-core 0.1.0
Unreleased**. Superseded in nostrc-gfvud: the scan takes no scratch and walks
a residency bitmap; see below.

## Cache generation and stats-history sync (nostrc-igg4w)

All three additions are C-only host diagnostics with no upstream counterpart,
and change no upstream behaviour.

- `fips_coord_cache_generation` advances on every call that can change what
  iter, stats or scan report apart from time (inserts, a refreshing or removing
  `get_and_touch`, `remove`, `demote`, `clear_path_mtu`, the purge/invalidate
  calls, `clear`). `fips_cache_stats` gains `next_expiry_ms`, the earliest
  `expires_at` among entries live at the stats' instant. While the generation
  is unchanged and `now` has not passed it, a scan's rows and counts stand and
  every age advances by the time elapsed. `FipsCoreCoordCacheTest` checks that
  every change to resident contents moves the generation and that a reused scan
  equals a fresh one under random churn.
- `fips_peer_stats.born` records `fast_pushes` at creation, so a peer evicted
  and re-created under the same address is distinguishable.
- `fips_stats_history_sync(dst, src)` brings an earlier state of `src` current
  by copying the samples pushed since, per ring, and the peers that arrived or
  left (a dst ahead of src is copied whole). It relies on every ring advancing
  once per tick and on slow samples being pushed every 60th; `FipsCoreStatsHistoryTest`
  syncs stale copies of all ages against a live history under churn, 24-hour
  eviction and re-creation, and checks that unchanged samples are not rewritten.

No pin change. Version decision: additive API (and two additive struct fields),
**libfips-core 0.1.0 Unreleased**.
## Coordinate-cache change feed (nostrc-x345n)

This is a C-only host diagnostic with no upstream counterpart, and it changes
no existing call's results. Every call that advances `fips_coord_cache_generation` now
stamps each slot it changed (`fips_coord_slot.changed`) and that slot's
32-slot block (a 64-bit stamp per block kept in the index words:
`FIPS_COORD_CACHE_INDEX_WORDS` grows by two words per 32 slots).
`fips_coord_cache_changes(c, since, visit, ctx)` visits, in storage order,
every slot changed after generation `since`: a resident slot (expired or not)
with its key and entry, or a vacant one. It reads one stamp for each
unchanged block, O(slot_capacity / 32 + 32 per changed block), so a host view
kept per slot (the fipsd dataplane's published cache) updates without
rereading the cache. It may report a slot whose state came out the same (a
demote), and never misses one. `FipsCoreCoordCacheTest` keeps a per-slot
mirror from the feed alone through random churn (every insert flavour,
touches, removes, demotes, purges, invalidations, clears), feeding after one
or several steps. It requires the mirror to equal every slot after each feed,
a feed from the current generation to visit nothing, and the slots visited to
stay far below a sweep per feed. A touch that does not stamp its slot fails it.
No pin change. Version decision: additive API and an additive slot field,
**libfips-core 0.1.0 Unreleased**.

## Coordinate-cache expiry index and running stats (nostrc-gfvud)

Internal storage only; every call keeps upstream's results. Upstream's
`purge_expired` is a `retain` over the whole map, once per tick. The C cache
keeps a fourth binary heap (C only) holding every resident slot by
`expires_at`, so `fips_coord_cache_purge_expired` pops what has expired and
costs O(k log len) for k removed, O(1) when nothing is due, whatever the
capacity. It removes the popped slots in ascending slot order, which is what
the previous storage-order sweep removed, in its order, so the free list and
with it where later inserts land (and the storage order iteration reports) are
unchanged. `FIPS_COORD_CACHE_INDEX_WORDS` grows by one word per slot and
`fips_coord_slot` gains `expiry_pos`. `FipsCoreCoordCacheTest` runs twin caches
through a random workload, one purging through the index and one removing what
a storage-order sweep finds expired, and requires identical counts and state
(every slot, the heaps, the map and the free list) after every step; the index
invariant is checked after every mutation in the existing map/index test.
`fips_bench_coord_cache --purge` times the purge at 1,000 and 50,000 entries.

Stats without a sweep: upstream's `stats` walks the map. The C cache keeps
the residents' `created_at` sum (mod 2^64) and an upper bound on it, so for any
`now` at or after the latest creation the summed ages are `len * now - sum`
(exact whenever `len * now` fits; otherwise, or for an earlier `now`, the
upstream per-entry saturating sum is computed by walking the residents). The
expired count and the earliest live expiry come from the expiry heap's top: a
node not yet expired has no expired descendant, so the walk is O(1 + expired).
A residency bitmap (`FIPS_COORD_CACHE_INDEX_WORDS` adds one bit per slot) gives
`iter`, the invalidations, `stored_addrs` and `fips_coord_cache_scan` the
resident slots in storage order without touching free slots; the scan no
longer takes caller mark scratch (`FIPS_COORD_CACHE_MARK_WORDS` is gone) and
stops when its visitor does, its stats coming from the totals. A host that
reads the first rows of a 50k-entry cache pays for the rows.
`FipsCoreCoordCacheTest` compares stats, iter and scan with a transcription of
the slot sweep at random instants after and before the latest write under
churn, pins the saturating fallback, checks the bitmap and totals after every
mutation, and the twin purge test compares stats with the sweep at every step.

No pin change. Version decision: **libfips-core 0.1.0 Unreleased** (storage
geometry, struct layout and the scan signature of an unreleased component;
no released consumer).
## Stats-history peer storage (nostrc-gfvud)

Upstream keeps peers in a `BTreeMap<NodeAddr, PeerStats>`. The C port kept a
caller-owned array sorted by address and inserted by `memmove`, so a peer
arriving at a low address shifted every later peer (~283 KB each; up to
~72 MB at 256 retained peers), on the live history every tick and on every
view replay. Peers now sit in stable slots `peers[0, peer_count)` in no
particular order: an arrival takes the next slot, an eviction's hole is refilled
by moving one peer from above the new count. The address order is an index
spread over the slots (`fips_peer_stats.by_addr`, a `fips_peer_rank` of slot
and address copy: `peers[r].by_addr` names the r-th peer), so a dense host
copy (`memcpy`, `realloc`) keeps both valid and `fips_stats_history_init` is
unchanged. Readers that need upstream's iteration order use
`fips_stats_history_peer_at(h, rank)`; lookups binary-search the index.
`fips_stats_history_sync` updates each peer dst already holds in its own slot
and rebuilds dst's index to src's order; `FipsCoreStatsHistoryTest` adds a
property test (slots dense, index a permutation in strictly ascending address
order, lookups agree, retention and `born` follow a model of upstream's map
under arrivals, 24-hour evictions and re-creation) and compares synced copies
by rank. Sample values, queries and every Series are unchanged.
`fips_bench_stats_history` times ticks with and without an arrival.

No pin change. Version decision: **libfips-core 0.1.0 Unreleased** (struct
layout and an additive accessor of an unreleased component).
## Transport send lanes (nostrc-085xw)

Host-only; no libfips-core or wire change. Upstream `transport::Lane`
(`Data`/`Path`) becomes `fipsd_transport_lane` (`DATA`/`CONTROL`) and the
optional `send_lane` vtable entry. TCP mirrors `tcp/pool.rs` and
`tcp_send_loop`: data queue 64, control queue 8 (`PATH_QUEUE_DEPTH`),
`try_send` refusal on both, control drained first with no starvation bound.
Tor and Nym implement `send_lane` too (`nostrc-3vl7v`, upstream
`socks5/pool.rs` `ProxiedConnection::queue`): each forwards to its inner TCP
owner's lanes. BLE does not yet (upstream does, `ble/mod.rs`); it keeps one
queue (fipsd README, BLE differences, for what it would take).

Classification, narrowed by `nostrc-3vl7v`: sealed link messages take the
control lane exactly as upstream, path messages (0x52/0x53/0x54, `node/mod.rs`
`send_path`) only. The one deliberate divergence is that fipsd also sends the
link handshakes there, IK/restart msg1/msg2 and link rekey msg1/msg2, which
upstream sends on its data lane. MMP sender/receiver reports, tree
announcements and heartbeats went back to the data lane. Rekey prompts,
session datagrams, bloom filters, lookups and Disconnect were always data.

Why, from upstream's key cutover rules (`core/src/proto/rekey`, README "Frame"
and "Poll" rows, `Config::fmp`; C `machine.c` cutover and current-epoch frame
handling): epochs are told apart by receiver index; the replay window is 8192
(`noise/cipher.rs`). An initiator cuts over when msg2 arrives and keeps the
previous epoch for the retention ceiling (120 s) until a frame under the new
epoch shows that the peer moved, then 10 s more (`drain`). A responder holds
its pending epoch (hold, 120 s at defaults) and cuts over on the first frame
under it, keeping the previous epoch 10 s from that evidence. A frame that
arrives after its epoch is retired is dropped (unknown index). So:

- A handshake message is not sealed under an epoch. Overtaking up to 64 queued
  frames, it moves no replay window and is no evidence; the frames it passes
  are opened by the epoch the peer still holds (its current one until it
  adopts, its previous one during the unevidenced drain after it cuts over).
  The first frame under the new epoch from the side that overtook is a data
  frame or prompt in FIFO order behind them. Safe.
- A heartbeat, MMP report or tree announcement sealed under the new epoch and
  sent past queued old-epoch data is that evidence: it starts the peer's 10 s
  drain while up to 64 old-epoch frames still wait. Safe only while a full data
  lane drains within 10 s (64 frames of 1400 bytes: above about 9 KB/s), so not
  provably safe; they now keep FIFO order. Path messages carry the same exposure
  upstream accepts for them.

Evidence against the pinned daemon (`FipsdTcpInteropRekeySaturatedTest`,
`...RustSmallerTest`, `fipsd/tests/tcp_interop.py --scenario rekey-saturated`):
both data lanes full through a 256 KiB/s relay (C offering twice the rate, Rust
1.1 times, both refusing throughout), the link rekeyed at the 31 s link floor:
with C as the tie-break winner C initiated (msg1 on its control lane past its
full data lane; K flip 33.8-34.5 s C, 35.9-37.0 s Rust), and with Rust winning
Rust initiated and C answered (msg2 on its control lane; K flip 37.2-37.6 s C,
36.8-37.1 s Rust); 10 of 10 each under ASan, 3 of 3 each under TSan. With Rust
offering 1.3 times, upstream's own full FIFO lane refused its handshakes (its
msg1 up to 15 times, once its msg2), which delayed the rekey to 70-160 s and
once past a 90 s deadline. On both daemons, before and after narrowing: one link id, one
session, one relay connection, `packets_recv` still rising after the flip, and
through a drain window after it zero `replay_suppressed_total`,
`consecutive_decrypt_failures`, handshake `bad_state`/`unknown_connection`,
dataplane `unknown_index`/`orphaned_index` and session
`unknown_session`/`rekey_key_mismatch`, except Rust handshake increments its
own log explains: a msg2 its full data lane refused (`bad_state`) or a
duplicate/late msg2 copy (`unknown_connection`, `LateReply`/`UnknownAttempt`).
C counts the same late/duplicate link msg2 as `unknown_connection` and logs it
under the same debug text, so the harness holds both daemons to that one
explained-increment rule (nostrc-tyukg, "Handshake msg2 classification"
below). Rust's session `bad_state` (FSP rekey
`LateReply`, a second ack for a resent setup on the data lane both daemons
use) is reported, not asserted. The loopback relay cannot stage the
slow-path case: macOS keeps about 175 KiB per loopback hop whatever the socket
buffer settings, so at any rate where 64 frames outlast the drain that FIFO
already outlasts the 30 s rekey handshake and no rekey completes; it follows
from the rules above. `FipsdTcpRekeySaturatedTest` still rekeys within
`after` + jitter + `handshake` with the narrowed classification; its pumps of
never-authenticating frames now back off 1 ms per refusal, because with
heartbeats and the rekey prompt on the data lane a pump that refilled every
freed slot at once left the responder no frame to adopt or count as liveness
(link-dead at 30 s under parallel load), which real, authenticating traffic
never does; for the same reason its link-dead timeout is now 60 s, beyond the
37 s rekey deadline.
Version decision: **fipsd 0.1.0 Unreleased**; libfips-core unchanged.

### Handshake msg2 classification (nostrc-tyukg)

Upstream (`src/node/handlers/handshake.rs` at the pin) classifies an inbound msg2
that completes nothing in two places. `msg2_body` (1951-2010): no `pending_outbound`
entry and no `peers_by_index` entry for the index is handshake `unknown_connection`
(debug "No pending outbound handshake for index"; a `pending_outbound` entry whose
connection carries no handshake is `bad_state`, and the entry is forgotten); a read
that fails while the attempt's handle is back in the leg is `bad_state` and keeps
the attempt (debug), a lost handle is `bad_state` (warn); an ACL refusal and a failed
promotion are `bad_state`. An index that is a link's (a link rekey's attempt, or a
completed dial's) goes to `msg2_to_link` (1854-1905), which maps the rekey
machine's reason (`core/src/proto/rekey/machine.rs` `judge_reply`): `LateReply`
(the index is held by the pending or a current/previous epoch, or the cycle is
`PendI`), `UnknownAttempt` and `UnknownIndex` are `unknown_connection`
(debug "Rekey msg2 for no attempt in flight, dropping", with the reason), and
`ReplyUnauthenticated` is `bad_state` (debug, "keeping the rekey cycle"; the attempt
stays); anything else is `bad_state` (warn). Nothing is sent, no session, link or
index changes. C now does the same: `hs_metrics_rekey` (`node/metrics.c`) counts those
four reasons for link machines, `rekey.c` logs them at debug under upstream's
text, and `handlers/handshake.c` `msg2` also counts an unauthenticated first-dial
read, an identity mismatch and a failed promotion as `bad_state` (the dial stays
after an unauthenticated read, as before). The control field names are upstream's
(`handshake.bad_state`, `handshake.unknown_connection`), so `show_metrics` and
`fipsctl` render them unchanged. `FipsdNodeRekeyMsg2Test` covers the first dial and a
link rekey (unauthenticated, stray, duplicate before and after the cutover), and the
rekey-saturated harness applies one explained-increment rule to both daemons.

Divergences that remain on this path, none a counter C gets wrong where it
counts: (1) upstream's catch-all `bad_state` for any other reason in a msg2 step
has no C branch; no other reason is reachable from a reply (`IndexExhausted` and
`HandshakeWrite` come from the msg1 and start paths). (2) A msg2 whose frame is
malformed is `bad_state` upstream (`Msg2Header::parse`, reached after only a
payload-length check); C's `fips_fmp_receive` frame validation drops it before
`msg2` as a dispatcher decode reject, so no handshake counter moves. (3) A C leg
whose machine is not `HANDSHAKING` (still connecting, no msg1 sent) returns
without a count, where upstream's stale-entry case is `bad_state`. (4) C
resolves a rekey msg2's index per transport (`hs_index_lookup`, any transport
only for a multipath peer) where upstream's `peers_by_index` is keyed by the index
alone, so a rekey msg2 arriving on another transport is `unknown_connection` in C
and is read upstream. (5) C refuses an ACL-revoked peer's rekey msg2 before the
machine (`outbound_rekey`, `bad_state`); upstream's `msg2_to_link` has no ACL gate.
(6) Upstream counts a msg2 it failed to send (its full FIFO data lane) as
`bad_state`; C's control lane is never refused and counts a failed send only in
`send_failed`.

Version decision: **fipsd 0.1.0 Unreleased**; libfips-core unchanged.
## BLE verified-link challenge (nostrc-o14sw)

Deliberate hardening, **not** upstream behaviour; no pin change. Upstream
(`src/transport/ble/mod.rs` `arbitrate`, at the pinned commit
**6aa80504baa0fa312b392009eea0e3e59fb6fe4c**) declines every newcomer beside a
verified link, whatever its age or direction. fipsd already relaxes that for one
case (`fipsd/README.md`, BLE "Differences from upstream" 1): inside the tie
window a verified link racing a newcomer at its own device address in the
opposite direction (the two ends' crossing dials) is settled by the node-address
tie-break, because fipsd pushes discovery to the node at once and one end can
verify the race's loser before the winner arrives. That relaxation let an
unauthenticated key claim from the connected device's own address displace a
verified link before authenticating.

The divergence now adds a state upstream does not have. A newcomer that wins the
tie-break against a **verified** link does not replace it
(`fipsd/src/transport/ble/pool.c`, `BLE_CHALLENGE`). It is pooled as a
*challenger* under an address of its own (`<device address>#<link id>`, not
parseable as a peer, never announced), so the node's per-address Noise (msg1/msg2
and `peer_link`) is attributable to it and not to the verified link. The
verified link stays authoritative until the node authenticates the challenger as
the same node, which replaces every link at that device address. A different
identity, a failed channel, the node's `close_unproven` deadline
(`handshake_timeout_secs`), or the verified link ending on its own drops the
challenger. Challengers are bounded to 2 per device address and count against
`max_connections`. Behaviour against unverified incumbents, other addresses and
settled verified links is unchanged (upstream's). Re-check this divergence at
every pin bump that touches `arbitrate`, `mark_verified` or the pool.

Version decision: **fipsd stays 0.1.0 Unreleased**; libfips-core is unchanged.
