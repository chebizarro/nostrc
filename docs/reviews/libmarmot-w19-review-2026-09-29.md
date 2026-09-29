# libmarmot W19 review: kind:445 authentication, storage transactions, persisted ratchets

- **Reviewer:** independent peer reviewer (AGENTS.md "Pre-Push Requirements → Peer Review")
- **Branch reviewed:** `marmot/w19-ratchet-persist` at `9a421fe1`. The four commits sit on `c0f8e059`, which differs from origin/master only in beads files.
- **Review branch:** `marmot/w19-review`
- **Date:** 2026-09-29
- **Verdict (initial, at `9a421fe1`):** **REQUEST CHANGES**. Blocking finding: **B1**. **C1** is a critical pre-existing issue that must be filed as P0 and fixed before any release, but it is not a merge blocker for this branch.
- **Verdict (final, at `6cfa86d5`):** **APPROVED**. B1 and C1 are resolved; see "Final pass" at the end.

**Commits**

| Commit | Bead | Change |
|---|---|---|
| `35503c9c` | nostrc-6r6s | kind:445 id and signature verification; rumor path; libmarmot 0.6.0, marmot-gobject 1.3.0 |
| `5661f8d1` | nostrc-qp24.7 | storage transaction hooks; late messages; gh-mls-commits; libmarmot 0.7.0 |
| `a7be92db`, `9a421fe1` | nostrc-ai04, P0 | persisted sender ratchets, state format 3, crash tests; libmarmot 0.8.0 |

**Checked against:** RFC 9420 §6.3, §9, §9.1, §9.2 and §15; Marmot `transports/nostr.md` and `foundation/application-messages.md` at `26fa6a6`; legacy MIP-03 at `cc73aa8`.

No code or beads were changed. Repros ran in a throwaway worktree (`/tmp/rr19a`), which was removed afterwards.

## Summary

**The core nostrc-ai04 fix is correct and well tested.**
- Each send now uses the next generation of a persisted sender ratchet, stored in the operation's transaction before the event is returned.
- A generation decrypts once, including through the retained parent.
- The secret tree follows the RFC 9420 §9.2 deletion schedule in memory and in format 3.
- The skipped-key window is bounded.

**Signature verification (6r6s) runs before any storage access or decryption.** The transaction hooks (qp24.7) cover every public writing operation. GhStoreMarmot savepoints nest correctly.

**All test runs pass.**
- `ctest -R 'marmot|mls|gnostr|groundhog-store'`: 87 run, 86 passed, 1 skipped (keyring).
- ASAN+UBSAN, `leaks`, and the Linux container (arm64, 29/29) are clean.

**Two security problems surfaced, both verified.**
- **B1 (blocking): documentation.** The format docs claim a stolen state cannot re-derive a consumed message key. It can: re-processing the relay-visible Commit against the retained parent regenerates the current epoch from generation 0. The advisory also claims "the sender's signature inside the MLS content still authenticates the sender". Application messages carry no such signature.
- **C1 (critical, pre-existing, untracked): impersonation.** libmarmot never checks that the inner event's `pubkey` equals the MLS sender leaf's identity. Both Marmot specs require this check. Any member can post as any other member through the public API.

## Findings

### B1 (High, blocking): the 0.8.0 security claims are wrong on two points

**Where**
- `libmarmot/README.md` 0.8.0 advisory: "The sender's signature inside the MLS content still authenticates the sender"; and "Deletion … The stored state holds only unconsumed values … Tests check that no used key … appears".
- The format comment in `mls_group.c` (`mls_group_serialize`): "a stolen state cannot re-derive a used message key".
- `mls_key_schedule.h` (`mls_secret_tree_serialize`: "cannot be derived from what is stored").
- The ai04 commit message.

**Claim 1: a stolen state cannot re-derive consumed keys. False while a retained parent exists, which is after every Commit.**

`mls_group_parent` (W17) stores the previous epoch's full state:
- its `init_secret`;
- the leaf and path private keys that decrypt the Commit's UpdatePath;
- its exporter secret, which opens the Commit's kind:445. The exporter-secret table holds it too.

The Commit itself is on relays. Re-processing it against the stored parent regenerates the current epoch from scratch, generation 0 of every sender.

**Verified (throwaway test, not committed):**

| Step | Actor | Result |
|---|---|---|
| 1 | Bob | reads Alice's first message of epoch 3; his live state then refuses it (`OWN_MESSAGE`) |
| 2 | Attacker, with only Bob's `mls_group_parent` record and the public Commit | `mls_group_deserialize` → epoch 2 |
| 3 | Attacker | `mls_group_process_commit` → rc 0, epoch 3 |
| 4 | Attacker | `mls_group_decrypt` on the consumed message → rc 0, **plaintext recovered** |

Forward secrecy against storage compromise therefore covers only messages older than the previous epoch. The byte-scan test ("no used key in the stored bytes") is true but does not test derivability.

This is inherent to keeping a parent that can re-process a competing Commit (Marmot convergence retention), so it is a documented trade-off, not something this branch must remove. But the advisory tells users the opposite.

**Claim 2: the sender's signature still authenticates the sender. False.**
- `mls_group_encrypt()` puts the raw plaintext into the `PrivateMessage`, with no `FramedContent` or `FramedContentAuthData` signature (RFC 9420 §6.3).
- The receiver takes the author from the inner event (C1).
- So the advisory's reassurance about the 0.7.0 nonce-collision forgery ("the sender's signature … still authenticates the sender") does not hold: a forger under a repeated key/nonce could also impersonate.

**Required**
- Correct the README advisory and the format and serializer comments.
- State that a stored state (with its retained parent) plus the public Commit exposes every message of the current epoch, and the parent epoch's unconsumed ones, until the next epoch transition replaces the parent.
- Remove the signature claim.
- File a follow-up to bound the exposure, for example by retiring the retained parent once the epoch is settled.

### C1 (Critical, pre-existing, untracked; file P0 now): members can impersonate each other

**Where**
- `process_group_event` (`messages.c`) sets `result->app_msg.sender_pubkey_hex` and the stored message's `pubkey` from the inner event's `pubkey` field, with no comparison to the MLS sender leaf's credential identity.
- `mls_group_decrypt` returns the sender leaf index but nothing checks it against the author.
- There is also no MLS content signature (B1).

**Spec**
- Adopted `foundation/application-messages.md` lines 70–73: a receiver MUST compare the inner `pubkey` with "the Marmot account identity authenticated by the MLS sender leaf… If … not equal, the receiver MUST drop the Marmot app payload".
- Legacy MIP-03 lines 244–245: "Clients MUST verify the MLS sender matches the inner event's pubkey".
- RFC 9420 §6.3 requires `PrivateMessageContent.auth` (`FramedContentAuthData`).

**Verified (throwaway test):** Bob calls `marmot_create_message` with an inner event whose `pubkey` is Alice's. Charlie's `marmot_process_message` returns `MARMOT_OK` with `sender_pubkey_hex` equal to **Alice's** key.

**Relation to this branch.** It is neither introduced nor made worse here, so it does not block this P0 fix from merging. But no release should ship without it. Beads has nothing on it (searched "impersonat", "inner pubkey", "sender leaf", "FramedContentAuthData").

**Fix**
- Drop an app payload whose inner `pubkey` is not the sender leaf's 32-byte identity.
- Sign `PrivateMessageContent` per RFC 9420 §6.3, and verify the signature with the sender leaf's key before accepting.

### Non-blocking

**N1 (Medium): nested transactions weaken the sender guarantee.** The hook contract says `commit` makes writes "durable", but also allows the backend to run as a savepoint inside an application transaction. GhStoreMarmot does exactly that. When nested, `marmot_create_message()`'s guarantee ("stored before the event is returned") holds only once the *outer* transaction commits. An app that publishes the event and then rolls back, or crashes before committing, the enclosing transaction rewinds the ratchet, and the next send reuses a generation.
- gh-mls-commits nests only Commit operations and publishes after its outer commit, which is correct.
- Nothing in Groundhog calls `marmot_create_message` today.
- Document on `marmot_create_message` and in the hook contract that an enclosing transaction must commit before the event is published (the same applies to nostrc-ow0c-style rewinds).

**N2 (Low): the ratchet restore after an AEAD failure is untested.** `mls_private_message_decrypt_with_sender_data` restores the sender's ratchet when the AEAD fails. Replacing that restore with a plain discard passes every test (`test_commits`, `test_ratchet_persist`, `test_protocol`, interop). Add a case that keeps the sender data intact but breaks the content tag, then checks the genuine message still decrypts.

**N3 (Low): migration window.** Judged acceptable, with a note.
- **The stride is sound.** Formats 1 and 2 restarted the tree on every load, and every public-API send loads per call, so no stored state ever sent above generation 0. 512 is within the 1000 that libmarmot, OpenMLS and MDK accept.
- **What replays.** A migrated receiver restarts every other sender at generation 0. One pre-upgrade message per sender per epoch can therefore be replayed once in a new envelope.
- **Impact.** A duplicate delivery of a genuine old message: no confidentiality or key-reuse issue. It is limited to the epoch current at upgrade, which the README's "move to a new epoch" advice closes.
- **Recommendations:**
  - De-duplicate by inner event id as defence in depth; messages are keyed only by the outer id today.
  - Say in the README that a 0.8.0 receiver drops every message after the first per epoch from a ≤ 0.7.0 sender (all at generation 0).

**N4 (Info): skipped-key cache.**
- Bounded to 32 keys per chain (window `[g−32, g)`, proved by construction and checked on load).
- A forward jump costs at most 1000 ratchet steps; the chain never wraps.
- Outsiders cannot reach it: it needs the exporter secret and the sender-data secret.
- An insider can derive every sender's keys from the shared secret tree, and without C1's check can forge a message for another sender's leaf. That lets it move that sender's chain up to 1000 forward, dropping that sender's in-flight messages outside the window. This is subsumed by C1.

**N5 (Info): nostrc-ow0c remains latent.** No code in libmarmot, marmot-gobject, Gnostr or Groundhog calls `rollback_snapshot`, and 0.8.0 documents the hazard on the hook. P2 is appropriate.

**N6 (Info): Groundhog under ASAN needs a flag.** With `-DSANITIZE=address,undefined`, Groundhog's `-Werror` fails on a `_FORTIFY_SOURCE` redefinition in both Debug and RelWithDebInfo. The run needed `-DCMAKE_C_FLAGS=-Wno-macro-redefined`. This is a build-configuration issue, not a code defect.

## Answers to the review questions

**Stored state: can a consumed key be re-derived?**
- **From the live format-3 state:** no.
  - Internal node secrets are zeroed once their children are derived (`populate_tree`).
  - A leaf secret is zeroed when its ratchets start (`init_sender_ratchet`).
  - Each step overwrites the ratchet secret (`ratchet_step`).
  - Taking or pruning a cached key wipes it.
  - `encryption_secret`, `joiner_secret` and `welcome_secret` are neither stored nor kept.
- **From the whole store:** yes, via the retained parent plus the public Commit (B1).

**Sender crash.** No key reuse is possible on a top-level transaction.
- `create_message_impl` encrypts, stores the advanced state (failing closed), and only then signs and returns, all inside the operation's transaction.
- A crash before commit returns no event; a crash after commit only leaves an unused generation.
- Without hooks, the direct write happens before return.
- The Groundhog crash suite pins this at every write, and before and after commit.
- The nested case is N1.

**Migration.** The 512 stride is sound. The once-per-epoch replay window is acceptable with documentation (N3).

**Skipped-key cache.** Bounded, evicts deterministically, and is DoS-resistant against outsiders (N4).

**Signature verification.**
- `parse_group_event` → `authenticate_group_event` runs after the kind and `h` checks and before any storage read or trial decryption.
- A rejected event changes nothing, and the canonical id keys the processed markers.
- The rumor path requires a declared id to be canonical.
- Everything compared is public (id hash, BIP-340 via libsecp256k1), so constant-time comparison is not needed.

**Transaction hooks**
- Every public writing function begins a transaction: create group, add, remove, leave, metadata, merge, clear, pending and outbox get/mark, process/accept/decline Welcome, KeyPackage creation (common helper), encrypt media, create/save/process message and rumor. The rest only read.
- `marmot_txn_begin` collapses nested calls with a depth counter.
- The author's shim test aborts on any write outside a transaction or any nested begin. Unwrapping `marmot_create_message` fails `test_commits`.
- GhStore uses SAVEPOINT at depth > 0, RELEASE on inner commit, and ROLLBACK TO plus RELEASE on inner rollback. gh-mls-commits publishes only after its outer commit.
- On nostrc-ow0c and nested durability, see N5 and N1.

**Do the tests fail without each fix?**

| Change | Result |
|---|---|
| No ratchet save on send | `test_commits` fails |
| Signature check skipped | `test_commits` and `test_protocol` fail |
| Legacy stride 0 | `test_ratchet_persist` fails |
| Skipped-window pruning removed | `test_ratchet_persist` fails |
| Late-path parent ratchet not stored | `test_commits` fails |
| `marmot_create_message` not in a transaction | `test_commits` fails |
| **Ratchet restore after an AEAD failure removed** | **survives (N2)** |

**Interop.** MDK tree-validation, treekem, passive-client, welcome and message vectors, and the RFC 9420 vectors, all pass on macOS and in the Linux container.

## Verification

- **Build.** `cmake -S . -B … -G Ninja -DBUILD_GROUNDHOG=ON && ninja` ok. The build rewrote a tracked blueprint `.ui` file; I restored it before committing.
- **Tests.** `ctest -R 'marmot|mls|gnostr|groundhog-store' -j6`: **87 run; 86 passed, 1 skipped** (`groundhog-store-key-keyring`, no keyring).
- **ASAN+UBSAN (runtime confirmed):**
  - all 23 `marmot_test_*` / `marmot_gobject_test` pass with no reports;
  - `groundhog-store-marmot` passes (RelWithDebInfo; see N6).
- **`leaks --atExit`:**
  - `test_commits`, `test_ratchet_persist`, `test_protocol`, `test_mls_group` and `test_marmot_gobject` each report **0 leaks**;
  - `test-groundhog-store-marmot` got through 18 cases without a report, then cannot run its fork-based crash harness under `leaks`.
- **Linux container** (`scripts/groundhog-linux-ci.sh`, Ubuntu 24.04, arm64): `-R 'marmot|mls|groundhog-store'` **29/29 passed**.
- **Repros (throwaway, not committed):**
  - the retained-parent re-derivation (B1);
  - the inner-pubkey impersonation (C1);
  - 7 mutations: 6 caught, 1 survivor (N2).

## Recommendation

**REQUEST CHANGES** for **B1**: correct the 0.8.0 advisory and the format and serializer comments (retained-parent exposure; no MLS content signature), and file the follow-up to bound the exposure.

**Before any release:** file **C1** as P0 and fix it (inner `pubkey` must equal the sender leaf identity; ideally §6.3 content signatures). It does not block merging this P0 fix, which is otherwise sound.

**Can follow:** N1–N3.

---

## Final pass (2026-09-29): `8ff2667b`, `6cfa86d5`

**Scope.** Two new commits on `marmot/w19-ratchet-persist`:
- `8ff2667b`: B1 docs, N1 docs.
- `6cfa86d5`, nostrc-we6g (C1): RFC 9420 §6.3.1 signed PrivateMessageContent, §6.3.2 SenderDataAAD, inner-author binding, handshake PrivateMessages refused, inner-id dedupe, N2 test, libmarmot 0.9.0.

Scratch worktrees: `/tmp/rr19b` (tip), with an ASAN build dir. No code or beads changed.

**Verdict: APPROVED.** B1 is resolved and C1 is closed. What remains is non-blocking (F1–F5).

### C1: closed

**The signature is exactly RFC 9420's.** `mls_application_content_encode`/`_decode` build a FramedContent view:
- `group_id`, `epoch`;
- `sender = member(leaf)`;
- the PrivateMessage's `authenticated_data`;
- `content_type = application`;
- `application_data<V>`.

They sign or verify it with the existing `mls_framed_content_sign`/`_verify`:
- **FramedContentTBS:** `version = mls10`, `wire_format = mls_private_message`, the FramedContent, and, for a member sender, the serialized GroupContext.
- **Wrapper:** `SignWithLabel(…, "FramedContentTBS", …)`, i.e. SignContent = `opaque label<V> = "MLS 1.0 FramedContentTBS"`, `opaque content<V>`.
- **Encoding:** every `<V>` is a QUIC varint (`mls_tls_write_opaque*` → `write_vli`).
- **PrivateMessageContent:** `application_data<V>`, `signature<V>` (exactly 64 bytes), then zero padding. Receivers reject non-zero padding; senders send none.
- **Keys and context:**
  - The sender signs with `own_signature_key` over its GroupContext.
  - The receiver verifies with `tree.nodes[leaf].leaf.signature_key` of the sender-data leaf, over its own GroupContext. The epoch is already checked equal, so the GroupContexts match.
  - The late path runs `mls_group_decrypt` on the retained parent, so it uses epoch-N's tree, GroupContext and key.

**Interop.** The MDK/RFC `message-protection` vector now opens `application_priv` end to end under ctest ("application_priv unprotected and its signature verified"):
- the SenderDataAAD decrypts;
- the signature verifies with `signature_pub` over the vector's GroupContext;
- a flipped key fails;
- our encoder reproduces the vector's PrivateMessageContent byte for byte (Ed25519 is deterministic).

Run bare from the build dir, `test_marmot_interop` loads no vectors; ctest runs it from the source dir, where it does.

**Ratchet safety.**
- `mls_group_decrypt` validates the sender data before consuming anything: leaf in range, `content_type == application` (else `UNSUPPORTED`), leaf node present, not our own leaf.
- It snapshots the sender's ratchet (leaf secret, ratchet struct with its skipped keys, init flag).
- It restores the snapshot on an AEAD or signature failure.

**Repros.** My own adversarial variants were throwaway, added to `test_commits` in `/tmp/rr19b` and since removed. Each rejection is checked with `expect_rejected`, which also asserts that the stored state is byte-for-byte unchanged.

| # | Attack | Result |
|---|---|---|
| — | Original repro: Bob `marmot_create_message` with Alice's pubkey | `MARMOT_ERR_AUTHOR_MISMATCH` at the sender, no event (the author's `test_member_cannot_post_as_another` also covers a modified client, on both the relay and rumor paths, live and late) |
| V1 | **Sender data from another member:** Bob encrypts under Charlie's leaf (keys derivable from the shared secret tree), inner pubkey Charlie's, signed with Bob's key | Alice: `MARMOT_ERR_MLS` (signature), state unchanged (relay and rumor path). Charlie: `OWN_MESSAGE`, state unchanged |
| V2 | **Signature key swapped mid-epoch:** Bob's own leaf and pubkey, fresh Ed25519 key not in the tree | Alice and Charlie: `MARMOT_ERR_MLS`, unchanged. A leaf's key changes only through a Commit (new epoch, new tree); `marmot_commit_authorize` pins the committer's identity, and the UpdatePath leaf keeps the credential |
| V3 | **Forged leaf index, pre-emption:** Bob forges Alice's leaf at the generation Alice sends next | Charlie rejects it (`MARMOT_ERR_MLS`, unchanged). Alice's genuine message at that **same leaf and generation** (checked through the sender data) then reads, with `sender_pubkey_hex` Alice's. Restore works; N4's pre-emption DoS is gone |
| V4 | **Handshake content type in a PrivateMessage** from Bob's real leaf | `MARMOT_ERR_MLS` (`UNSUPPORTED` inside), unchanged |
| V5 | **Key and leaf changed by a Commit:** Alice's epoch-E message read after her path Commit replaced her leaf | Accepted as Alice, via the parent tree. V1's epoch-E forgery read late by Alice: rejected, unchanged |

**Negative control.** With `mls_framed_content_verify`'s result ignored, V1 is accepted (as Charlie), and so is the author's "Alice's leaf" case. The signature carries the load, and the tests see it.

**Author binding.** The inner pubkey must equal the sender leaf's 32-byte credential identity from the state that decrypted the message (`check_inner_author`):
- A mismatch, a missing pubkey or non-event JSON is rejected.
- A late step is undone with `state_undo_apply`.
- `sender_pubkey_hex` is taken from the inner event only after that check, so it is authenticated.
- The identity rests on:
  - the adder's KeyPackage check (event pubkey, or ACCOUNT_PROOF_V2);
  - `marmot_commit_authorize` (a committer cannot change its identity);
  - the MLS Update and UpdatePath pinning (see F2).

### B1: wording now accurate

The README "What 0.8.0 does not protect" section is correct:
- the retained parent holds the parent's init secret and UpdatePath private keys;
- with the relay-visible Commit, the current epoch re-derives: every current-epoch message, including already-read ones, plus the parent's unconsumed keys;
- this lasts until the next transition;
- nostrc-yuj2 tracks it.

The other corrections:
- The Deletion bullet is limited to the live `mls_group`, and the byte-scan sentence now says it covers the live state only.
- The false signature claim is replaced by an accurate statement that pre-0.9.0 nothing authenticated the author.
- The format and serializer comments match (checked in the diff).

One nit (F4): "Forward secrecy … covers only messages older than the previous epoch" is conservative. The previous epoch's *consumed* messages are also protected, because the parent holds only its unconsumed values. It understates rather than overclaims, so it is fine.

**N1** is documented in the `marmot_create_message()` header, the hook contract and the Groundhog headers. **N2** now has a test: replacing the AEAD-failure restore with a discard fails `test_ratchet_persist` ("tampered generation 5: the rejection changed the state"). **N3**'s dedupe is implemented, and the ≤ 0.7.0 generation-0 note is in the README.

### Regressions checked

- **Interop.** All MDK and RFC vectors still pass. The SenderDataAAD change fixes a latent non-interop: the empty AAD, which OpenMLS and MDK reject.
- **Migration notes.** Accurate.
  - 0.9.0 ↔ ≤ 0.8.0 cannot read each other's application messages (unsigned content, empty sender-data AAD). Commits, Welcomes and KeyPackages are unchanged; state stays format 3.
  - The MINOR bump is right for 0.x.
  - "No public API change" holds: `MARMOT_ERR_AUTHOR_MISMATCH` (-34) already existed.
  - Gnostr's router already puts the user's pubkey in the rumor, so it is unaffected.
- **Dedupe semantics.**
  - A duplicate (same inner NIP-01 id in another envelope) keeps its ratchet step, marks the outer envelope, and returns `MARMOT_RESULT_OWN_MESSAGE`. A replay of that envelope then fails on the used generation (tested).
  - A foreign author cannot collide with another member's inner id, because the id covers the pubkey, which is now bound.
  - Namespace: see F1.

### Non-blocking follow-ups

**F1 (Low): inner-id dedupe is global, not per group.** `is_message_processed(ctx, id)` takes no group id in any backend (memory, SQLite, nostrdb, GhStoreMarmot).
- A member of two groups who receives the *same* rumor in both gets it only in the first; the second returns `OWN_MESSAGE`.
- Gnostr's rumors (second-granularity `created_at`, empty tags, no group tag) make that reachable: the same text sent to two groups within a second. So is the same text sent twice within a second in one group; the sender shows two, receivers one. The README documents the single-group case.
- Suggest scoping the dedupe key to (group, inner id), for example by hashing the group id into the stored key.

**F2 (Medium, pre-existing, outside we6g): the leaf identity rests on the committer's KeyPackage check.** ACCOUNT_PROOF_V2 is verified only where the adder parses a KeyPackage (`validate_leaf_adopted`, `credentials.c:1213`). Receivers of a Commit's Add, and joiners reading a Welcome's tree, do not re-verify it.
- A malicious or compromised admin can therefore add a leaf whose credential names another account and post as that account; C1's fix then faithfully reports it.
- Admins are already trusted with membership, so this does not block. But RFC 9420 §5.3.1 expects every member to validate new credentials.
- Suggest verifying the proof on received Adds and on Welcome trees whenever the leaf carries it. File a bead.

**F3 (Info): result labels.**
- A duplicate is reported as `MARMOT_RESULT_OWN_MESSAGE`. A future `DUPLICATE` result would be clearer (an API change, so defer).
- Sender data naming the receiver's own leaf also returns `OWN_MESSAGE` before any signature check. This is harmless: nothing is consumed or stored (V1).

**F4 (Nit):** the conservative forward-secrecy sentence above.

**F5 (Nit): legacy raw mode is unauthenticated.** With `MarmotConfig.allow_legacy_raw_messages`, the raw-JSON path has no MLS signature and no author binding, yet `marmot_process_message`'s doc says `sender_pubkey_hex` "is therefore the authenticated author". Qualify that sentence for legacy mode. The mode is off by default.

### Verification (tip `6cfa86d5`)

- **Build.** `cmake -S . -B /tmp/rr19b-build -G Ninja -DBUILD_GROUNDHOG=ON && ninja` ok. The blueprint `.ui` rewrite was restored.
- **Tests.** `ctest -R 'marmot|mls|gnostr|groundhog-store' -j6`: **87 run; 86 passed, 1 skipped** (`groundhog-store-key-keyring`, no keyring).
- **ASAN+UBSAN** (RelWithDebInfo, `-fsanitize=address,undefined -fno-sanitize-recover=undefined -Wno-macro-redefined`, see N6): `ctest -R 'marmot|mls|groundhog-store-marmot'` **26/26 passed**, with no ASAN or UBSAN reports. This covers all `marmot_test_*` (including the RFC 9420 and MDK vectors), `marmot_gobject_test`, the gnostr MLS plugin tests, `groundhog-store-marmot`, and my V1–V5 variants.
- **`leaks --atExit`: 0 leaks** in `test_commits` (with V1–V5), `test_ratchet_persist`, `test_mls_framing`, `test_mls_group`, `test_marmot_interop`, `test_protocol`, `test_storage_contract` and `test_marmot_gobject`.
- **Mutations:**
  - signature verification ignored → V1 and the author's leaf-forgery test fail;
  - AEAD-failure restore → discard → `test_ratchet_persist` fails.
  - The author reports six more (author check, sender-side binding, either restore, SenderDataAAD, dedupe).

### Recommendation

**APPROVED** for merge. Before release, consider F1 (dedupe scope) and file F2 (receiver-side account-proof checks) as a bead. F3–F5 can follow.
