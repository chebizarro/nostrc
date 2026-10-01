# Marmot third-party interop: Groundhog/libmarmot vs MDK 0.8 (W23)

Date: 2026-09-30. Branch `marmot/w23-mdk-interop`, rebased on `43f39adf`.
Beads: nostrc-7gx7 (the encrypted-groups release gate), nostrc-77pa.
Charter: `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md` §7.9
("The whole MLS path is compiled and shown only when `qp24.13` passes
acceptance").

## Summary

This is the first time libmarmot was run against another Marmot
implementation.
- **It did not interoperate at all.** MDK 0.8 could parse our KeyPackage.
  Beyond that, every exchange broke: group data, kind:445 content, MDK's
  Commits, our renames, and our Add Welcomes.
- **All six wire deviations were ours.** Each is fixed in libmarmot 0.11.0,
  with vectors and tests.
- **In legacy mode everything tested works both ways.** With
  `allow_unproven_members`, a Groundhog account and MDK 0.8 exchange
  Welcomes, messages and Commits (rename, admins, self-update, add, remove)
  in both directions and converge.
- **In default mode, Groundhog refuses MDK 0.8 users, deliberately.** MDK 0.8
  leaves carry no account-identity proof. The refusals are honest
  (NEEDS_UPDATE) when Groundhog invites or accepts. One path is not honest
  yet: an MDK admin adding an MDK member to a group we are in (nostrc-prrl).

Recommendation: **do not flip `GH_FEATURE_ENCRYPTED_GROUPS` yet** (see the
end).

## Setup

- **Groundhog side.** The real `GhMlsService`: real stores, transports,
  signer bus, and libmarmot 0.11.0. Accounts come from
  `gnome/groundhog/tests/mls/mls-world.h`.
- **Third party.** MDK v0.8.0 (`575ae29d25d58494135058d9e46affec7174e79a`,
  mdk-core with memory storage) on OpenMLS `04c50d7f`. It runs behind a
  JSON-lines driver (`tests/interop/mdk/driver`), built as a Docker image
  with `--locked`, so the host needs no Rust.
- **Relays.** Local store-and-serve relays only (AGENTS.md):
  - W for KeyPackages;
  - X for the NIP-17/59 inbox (gift-wrapped Welcomes), with NIP-42 AUTH;
  - G for group traffic.
- **Where the tests live.**
  - `gnome/groundhog/tests/mls/test_mdk_interop.c`, 6 cases, opt-in:
    `-DBUILD_MDK_INTEROP=ON`, CTest `groundhog-mdk-interop`.
  - Wire vectors in `libmarmot/tests/test_interop.c`, captured from MDK with
    the driver.

## Matrix

"Default" is Groundhog as it would ship. "Legacy" is libmarmot with
`MarmotConfig.allow_unproven_members`; Groundhog sets it only through a test
hook.

| # | Flow | Default | Legacy | Evidence |
| --- | --- | --- | --- | --- |
| 1 | MDK parses our kind:30443 (0x0006 leaf app_data_dictionary carrying the 0x8009 proof; capabilities 0x0006 0x000a 0xf2ee) | works | works | `parse_key_package` view in every case (nostrc-77pa) |
| 1 | MDK creates a group with us; we join from its NIP-59 Welcome (name, relays, admins converge) | works | works | mdk-invites-groundhog |
| 1 | Messages MDK -> Groundhog and Groundhog -> MDK | works | works | same |
| 1 | MDK Commits (rename, self-update) applied by us; our rename applied by MDK | works | works | same |
| 1 | MDK admin removes us: group ends, "removed by" the MDK admin | works | works | same |
| 1b | MDK group with a second MDK member invites us | honest refusal on Accept: NEEDS_UPDATE, "Someone uses an app that can't prove their account yet…", Welcome recorded failed and delisted. It is still listed before Accept (nostrc-ho1z). | works: joined, three-way messages | mdk-group-unproven-member-{default,legacy} |
| 1c | MDK admin adds an MDK member to a group we are in | **not honest**: we refuse the Commit and stall at the old epoch for good, and the view says "Waiting for an earlier change…" (nostrc-prrl) | works (as 1b/2b) | mdk-adds-unproven-member |
| 2a | We invite an MDK user | honest refusal: the New Group check row says "Needs to update their app…"; the service says NEEDS_UPDATE, "Nothing was changed"; nothing is created or published | n/a | groundhog-invites-mdk-default |
| 2b | We create a group and invite MDK; MDK joins from our Welcome | refused (2a) | works | groundhog-invites-mdk-legacy |
| 2b | Messages both ways | | works | same |
| 2b | Our rename and admin change applied by MDK; MDK's rename applied by us | | works; converged (epoch, name, members, admins) | same |
| 2b | MDK adds a second Groundhog account (Bob), who joins from MDK's Welcome; three-way messages | | works | same |
| 2b | MDK removes Bob (Bob's group ends, "removed by" the MDK admin); we remove MDK (MDK ends Inactive) | | works | same |
| - | An MDK member leaves (SelfRemove proposal) | not supported (W23) | not supported (W23) | libmarmot returned UNSUPPORTED for standalone proposals; works since W24, see the addendum below |
| 3 | Two Groundhog devices on one account | out of scope | out of scope | Marmot multi-device is non-normative (`ideas/multi-device.md`); nostrc-yaa1 |
| 3 | Two Groundhog installs, different accounts, next to MDK | | works | 2b (Alice and Bob: separate GhMlsService, stores, relays sessions) |

Runs:
- Six of six pass with `ctest -R groundhog-mdk-interop`, repeated three
  times in a row.
- Two full `ctest -j 8` runs: 449/450 and then 450/450. The one failure is an
  unrelated gnostr flake, nostrc-gzdn.

## Root causes, who deviates, and fixes

Spec references are the legacy Marmot text MDK 0.8 implements
(marmot-protocol/marmot `cc73aa8`, 2026-07-02, MIP-00..03) and RFC 9420.

### D1. kind:445 content encryption (MIP-03): libmarmot deviated

- **Symptom.** MDK could open none of our kind:445 events (messages or
  Commits).
- **Spec.** MIP-03, `content`: `base64(nonce || ChaCha20-Poly1305(key, nonce,
  MLSMessage, aad ""))`, with `key = MLS-Exporter("marmot", "group-event",
  32)` (03.md lines 40-83, 296).
- **What libmarmot did.** It NIP-44-encrypted with the raw exporter_secret
  as a secp256k1 key. That is MIP-03 before marmot #48.
  - MDK 0.8 still tried that as a legacy fallback, but only until
    2026-05-15 (`LEGACY_EXPORTER_SECRET_MIGRATION_DEADLINE = 1_778_803_200`
    in `messages/decryption.rs`).
- **Fix (0.11.0, `messages.c`).**
  - ChaCha20-Poly1305 under the MIP-03 key. It fails closed on short or
    tampered content.
  - The old format is neither written nor read.
  - Application content is zero-padded to NIP-44's buckets inside the MLS
    PrivateMessageContent (RFC 9420 §6.3.1), since the new AEAD pads
    nothing.
  - Tests: vector `MDK_445_*` (an MDK kind:445 and its epoch key) and a
    round trip, in `test_interop.c`.

### D2. marmot_group_data encoding (MIP-01 v2): libmarmot deviated

- **Symptom.** MDK could not decode our group data, and we joined MDK's
  groups without name, relays or admins.
- **Spec.** MIP-01, "Marmot Group Data Extension": every vector uses QUIC
  varint length prefixes (01.md line 71). `admin_pubkeys` is one outer
  varint over concatenated keys (line 138). The image fields are
  `opaque<V>`.
- **What libmarmot did.** A u32 admins length, and a `has_image` byte
  followed by fixed-size fields.
- **Fix (`extension.c`).**
  - Now MIP-01 v2 exactly. `MDK_GDE_*` vectors, captured from MDK, decode
    and re-encode byte for byte.
  - The 0.10.0 layout is still read (strict fallback), but never written.
  - Later-version fields are kept in the new `extra`/`extra_len` (MIP-01
    forward compatibility).
  - A Welcome whose group lacks exactly one valid 0xF2EE is now refused
    (`welcome.c`). It used to be joined half-empty.

### D3. Commits as PrivateMessages (RFC 9420 §6.3): libmarmot gap, MDK conformant

- **Symptom.** Every MDK Commit failed on our side ("MLS protocol error").
- **Spec.**
  - RFC 9420 lets handshake messages travel as PrivateMessages.
  - MDK uses OpenMLS `MIXED_CIPHERTEXT` (`groups.rs` line 1586), so its
    Commits are encrypted.
  - MIP-03 constrains only SelfRemove proposals to PublicMessage
    (03.md line 145).
- **What libmarmot did.** It sent every PrivateMessage to the application
  decryptor.
- **Fix (`messages.c`, `commits.c`, `mls_group.c`).**
  - Handshake PrivateMessages are routed by their clear `content_type`.
  - The sender is resolved from the sender data against the state that
    judges the Commit: current epoch, retained parent, contested removal, or
    deferred replay.
  - The Commit is decrypted with the sender's handshake ratchet, which is
    restored afterwards.
  - The signature is verified with `wire_format = mls_private_message`, and
    that wire format goes into the confirmed transcript. There is no
    membership tag.
  - Proven by case 1 (MDK rename, self-update and removal applied by us).

### D4. required_capabilities missing (MIP-01): libmarmot deviated

- **Symptom.** MDK refused our renames and admin changes:
  `InvalidCommit(GroupContextExtensionsProposalValidationError(ExtensionNotInRequiredCapabilities))`.
  This is OpenMLS check valn1001: every non-default extension in a
  GroupContextExtensions proposal must be listed in that proposal's
  required_capabilities.
- **Spec.** MIP-01, "Required MLS Extensions": "All groups MUST include …
  `required_capabilities` … `ratchet_tree` … `marmot_group_data`" (01.md
  lines 26-33 and 376).
- **Fix (`groups.c`).**
  - New groups carry `required_capabilities {extensions [0xF2EE], proposals
    [], credentials []}`. That is byte for byte what MDK computes when an
    invitee lacks SelfRemove (its LCD rule, `groups.rs` 1535-1575).
  - A metadata Commit adds it to older groups.
  - Test: `test_group_context_required_capabilities`.
- **Remaining deviation.** MIP-01 also requires `self_remove` (0x000a) as a
  required proposal type. libmarmot implements no SelfRemove, so it cannot
  require it (nostrc-2um6). MDK relaxes the same rule for mixed groups.

### D5. Add Welcomes without a relays tag (MIP-02): libmarmot deviated

- **Symptom.** MDK refused our Welcome with `InvalidWelcomeMessage`: the
  rumor had `e`, `encoding` and preview tags, but no `relays`.
- **Spec.**
  - MIP-02 lists `relays` among the required fields (02.md lines 60-79).
  - MDK's `validate_welcome_event` enforces it.
- **What libmarmot did.** `marmot_create_group()` wrote the tag, but
  `marmot_add_members()` passed no relays. Groundhog always adds invitees
  through an Add.
- **Fix.** The tag now comes from the group's marmot_group_data. Test in
  `test_multi_member_commits_converge`.

### D6. A finally refused Welcome stayed an invitation: libmarmot bug (not wire)

- **What happened.** When accept failed for good (for example, an unproven
  Welcome-tree leaf), libmarmot recorded the processed Welcome as failed but
  left the Welcome itself pending. The invitation reappeared on every
  attempt.
- **Fix (`welcome.c`).** Such a Welcome is saved as FAILED, as decline saves
  DECLINED. Storage errors still roll back and can be retried.
- **Test.** `expect_join_ex` asserts that no join attempt leaves a pending
  Welcome. It was mutation-checked.

### Groundhog: a refused New Group left a group of one

- **What happened.** `create_group_now` created and stored the account's own
  group before the Add that libmarmot refused. The UI said "Nothing was
  changed" while an empty group stayed listed.
- **Fix.** `invitees_ready` checks every invitee's KeyPackage for the account
  proof (`marmot_key_package_event_has_account_proof`) before anything is
  made; libmarmot still judges the whole tree.
- **Tests.** `test_unproven_invitee_needs_update` (groundhog-mls-service)
  now asserts no group and no G traffic. Interop case 2a asserts the same.

### Findings on the MDK side (no MDK deviation from the legacy text)

- **No account-identity proof in MDK 0.8 leaves.**
  - The proof (`app-components/account-identity-proof-v2.md`) is not part
    of the legacy MIP-00..03 text. MDK 0.8 conforms to its profile.
  - libmarmot 0.10.0 applies the adopted rule to legacy groups as a
    deliberate security policy (README 0.10.0, "MDK 0.8 interoperability").
  - Policy is unchanged: refuse by default, with honest copy.
    `allow_unproven_members` stays test-only in Groundhog.
- **MDK never retries a Failed event.**
  - `process.rs` blocks reprocessing Failed ids. A Commit of epoch n+1 fed
    before epoch n's is lost for good, and the peer stalls.
  - Two Commits in the same second have no created_at order, so this hit
    our run: Groundhog renamed and then changed admins within one second.
  - MIP-03 is silent on buffering, so this is MDK robustness, not a
    deviation. It hits MDK-only groups as well.
  - The driver now feeds MDK events epoch by epoch, as a careful client
    would.
  - Our mitigation: strictly increasing created_at for a group's Commits
    (nostrc-2lrz). Report it upstream too.
- **MDK's SelfRemove leave is invisible to us** (nostrc-2um6).

### Harness bugs found and fixed

- **The Docker image fixture built nothing on macOS.** `find_program(docker)`
  resolved to Docker.app's backend binary (CMake prefers app bundles), so
  the image "build" passed in 0.16 s.
  - Fixed with `CMAKE_FIND_APPBUNDLE NEVER`.
- **MDK's errors were invisible.** It logs only "Error processing MLS
  message". The driver now has three diagnostics:
  - `MDK_DRIVER_LOG` shows MDK's tracing.
  - `sync` failures carry OpenMLS's own verdict, from a fresh group copy.
  - A refused Welcome reports its rumor's kind and tags.

## Two devices on one account (item 3)

This is out of scope:
- Marmot multi-device is an idea, not protocol (`ideas/multi-device.md`:
  "Status: idea (non-normative)").
- Groundhog refuses to invite its own account.
- It looks up one KeyPackage per invitee.
- It has no device group.

Filed as nostrc-yaa1. Two Groundhog installs on different accounts are
covered, in case 2b and in groundhog-mls-service. The charter's recorded
two-device run over real relays remains a manual release step. Tests use
local relays only.

## Version decisions

- **libmarmot 0.10.0 -> 0.11.0 (MINOR).** A 0.x wire break:
  - kind:445 is incompatible both ways with 0.10.0 and older;
  - 0.10.0 cannot read 0.11.0's group data;
  - `MarmotGroupDataExtension` grows at its end.
- **marmot-gobject, gnostr, Groundhog: no bump.** No source change for the
  first two; for Groundhog, encrypted groups stay behind the flag at 0. The
  test hook and CMake option are test-only.
- Recorded in `VERSION_MANIFEST.md` and in libmarmot's README.

## How to run

```sh
cmake -S . -B _build -G Ninja -DBUILD_GROUNDHOG=ON -DBUILD_MDK_INTEROP=ON
cmake --build _build --target test-groundhog-mdk-interop
ctest --test-dir _build -R '^groundhog-mdk-interop' -V   # builds the image first
MDK_DRIVER_LOG=debug ctest --test-dir _build -R '^groundhog-mdk-interop$' -V
```

- CI: `.github/workflows/marmot-mdk-interop.yml` (on demand, nightly; log
  artifact).
- Details: `tests/interop/mdk/README.md`.

## Recommendation on GH_FEATURE_ENCRYPTED_GROUPS

Keep it at 0. The third-party evidence the charter asks for now exists, and
it is positive in legacy mode. What is not yet acceptable:

1. **Default mode is where Groundhog ships.** There, a Groundhog user can
   interoperate with an MDK 0.8 / White Noise 0.8 user in exactly one shape:
   a two-person group the MDK user creates (case 1). Every larger or
   Groundhog-created group is refused. The release note ("people who use
   apps that support Marmot") would still overstate this. Per nostrc-7gx7
   (2), the owner must choose:
   - ship Groundhog-to-Groundhog only, saying so in metainfo and in-app
     copy; or
   - wait for an MDK that emits the proof (the adopted profile,
     nostrc-qp24.5.1).
2. **nostrc-prrl (P1) blocks 7gx7.** A default-mode member of an MDK group
   silently stalls with false "waiting" copy when an MDK admin adds an MDK
   member.
3. **Charter acceptance (1), the recorded two-device run over real relays,
   is still manual and not done.**
4. **libmarmot 0.11.0 is a wire break.** It should land and settle, with the
   nightly job green, before any build ships with the flag on.

Also open: nostrc-2um6 (SelfRemove), nostrc-2lrz (Commit created_at
ordering), nostrc-ho1z (a pre-Accept invitation for a group we cannot join).

## Addendum (W24, nostrc-2um6): leaving, both ways

libmarmot 0.12.0 keeps standalone proposals and implements SelfRemove
(0x000a). Two new cases of `test_mdk_interop.c` pass against MDK v0.8.0
(`575ae29d`):

| # | Flow | Mode | Result |
| --- | --- | --- | --- |
| 3a | An MDK member leaves a Groundhog group. Groundhog makes groups alone, so they do not require SelfRemove (MDK's rule, after review L1). MDK's `leave_group()` therefore sends a Remove of itself as a PrivateMessage; Groundhog, the admin, keeps it, commits it by reference after its jitter and reports "member-left"; MDK applies the Commit and is Inactive. This proves PrivateMessage proposal interop: MDK resolves our reference to its own encrypted proposal | legacy (the MDK leaf is unproven) | works (`mdk-member-leaves`) |
| 3b | Groundhog leaves an MDK group: Groundhog's SelfRemove reaches G; MDK auto-commits it by reference (a PrivateMessage Commit); Groundhog ends the group as LEFT | default | works (`groundhog-leaves`) |
| 3c | Groundhog leaves a group whose only admin is MDK (re-review R1). Groundhog's Remove request is dropped by MDK 0.8's admin auto-commit, which commits an empty Commit (nostrc-laxu). Groundhog asks once more, then stops with "Your leave request wasn't processed by the group admin…" and can send again; no endless request/Commit loop | legacy | bounded: 2 requests, 2 empty Commits, then nothing (`groundhog-leaves-mdk-admin`) |

- **Vector.** MDK's SelfRemove MLSMessage and OpenMLS's own ProposalRef for
  it (driver `leave_group`, captured from a group that required SelfRemove)
  are `MDK_SELF_REMOVE_*` in `libmarmot/tests/test_interop.c`. libmarmot
  computes the same ProposalRef.
- **When SelfRemove is sent.** As MDK 0.8 decides: only when the group's
  required_capabilities list it; otherwise the leave is a Remove request for
  an admin (review M1).
- **Who commits.** Any member commits a SelfRemove: MDK 0.8, MDK 0.11 and
  the adopted spec agree. Groundhog commits after 1-4 s of jitter.
- **MDK's Remove-based leave.** Where a group does not require SelfRemove,
  MDK 0.8 leaves with a Remove of itself, sent as a PrivateMessage. An admin
  commits it (libmarmot test `test_private_remove_self_committed_by_admin`).
  MDK's own admin auto-commit of such a proposal filters for SelfRemove
  only, so it commits an empty Commit (`messages/proposal.rs`
  `auto_commit_proposal`). That is an MDK issue, not exercised here.
- **D4's remaining deviation is closed** for new groups: they require
  SelfRemove when every initial member advertises it.
- **Still open:** admins cannot leave for everyone (they step down first, and
  Groundhog does not offer that yet), and the adopted profile (nostrc-qp24.5.1).
