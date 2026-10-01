# MDK interop harness

Third-party Marmot interop for the encrypted-groups release gate
(nostrc-7gx7, nostrc-77pa; privacy charter section 7.9). One side is
Groundhog's real `GhMlsService` (libmarmot). The other side is
[MDK](https://github.com/marmot-protocol/mdk) v0.8.0, the Rust library White
Noise 0.8 builds on. Both talk through the test's own local relays only.
Results and root causes are in
`docs/analysis/marmot-mdk-interop-2026-09-30.md`.

## Pins

| What | Pin |
| --- | --- |
| MDK | `v0.8.0` = `575ae29d25d58494135058d9e46affec7174e79a` (mdk-core, mdk-memory-storage, mdk-storage-traits) |
| OpenMLS | `04c50d7fb12d52f4f9aee26de5f5234f3df29fa8` (what MDK v0.8.0 locks) |
| Rust | `rust:1.93-slim-bookworm` (build stage only) |
| Everything else | `driver/Cargo.lock`, built with `--locked` |

MDK 0.9.0 and later is a different engine (cgka-engine, the adopted
profile), which libmarmot's group engine does not speak yet
(nostrc-qp24.5.1). Those versions have their own drivers and images; see
[Versioned matrix](#versioned-matrix-mdk-011-and-the-090-probe) below. This
section and `driver/` are the MDK 0.8 harness, unchanged.

## Run it

Docker is required; no Rust toolchain is needed on the host.

```sh
cmake -S . -B _build -G Ninja -DBUILD_GROUNDHOG=ON -DBUILD_MDK_INTEROP=ON
cmake --build _build --target test-groundhog-mdk-interop
ctest --test-dir _build -R '^groundhog-mdk-interop' -V
```

- `groundhog-mdk-interop-image` (a CTest fixture) runs
  `docker build -t nostrc-mdk-interop:0.8.0 tests/interop/mdk/driver`.
- `groundhog-mdk-interop` then runs the eight cases of
  `gnome/groundhog/tests/mls/test_mdk_interop.c` (since W24 also an MDK
  member leaving and Groundhog leaving, nostrc-2um6).
- The container reaches the test's relays on the host's 127.0.0.1:
  - Linux: `--network host`.
  - macOS (Docker Desktop): `MDK_DRIVER_DIAL_HOST=host.docker.internal`.
    Only the dialled host changes; the URLs, and so the NIP-42 `relay` tag,
    stay `127.0.0.1`.
- Without `BUILD_MDK_INTEROP` the test is not built. Without `GH_MDK_DRIVER`
  set, every case skips.

Knobs:

- `MDK_DRIVER_LOG=debug` (or any `tracing` EnvFilter): MDK's own diagnostics
  on stderr (default `warn`).
- `MDK_DRIVER_DEADLINE_S` (default 30): the bound on every relay wait. A hang
  becomes an error; nothing sleeps.
- `-DMDK_INTEROP_DRIVER="<command line>"`: another driver instead of the
  image, e.g. a native `cargo build --release` of `driver/`.

CI: `.github/workflows/marmot-mdk-interop.yml` runs on demand and nightly,
and keeps the verbose log as an artifact.

## Driver protocol

`driver/src/main.rs` reads one JSON object per line on stdin,
`{"id": N, "cmd": "...", ...}`. It writes exactly one answer per request, in
order: `{"id": N, "ok": true, ...}` or `{"id": N, "ok": false, "error": "..."}`.
Logs go to stderr.

Relays: the driver opens one WebSocket per operation, and answers NIP-42
AUTH:
- as the peer's account for its inbox (NIP-17 serves gift wraps only to
  their recipient);
- as a fresh key everywhere else.

| Command | Arguments | Answer |
| --- | --- | --- |
| `hello` | | MDK and OpenMLS pins |
| `peer_new` | `peer`, `secret` (hex) | `pubkey` |
| `publish_key_package` | `peer`, `relays` (tag), `to` (targets; `[]`: made, not sent) | `event` (JSON), `event_id` |
| `fetch_key_package` | `peer`, `author`, `from` | newest kind:30443 `event`; `mdk`: what `parse_key_package` makes of it (leaf/capability extensions, proposals) |
| `parse_key_package` | `peer`, `event` | same view for a given event |
| `create_group` | `peer`, `name`, `description`, `relays`, `admins`, `key_packages`, `welcome_relays`, optional `image` | group `state` and `welcomes` (gift-wrap ids per invitee) |
| `add_members` / `remove_members` | `peer`, `group`, `key_packages` + `welcome_relays` / `members` | state after the merged Commit |
| `update_group_data` | `peer`, `group`, `name`, `description`, `admins`, `image`, `publish` | state |
| `self_update` | `peer`, `group` | state |
| `send` | `peer`, `group`, `text`, `publish` | `event_id` (or the unpublished `event`) |
| `leave_group` | `peer`, `group` | MDK's leave (a SelfRemove where the group requires it, else a Remove of itself) published to the group relays, not merged: `state`, `event_id`, `mls_message` (hex) and OpenMLS's `proposal_refs` (nostrc-2um6) |
| `sync` | `peer`, `group` | every kind:445 of the group through `process_message` as a fixpoint: `results`, `failed` (MDK's error and `openmls`: OpenMLS's own verdict on a fresh copy), `state`. A leave MDK auto-commits (`type` `proposal`) is published and merged like any Commit (`auto_commit`, `auto_commit_error`) |
| `fetch_welcomes` / `accept_welcome` | `peer`, `from` / `wrapper_id` | Welcomes found (refusals carry the rumor's kind and tags) / state |
| `state`, `messages` | `peer`, `group` | group view / stored messages |
| `export_secret` | `peer`, `group`, `label`, `context`, `length` | MLS-Exporter output (vector capture) |
| `group_extension` | `peer`, `group` | MDK's 0xF2EE bytes (vector capture) |
| `welcome_bytes` | `content` | whether OpenMLS parses a Welcome |

Commits follow MIP-03: published to the group relays first, merged only once
one relay accepted (else cleared). Welcomes follow MIP-02: one NIP-59 gift
wrap per invitee, sent after the Commit.

The vectors in `libmarmot/tests/test_interop.c` (`MDK_GDE_*`,
`MDK_445_*`) were captured with `group_extension`, `send` with
`"publish": false`, and `export_secret`.

## Versioned matrix: MDK 0.11 and the 0.9.0 probe

The adopted-profile release gate (nostrc-a5u5, nostrc-7gx7;
`docs/analysis/marmot-adopted-profile-gap-2026-09-30.md`, "Harness matrix and
acceptance evidence"). Each MDK version has its own directory, `Cargo.lock`
(built with `--locked`), Docker image and CTest names, so a green 0.8 run can
never stand in for an adopted one.

| Directory | Image | MDK pin | Profile (`hello`) | Role |
| --- | --- | --- | --- | --- |
| `driver/` | `nostrc-mdk-interop:0.8.0` | `v0.8.0` = `575ae29d…` (mdk-core) | (none: contract 1) | legacy MIP peer (above) |
| `driver-0.11/` | `nostrc-mdk-interop:0.11.0` | `v0.11.0` = `946e0547485c9a2c393c2048ec3a968fd50fb441` | `marmot-adopted` | adopted peer |
| `driver-0.9-probe/` | `nostrc-mdk-interop:0.9.0-probe` | `v0.9.0` = `a102b1966267c5bfcbe3a822212c0e343ac109ef` | `marmot-dictionary-proof-v1` | expected-incompatible KeyPackage probe |

The 0.11 peer is the account-device stack White Noise 0.11 runs:
cgka-session's `AccountDeviceSession` (cgka-engine on SQLCipher
storage-sqlite, `ProtocolProfile::Current`, the pinned v1 convergence
policy), the real `NostrMlsPeeler` (kind 445 sealing, NIP-59 Welcomes
1059 -> 13 -> 444) and the transport-nostr-adapter's kind 30443 KeyPackage
publication. What reaches the wire matches White Noise 0.11:

- **Session configuration.** `app_feature_registry()` and
  `supported_app_component_ids()` are verbatim copies of marmot-app's at
  `946e0547` (`crates/marmot-app/src/lib.rs`). Leaves and KeyPackages
  therefore advertise SelfRemove (proposal 0x000a), the three
  agent-text-stream-QUIC roles as Optional private-use extensions
  (0xF2D1, 0xF2D2, 0xF2D4; advertising needs no QUIC transport) beside
  0x0006, and the component set with 0x8006 and the 0x8009 proof.
- **Parity check.** `cargo test` (run by the image build, so a drift fails
  the `-image` fixture and every case) compares both function bodies with the
  pinned marmot-app source, read from Cargo's git checkout of MDK. It also
  checks that marmot-app's `SessionConfig` chain is still exactly the proof
  signer, these two functions and wire-neutral settings (hydration timing,
  the test-only convergence override, the audit recorder). The control case
  asserts the resulting wire shape on every MDK KeyPackage.
- **KeyPackages** come from `fresh_key_package()`, which builds them with
  the same engine function (`build_fresh_key_package`) as marmot-app's
  lifecycle staging. Their events carry White Noise Android's `client` tag
  (`White Noise Android`, whitenoise-android `6186a253`; `peer_new`
  `"client": null` drops it, as for a generic MDK consumer). Fetched
  KeyPackages pass the checks of marmot-app's relay-fetch path (strict
  cutover: current profile only), and the newest wins with marmot-app's
  tie-break (equal `created_at`: the smaller event id).

Not matched: the relay I/O is the driver's own, as in the 0.8 driver.
MDK 0.11's relay plane (nostr-sdk, NIP-65 discovery, the KeyPackage
lifecycle's publish/retire bookkeeping, client-priority selection among
several KeyPackages) is not exercised. Pins beyond MDK: OpenMLS fork
`59e7d3b2…`, nostr fork `a9c7a642…` and MDK's two `[patch.crates-io]`
entries, all as MDK v0.11.0 locks them; Rust 1.97.1 (MDK's toolchain pin).
Every crate version in `driver-0.11/Cargo.lock` is the one in MDK v0.11.0's
own lock.

MDK v0.9.0 leaves carry the superseded `0xF2F1` v1 account proof, which
neither MDK 0.11 nor libmarmot accepts. The probe makes and publishes
KeyPackages exactly as 0.9's marmot-app does (Rust 1.90.0, crates.io nostr
0.44.2 and OpenMLS 0.8.1 as 0.9 locks them), with 0.9 marmot-app's feature
registry and component set (verbatim, under the same parity tests against
`a102b196`) and its hard-coded event tags. It does nothing else: no group
operation is expected to interoperate, so none is implemented.

### Run it

```sh
cmake -S . -B _build -G Ninja -DBUILD_GROUNDHOG=ON -DBUILD_MDK011_INTEROP=ON
cmake --build _build --target test-groundhog-mdk011-interop
ctest --test-dir _build -R '^groundhog-mdk011-interop' -V
```

| CTest | What | Expected |
| --- | --- | --- |
| `groundhog-mdk011-interop-artifacts-reset` | empties `vectors.jsonl` (fixture) | pass |
| `groundhog-mdk011-interop-image`, `-image-mdk09` | `docker build` of the two images, parity tests included (fixtures) | pass |
| `groundhog-mdk011-interop-control` | MDK 0.11 <-> MDK 0.11 on the test's relays: KeyPackage publish/fetch/admit (White Noise wire shape asserted), create, Welcome 1059 -> 13 -> 444 (rumor `e` = the consumed KeyPackage, `relays` = G), kind 9 both ways, rename, add (third member joins), remove, self-update, SelfRemove leave committed by the admin; both GroupContexts equal | **pass** (proves driver, relays and flows) |
| `groundhog-mdk011-interop-groundhog-invites-mdk` | Groundhog (default and legacy mode) asked to invite an MDK 0.11 user | XFAIL `unsupported`: libmarmot `MARMOT_ERR_VALIDATION`, row NOT_SET_UP, `NO_KEY_PACKAGE`, nothing published (copy: nostrc-ncp0) |
| `groundhog-mdk011-interop-mdk-invites-groundhog` | MDK 0.11 asked to invite Groundhog | XFAIL `unsupported`: MDK cannot decode the legacy MIP-00 KeyPackage (no MLSMessage framing), nothing published |
| `groundhog-mdk011-interop-adopted-welcome` | an adopted Welcome reaches Groundhog (MDK 0.11 invites a second, MDK device of Alice's account) | XFAIL `unsupported`: libmarmot records it failed, "welcome content decode failed"; no invitation, no group |
| `groundhog-mdk011-interop-mdk09-probe` | an MDK 0.9.0 KeyPackage, to MDK 0.11 and to Groundhog | XFAIL `unsupported`: MDK 0.11 "unsupported proof version 1"; Groundhog as for 0.11 |

Expected failures are never green. An adopted case asserts today's refusal
precisely (failure class, nothing published, no group or invitation, bounded
waits so no stall, no crash), then exits 77, which `SKIP_RETURN_CODE` reports
as **Skipped**, not Passed. A refusal of another shape fails, and so does an
unexpected success (`XPASS: ... update the expectation`). A case that cannot
run (driver unset) exits 77 too, and a `-p` path that matches no case exits
1. Each case runs alone (`-p`), so one case's XFAIL cannot hide another's
result.

Reading the results: ctest's footer counts a Skipped test as passed ("100%
tests passed"); its "The following tests did not run: ... (Skipped)" list is
the one that names the XFAILs. Likewise the CI job's conclusion is green when
the control passes and nothing fails: "not green" for the adopted cases lives
in the job summary table, by design.

Timings (macOS 27, Docker Desktop, 14 CPUs): a cold `driver-0.11` image build
248 s, a cold probe build 95 s, an incremental image rebuild after a driver
edit about 30 s; then about 10 s for the control and 1.3 to 5 s for each
adopted case (21 s for the whole matrix with both images cached).

Knobs, beyond those of the 0.8 harness:

- `-DMDK011_INTEROP_DRIVER="<command line>"` / `-DMDK09_PROBE_DRIVER=...`:
  another driver instead of the image, e.g. a native
  `cargo build --release` of `driver-0.11/` (whose `rust-toolchain.toml`
  selects 1.97.1).
- `MDK_DRIVER_ARTIFACT_DIR`: the 0.11 driver appends to `vectors.jsonl`
  there the wire objects it makes or sees (signed kind 30443 events, made and
  fetched with the admission verdict; unwrapped Welcome rumors, kind 444 with
  its tags; kind 445 events: application, Commit, SelfRemove) and the
  member-visible GroupContext per epoch. A GroupContext is
  member-confidential, not public: components known to be key-free are
  dumped as bytes, and 0x8002 (the Blossom group image's key, nonce and
  upload key) and any id not known to be key-free are redacted to their
  length and SHA-256, in answers and vectors alike. Each line carries its
  driver process's `driver_run` id; CTest empties the file before each run
  and sets the directory to `<build>/mdk011-interop-artifacts` (mounted at
  `/artifacts` in the container). No secret is ever written: no command
  returns one (there is no `export_secret`), requests are never logged, and
  each line is checked against the peers' secret keys before it is written.

CI: `.github/workflows/marmot-mdk011-interop.yml` (nightly and on demand,
`contents: read`, actions pinned by SHA, no secrets) runs the matrix apart
from the 0.8 workflow, writes a job summary in which every XFAIL is marked
"not green", and keeps the log, the JUnit report and the vectors.

### Contract 2

`driver-0.11/` keeps the 0.8 contract (one JSON object per line, one answer
per request, in order, logs on stderr, every relay wait bounded by
`MDK_DRIVER_DEADLINE_S`) and adds:

- **Profile negotiation.** `hello` takes an optional `profiles` list (the
  caller's acceptable profiles, best first) and answers `contract: 2`,
  `mdk_rev`, `openmls_rev`, `nostr_rev`, `profile`, `configured_as` and
  `commands`; a list
  without the driver's profile is refused as `unsupported`. Profile names:
  `marmot-legacy-mip` (MDK 0.8: 0xF2EE group data, contract 1, no
  negotiation), `marmot-dictionary-proof-v1` (MDK 0.9.0), `marmot-adopted`
  (MDK 0.11).
- **Failure classes.** A refusal answers `{"ok": false, "class": ...,
  "error": ...}` with `class` one of `unsupported` (a profile, framing or
  capability this peer does not speak, including another profile's account
  proof), `crypto/auth failure` (a signature, proof, credential or decryption
  check failed), `transport` (relays), `state` (the group's or request's
  state forbids it) or `internal`. MDK's error variant is named in
  `error` (`[...]`). KeyPackage views and Welcome entries carry the same
  `class`.

| Command | Arguments | Answer (0.11) |
| --- | --- | --- |
| `hello` | optional `profiles` | pins, profile, commands |
| `peer_new` | `peer`, `secret`, optional `client` (the KeyPackage `client` tag; default `White Noise Android`, `null`: none) | `pubkey` (a fresh SQLCipher session per peer) |
| `publish_key_package` | `peer`, `to` (`[]`: made, not sent); `relays` accepted and unused (adopted KeyPackages carry no relays tag) | `event`, `event_id`, `mdk` (decoded metadata: profile, ref, ciphersuite, extensions, proposals, components, lifetime) |
| `fetch_key_package` / `parse_key_package` | `peer`, `author` + `from` / `event` | `event`, `mdk`: `parsed` and metadata, or `class` and `error` |
| `create_group` | `peer`, `name`, `description`, `relays`, `admins`, `key_packages`, `welcome_relays` | `state` and `welcomes` (`to`, `wrapper_id`) |
| `add_members` / `remove_members` / `update_group_data` / `self_update` | as for 0.8 (`update_group_data`: `name`, `description`) | `state`, `commit_id`, `welcomes`, `events` (published to the group relays first, confirmed once one accepted, else rolled back) |
| `leave` | `peer`, `group` | `state` (`leave_in_progress`), `proposal_id` (a standalone SelfRemove; an admin's sync commits it) |
| `send` | `peer`, `group`, `text`, `publish` | `event_id` (kind 9 inside MLS) |
| `sync` | `peer`, `group` | the group's kind 445 ingested as a fixpoint, the engine's settlement window waited out and convergence advanced: `results` (`application`: author, kind, content; `commit`; other group events), `inputs`, `failed`, `published` (e.g. the admin's SelfRemove Commit), `state` |
| `fetch_welcomes` / `accept_welcome` | `peer`, `from` / `wrapper_id` | Welcomes found and joined (rumor kind and tags, `class` on refusal) / the joined group's state |
| `state` | `peer`, `group` | group, routing id and relays, epoch, name, description, profile, removed, members, admins, component ids |
| `group_context` | `peer`, `group` | component bytes by id (key-bearing ones as `{redacted, len, sha256}`), required proposals/extensions/components, epoch |
| `export_secret`, `group_extension`, `welcome_bytes` | | `unsupported` (0.8 vector commands; no secrets, no 0xF2EE) |

The 0.9.0 probe answers `hello`, `peer_new` and `publish_key_package`, and
`unsupported` to anything else.
