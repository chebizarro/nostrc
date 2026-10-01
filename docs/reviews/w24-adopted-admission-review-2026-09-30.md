# Review: W24 slice E — adopted-profile group admission and creation (nostrc-qp24.5.1, nostrc-qp24.5.1.1)

- Branch: `marmot/w24-adopted-admission` at `c28c4b22` (one commit on master `82a615e4`)
- Reviewer: independent peer review (AGENTS.md), 2026-09-30
- References: Marmot adopted spec `07da8ffb` (`app-components/account-identity-proof-v2.md`, `foundation/authorization-proofs.md`, `protocol-core/joining.md`, `app-components/admin-policy-v1.md`, `transports/nostr.md`); MDK v0.11.0 `946e0547` (`cgka-engine/src/account_identity_proof.rs`, `traits/src/app_components/routing.rs`, `marmot-app/src/key_package_records.rs`); MDK v0.8.0 `575ae29d`.

## Verdict: CHANGES-REQUIRED

Admission itself is careful and, as far as this review could find, correct:
- The 0x8009 v2 proof matches the spec and MDK byte for byte.
- The Welcome path verifies the tree, the GroupInfo, every leaf signature, every account proof, the inviter's admin standing and the seal author, in the right order.
- The GroupContext and component parsers are canonical and survived about 56M fuzz iterations under ASAN and UBSAN.
- Stored adopted states are re-validated on every load and clone.
- The fixtures are reproducible. MDK v0.11.0 joins a libmarmot-created group, which I re-ran independently.

The blocker is the Commit refusal. A Commit that removes *our* leaf never reaches the new guard in `marmot_commit_authorize()`. The MIP-01 removal path judges it instead, and that path treats a group without 0xF2EE GroupData (that is, every adopted group) as "everyone is admin". So any non-admin member can make libmarmot evict itself from an adopted group. Depending on key order, it also deletes the group's keys. Reproduced (H1).

## Gates run

| Gate | Result |
| --- | --- |
| macOS build (`cmake -G Ninja -DBUILD_GROUNDHOG=ON`, `ninja`, 2468 steps) | OK; no compiler warnings in the touched files |
| `ctest -R "mls\|marmot\|welcome\|invite\|group"` (macOS) | 34/34 passed (all 23 libmarmot suites, `groundhog-mls-service`, `-store-marmot`, `-privacy-mls`, `-mls-ui-gui`, `-group-ui-gui`) |
| `scripts/check-unsequenced-args.py` | clean |
| `scripts/linux-gate.sh --sanitizers` | passed, 49 tests |
| libmarmot's 23 suites under ASAN+UBSAN+LSan (Linux CI image, GCC; a throwaway volume, removed) | 23/23. These suites are not in the CI sanitizer list. |
| libmarmot's 23 suites under ASAN+UBSAN (macOS, Apple clang; LSan unsupported there) | 23/23 |
| Mutation fuzzing (custom harness; ASAN+UBSAN; 4 × 300 s; ≈56M iterations) over the GroupContext, ratchet tree + leaf/proof checks, v4 state load/round trip, leaf dictionaries and component codecs, seeded from the MDK captures | No report in the new code. One pre-existing allocation bug, L5. |
| Fixture reproducibility (see focus 6) | the header regenerates byte for byte; a **fresh** MDK v0.11.0 capture passes `test_adopted` 14/14 |
| Reverse interop (MDK v0.11.0 joins a group created by this build) | re-run: `"protocol_profile":"Current","members":2,"required_app_components":[32769,32771,32772,32777,32780],"result":"joined"` |
| Revert spot-checks (20 single-check removals, whole marmot suite) | 16 caught, 4 not caught (L2) |
| Merge probes (`git merge-tree` against master and slices A, B, G; E+B materialized and built) | see "Merge risk". E+B: 23/23 suites pass. |

Version bump: libmarmot 0.11.0 → 0.12.0 (MINOR) in CMake, meson, VERSION_MANIFEST and the README changelog. That's right for new public API in a 0.x component (AGENTS.md). Not bumping marmot-gobject or groundhog is justified (no source or ABI change; rebuild only). `MARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER` / meson `adopted_key_package_producer` stay OFF by default.

## Findings

### H1 (High) — A non-admin member can evict libmarmot from an adopted group and make it delete the group's keys: the removal path bypasses the adopted-Commit refusal

`libmarmot/src/commits.c:2138-2151` (and `:2041-2056` in `inbound_removed()`, `:2189-2198` on the retained parent), `:1535-1553` (`removal_key()`), `:106-108` (`gde_is_admin()`), `:1673-1688` (`removal_contested()`), `:1703-1720` (`forget_keys()`); the guard E added is only at `:228`.

The Commit refusal leaks through a path that never reaches the new guard:
1. E refuses adopted Commits in `marmot_commit_authorize()`, because under the MIP-01 rules "a group without GroupData lets any member commit".
2. A Commit that removes our own leaf never reaches that function. Staging fails, because the UpdatePath is not encrypted to the removed member, and `mls_group_commit_removes_self()` reports the removal.
3. `marmot_commit_process_inbound()` then judges the remover with `removal_key()` against the pre-Commit **0xF2EE GroupData**.
4. An adopted group has none. `group_data_of()` returns OK with NULL, and `gde_is_admin(NULL, …)` returns true, so every member counts as an admin.
5. `evict()` marks the group INACTIVE and stores the removal.
6. `removal_contested()` uses the same NULL rule. If no other member's key sorts below the remover's, the removal is final at once and `forget_keys()` deletes the MLS state, the retained parent and the exporter secrets.

Reproduced with a probe: a three-member adopted group created by `marmot_create_group_for_profile()`, where Alice is the only admin. Bob, who is not an admin, builds an authenticated Remove of Carol with `mls_group_remove_members()`, as a modified client or compromised device could.

```
MLS-layer remove by non-admin bob: rc=0
carol: marmot_process_message -> 0 (success), result type 1
carol: group lookup 0, state=INACTIVE
carol: removal record err=0 removed=1 final=1 remover_is_bob=1 remover_is_alice=0
carol: MLS state still stored: NO (deleted)
alice: marmot_process_message -> -5 (unsupported feature or version)
```

Across 8 runs with fresh random keys, Carol was evicted every time. In 4 of them the removal was final at once and her state was deleted. A remover who grinds an account key that sorts first (a few hundred tries per leading zero byte) makes deletion deterministic.

The bug persists across the other slices:
- The same result reproduces on the E+B merge.
- `removal_key()` is unchanged in slice A's current tip `fc6c896b`, whose own non-legacy refusal is also only in `marmot_commit_authorize()`.

**Failure scenario.** Any member of an MDK 0.11 group can unilaterally end the group for a libmarmot member and destroy its keys; the attacker could be a modified client, a compromised device or a malicious participant.
- admin-policy-v1.md:98-101 makes "remove another member" admin-only. MDK peers reject the same Commit, so the group carries on without the victim.
- This contradicts `marmot.h:541-543` ("those fail with %MARMOT_ERR_UNSUPPORTED and leave the group unchanged") and README.md:263 ("Refused … ours and others'").

**Test gap.** `test_adopted_commits_refused` (`test_adopted.c:1537-1603`) covers one self-update. It accepts any outcome except an applied Commit (`:1576-1577`), and it never exercises the removal path.

**Fix.**
- Refuse every Commit judgement in a non-legacy group before the removal logic. For example, return `MARMOT_ERR_UNSUPPORTED` in `marmot_commit_process_inbound()` right after `load_current()` when `cur.profile != MARMOT_GROUP_PROFILE_LEGACY`, and do the same in `inbound_removed()` and the deferred replay.
- Alternatively, make `removal_key()`/`removal_contested()` refuse adopted groups, or judge them by the 0x8003 admin set (`marmot_adopted_leaf_is_admin()` on the parent).
- Outside the legacy profile, a missing GroupData must never mean "everyone is admin".
- Add the probe as a regression test: a non-admin Remove gives `MARMOT_ERR_UNSUPPORTED`, the group stays ACTIVE and the state is kept. Add an admin Remove with whatever behaviour you choose, and pin `MARMOT_ERR_UNSUPPORTED` in `test_adopted_commits_refused`.
- If admin removals are refused too, say in the README that a removed libmarmot member is not told; it stays active but stuck.

### L1 (Low) — The profile classifier treats a recognizable but malformed `app_data_dictionary` as legacy, which weakens the pre-0.12 gate

`libmarmot/src/mls/mls_app_components.c:332-339`. The claim is made at `mls_app_components.h:95-102` and README.md:305 ("exactly what libmarmot refused before 0.12.0").

`mls_group_context_profile_of()` reads an entry's length before it looks at the type, so a truncated or non-minimal 0x0006 entry ends the scan as LEGACY. The old gate, `mls_group_extensions_supported()`, refused any recognizable 0x0006 type.

Reproduced on a legacy GroupContext followed by either `00 06 05 00 00` (truncated) or `00 06 40 01 00` (non-minimal):
- the old gate returns −1;
- the new classifier says LEGACY;
- `mls_group_create_with_leaf_extensions()` returns 0 (LEGACY);
- serialize and deserialize both succeed.

The fuzzer hit about 4M such inputs. A Welcome does not reach this today, because `legacy_group_from_mls()` → `marmot_extensions_find()` refuses the malformed list. That's why this is Low. But the MLS-layer create and load gates now fail open for this shape, contrary to the docs.

**Fix.** Return ADOPTED as soon as a 0x0006 type is read, before its length, so the strict parse then fails with `MARMOT_ERR_EXTENSION_FORMAT`. Add both inputs to `test_group_context_negatives`.

### L2 (Low) — Four checks no test exercises (revert spot-checks)

Removing any of these leaves all 23 marmot suites green:

| Check | Where |
| --- | --- |
| Welcome: every leaf signature (RFC 9420 §12.4.3.1). This is the only check that binds a member leaf's HPKE key, capabilities and dictionary to the signature key its 0x8009 proof authorizes. | `mls/mls_welcome.c:790-796` |
| Local Commit producers install through `group_install_checked()` | `mls/mls_group.c:1087-1098` |
| The Commit processor's entered-epoch profile check | `mls/mls_group.c:3816` |
| Creation runs `marmot_adopted_members_proven()` before publishing. Only a wrong enrolled proof would trip it, and nothing tests that. | `groups.c:964` |

The other 16 are caught:
- proofs, admin inviter and seal author;
- load re-validation, `marmot_commit_authorize()` refusal and admins-are-members;
- canonical order, mixed 0xf2ee and 0x8009 in the GroupContext;
- disbanded lifecycle, rumor tags, the KeyPackage tag fix and process-time opening.

**Failure scenario.** A refactor drops the leaf-signature loop. A GroupInfo signer can then serve a member's KeyPackage leaf whose encryption key or dictionary it changed after signing, and nothing fails.

**Fix.**
- Add an OpenMLS negative fixture `TamperedLeaf`: a member leaf altered after signing, rebuilt into the tree under the creator's GroupInfo signature.
- Test creation with an enrolled proof over another signature key.
- Add direct tests of the two MLS-layer guards.

### L3 (Low) — Opening adopted Welcomes on arrival turns a transient storage error into a permanent failure

`welcome.c:697-703` and `:752-756` map any `mls_load` failure of `kp_priv` to `MARMOT_ERR_KEY_NOT_FOUND`. `:610-627` then records a process-time failure as final.

**Failure scenario.** The store is busy or locked when a gift wrap arrives. The adopted Welcome is recorded as FAILED with "matching KeyPackage private key not found". Every redelivery returns `MARMOT_ERR_WELCOME_PREVIOUSLY_FAILED`, so the user never sees the invitation. Before this change, the lookup ran only on a user-initiated accept.

**Fix.** Separate `MARMOT_ERR_STORAGE_NOT_FOUND` from other storage errors, and record a final failure only for definitive outcomes.

### L4 (Low) — Merge integration: other slices' new Commit producers bypass `group_install_checked()`, and the v4 state byte is the public enum value

`mls/mls_group.c:1087-1098` and `:4798`, `:4963-4968`.
- Slice B's `mls_group_commit_by_ref()` and slice A's new `mls_group_replace_members()` (fc6c896b) both install with plain `group_install_staged()`. After merging they skip E's MLS-layer profile check. L2 shows no test would notice.
- Format 4 writes `(uint8_t)group->profile`, so the on-disk byte is whatever the public enum's numbering is. A and E now agree (LEGACY=0, ADOPTED=1). A had UNKNOWN=0, LEGACY=1, ADOPTED=2 at 74d9067f.

**Fix.**
- Switch both producers to `group_install_checked()` when merging.
- Persist a private serial constant mapped explicitly, rather than the enum value.

### L5 (Low, pre-existing, out of scope) — LeafNode capability vectors allocate from an unchecked varint

`mls/mls_tree.c:642-660` (`READ_U16_VEC`, since `0e114e1a`). It calls `malloc(count * 2)` before checking that many bytes remain. The fuzzer hit this within seconds (ASAN allocation-size-too-big in `mls_leaf_node_deserialize`). It's reachable from any kind:30443 event, GroupInfo tree or Commit. In production an absurd size returns NULL (→ −1), but a 4-byte varint still forces a transient ~1 GiB allocation per parse. Suggested bead: check `_bytes <= mls_tls_reader_remaining(reader)` first.

### Nits

- **N1 — the relay URL validator diverges from MDK's `url::Url`** (`mls_app_components.c:135-185`).
  - It accepts `wss://[zzz]`, `wss://ex<ample.com` and `wss://exa%zzmple.com`. MDK refuses these, so `marmot_create_group_for_profile()` can create a group MDK will not join.
  - It refuses `wss://relay.example.com/ path`, which MDK accepts; that direction fails closed.
  - Validate IPv6 literals and forbidden host code points, at least on the create path.
- **N2** — an adopted kind:444 rumor that carries `sig` is not refused. transports/nostr.md says it "MUST NOT have a `sig` field" (`welcome.c:166-191`).
- **N3** — README.md:370-372 has the `### 0.11.0 (unreleased)` heading twice.
- **N4** — constants are duplicated under two names: `MLS_COMPONENT_ACCOUNT_PROOF_V2`/`MARMOT_COMPONENT_ACCOUNT_PROOF_V2`, `MLS_COMPONENT_SAFE_AAD`/`MARMOT_COMPONENT_SAFE_AAD`, `MLS_COMPONENT_APP_COMPONENTS`/`MARMOT_COMPONENT_APP_COMPONENTS`.
- **N5** — `marmot_adopted_members_proven()` verifies under the hard-coded `MARMOT_CIPHERSUITE` (`adopted.c:21`), not the group's suite. That's correct today, since one suite is enforced at Welcome and create, but brittle.

## Focus answers

1. **Proof verification.**
   - The template is exactly the spec's: kind 450; tags `d`, `component` `0x8009`, `ciphersuite`, `signature_scheme` (`0x%04x`, lowercase) and `mls_signature_key` (lowercase hex of the 32-byte leaf key) in that order; the exact content; `pubkey` is the credential identity (`kp_profile.c:214-257`).
   - `created_at` must be in 1..2^53−1. There is no age limit, per authorization-proofs.md:104-107 and MDK's `validate_current_timestamp()`.
   - The BIP-340 check is `nostr_event_validate()`: it recomputes the NIP-01 id, compares it, and verifies under a parsed x-only key. The spec vector (id `b7e9a15d…`, signature `c5315d3c…`) is pinned in `test_kp_profile.c:28-79`, with negatives for key, suite, scheme, `created_at`, signer and length.
   - The binding: `signer_pubkey` equals `BasicCredential.identity` (`kp_profile.c:426`, `mls_app_components.c:641-643`), and the signed key is the leaf's own 32-byte `signature_key` (the deserializer enforces 32).
   - Replay onto another leaf is impossible without that leaf's signature key, and the Welcome leaf-signature check binds the rest of the leaf.
   - Replay onto another group is allowed by the spec ("MAY be reused … does not expand its authority"). It needs the member's whole signed leaf, which is joining.md's documented "Welcome-bootstrap trust" limit, not a proof weakness.
2. **Welcome admission.**
   - Order: HPKE → GroupInfo AEAD → GroupContext admission → tree **from the GroupInfo `ratchet_tree` only** → tree hash and parent hashes → GroupInfo signature (the signer must be a member leaf) → adopted structure and capabilities, admins-are-members → every leaf signature → every BIP-340 proof → inviter (GroupInfo signer) in 0x8003 of the joined state → seal author equals the inviter.
   - A forwarded or re-wrapped Welcome is refused with `MARMOT_ERR_AUTHOR_MISMATCH` (tested).
   - Through `marmot_process_welcome()`, which has no seal author, the sender is the rumor pubkey. That's a caller contract documented since 0.10.0 (`marmot.h:897-919`); marmot-gobject uses `_from` when it has the seal author.
   - Adopted Welcomes are opened on arrival (L3), and nothing is stored on refusal (tested). I found no admission bypass. H1 happens after admission.
3. **Profile and persistence.**
   - Legacy states are still written as v3, byte for byte (tested). v4 is v3 plus the profile byte.
   - Every load and clone re-runs the structural profile check; every Commit stage is a serialize/deserialize round trip. v4→v3, v4 claiming legacy, and a GroupContext/profile mismatch are all refused (tested).
   - The format is not integrity-protected: there is no MAC, and proofs are not re-verified on load. Downgrading needs a rewritten GroupContext, which is equivalent to write access to the store. Groundhog's store is SQLCipher.
   - There is no cross-profile fallback on a crypto or auth failure. The only parse-level corner is L1, and the numbering coupling is L4.
4. **Parsing safety.**
   - Varints must be minimal; lengths must be exact with no trailing bytes.
   - Dictionaries and component lists must be strictly ascending and unique; id arrays are bounded.
   - Names are length-bounded UTF-8; admin keys are sorted and unique; relay URLs are 1-16, sorted and unique.
   - The fuzzer found nothing in the new parsers (see Gates). It did hit the pre-existing L5.
5. **Refusal path.**
   - Our own Commits are refused before staging (self-update and rename were tested; nothing is left pending).
   - Inbound Commits are processed only on clones, and state is unchanged on refusal (tested). The exception is a removal of our leaf (H1).
   - User-visible consequence: a libmarmot member freezes at the group's first Commit. Every later Commit gives `MARMOT_ERR_UNSUPPORTED` and later-epoch messages can't be decrypted. The README says this honestly. Before H1 is fixed, the README also needs a note about removal.
6. **Fixture provenance and reproducibility.**
   - `adopted-groups.json` and `emitter/` are identical to the capture workspace.
   - That workspace's MDK checkout is at `946e0547`, and its `Cargo.lock` differs only by the emitter's own `[[package]]` entry.
   - The OpenMLS pin `erskingardner/openmls@59e7d3b2` matches MDK's `Cargo.toml`/`Cargo.lock`, and rustc 1.97.1 matches `rust-toolchain.toml`.
   - `gen_adopted_fixture.py` regenerates `adopted_fixture.h` byte for byte.
   - A fresh capture with the same emitter build (new random MLS keys) passes `test_adopted` 14/14, so the tests don't depend on captured values.
   - The reverse direction re-ran successfully with this build (see Gates). It is manual and not in CI, as the README says.
   - The test secrets are seed-derived and marked test-only.
7. **Merge risk.** See below.

## Merge risk (slices A, B, G and master)

- **master `d88d77e0`** now includes slice G (media v2), which declares libmarmot 0.12.0 (unreleased, SOVERSION 0.12).
  - Conflicts: `VERSION_MANIFEST.md`, `libmarmot/README.md`, `libmarmot/include/marmot/marmot.h` and `libmarmot/CMakeLists.txt`. The last two are include and public-header lists: keep both `marmot-group-profile.h` and `marmot-media.h`.
  - Fold E into the unreleased 0.12.0 entry rather than bumping again.
  - There are no macro clashes with G. G's 0x8002, 0x8007 and 0x800b stay outside `MLS_ADOPTED_SUPPORTED_COMPONENTS` until they are wired; groups requiring them are refused, as expected.
- **Slice A.**
  - At `74d9067f`, A had its own `MarmotGroupProfile` in `marmot/marmot-profile.h` (UNKNOWN=0, LEGACY=1, ADOPTED=2) and its own `marmot_get_group_profile()`. That would have meant redeclared enumerators and a duplicate symbol.
  - A's `fc6c896b` (pushed during this review) now ships a byte-identical copy of E's `marmot-group-profile.h` and drops its enum and function, so that blocker is resolved. E's header comment ("shared with W24 slice A") is now true.
  - Remaining conflicts: `commits.c`, where both slices added a non-legacy refusal at the top of `marmot_commit_authorize()` (keep both); `mls_group.c`, where A's new `add_members_staged()` signature and `mls_group_replace_members()` meet E's `group_install_checked()` (keep the checked install in both producers, L4); README; and VERSION_MANIFEST.
  - H1 is not fixed by A.
- **Slice B** (`00eb9d49`; `6df721dd` by the end of the review).
  - The only conflict is the README. The merged tree builds and all 23 marmot suites pass.
  - E's refusal sits at the top of B's `marmot_commit_authorize_ex()`, and every B Commit path (`stage_pending_ex`, `pending_apply`, `stage_inbound`) goes through it.
  - B's `mls_group_commit_by_ref()` installs unchecked (L4), and H1 reproduces on the merge.
  - After B, `MLS_ADOPTED_EXTRA_PROPOSAL_COUNT` (SelfRemove 0x000a) is the hook to update.

## Required before merge

- **H1**, with the regression test. Clarify the README for removed members.
- Strongly recommended in this slice: **L1** (a one-line classifier fix plus two negatives) and **L2** (a `TamperedLeaf` negative fixture; pin `MARMOT_ERR_UNSUPPORTED` in `test_adopted_commits_refused`).
- When merging A and B: **L4** (checked install for their new producers).
- Otherwise, file as beads: L3, L5 (pre-existing), N1–N5.
