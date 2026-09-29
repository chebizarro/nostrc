# libmarmot W20 review: member identity binding and retained-parent retirement

- **Reviewer:** independent peer reviewer (AGENTS.md "Pre-Push Requirements → Peer Review")
- **Branch reviewed:** `marmot/w20-identity-parent` at `ddc9dad1`, on `a15489c9` (origin, libmarmot 0.9.0)
- **Review branch:** `marmot/w20-review`
- **Date:** 2026-09-29
- **Verdict:** **REQUEST CHANGES**. Blocking finding: **B1**. The identity binding itself is sound, and so is the retirement of the retained parent. B1 is about what the new join rule does to groups that already contain unproven leaves.

**Commits**

| Commit | Bead | Change |
|---|---|---|
| `31767a66` | nostrc-7vyi (W19 F2) | `marmot.member.account-identity-proof.v2` (0x8009) on every produced leaf; enrollment API; receiver checks on Commits and Welcome trees; `allow_unproven_members`; marmot-gobject 1.4.0 |
| `ddc9dad1` | nostrc-yuj2 (W19 B1) | the retained parent keeps its full state only while a competing Commit could win, then is stripped to reader-only; record trailer; libmarmot 0.10.0 |

**Checked against**
- Marmot spec at `26fa6a6`:
  - `foundation/identity.md` ("Account identity proof");
  - `app-components/account-identity-proof-v2.md` (the event template, "Production and reuse", "Lifecycle", "Validation");
  - `protocol-core/joining.md`, `protocol-core/retained-history.md` ("Retained cryptographic material"), `protocol-core/convergence.md`;
  - the KeyPackage and admin rules.
- RFC 9420:
  - §7.2 (LeafNode, capabilities, extensions);
  - §7.3 (leaf validation);
  - §12.4.3.1 (joining via Welcome);
  - the app_data_dictionary extension type 0x0006.
- MDK v0.8.0 source (`/tmp/mdk080`, `crates/mdk-core/src/key_packages.rs`).

No code or beads were changed. Repros and mutations ran in a throwaway worktree (`/tmp/rr20a`), which was removed afterwards.

## Summary

**7vyi binds each leaf to its account correctly, where it is checked.**
- `marmot_leaf_proof_status` implements the spec's Validation list: exactly one LeafNode dictionary, 0x8009 advertised in `app_components`, one 0x8009 entry, and a BIP-340 signature by the credential identity over the kind:450 template for *this leaf's* signature key, ciphersuite and scheme.
- A proof lifted from another leaf fails. A proof signed by another account fails.
- Commits must prove every added or re-filled leaf, and may not drop a proof.
- Welcome trees must prove every leaf except our own, and the GroupInfo signer when the signer is the Welcome's sender.
- **The Welcome-sender exception cannot be used for impersonation in Gnostr.** The exempt leaf's identity must equal the rumor `pubkey`, and Gnostr's NIP-59 unwrap requires rumor pubkey == seal pubkey, with a verified seal signature.

**yuj2 is sound.**
- The "could still win" set is exactly libmarmot's `CommitOrderingSuffix` tie-break.
- A witness must be an authenticated application message from the same leaf key at the new epoch, which a member cannot forge since 0.9.0's signatures.
- `mls_group_strip_to_reader()` removes the init secret and every private key that re-derives the next epoch. The W19 B1 attack now fails after retirement.

**Blocking: B1.** The new rule rejects a Welcome whose tree holds an unproven leaf other than the sender's. Pre-0.10 groups are made entirely of such leaves, and so are groups created before enrollment. In default mode, libmarmot still lets a member publish and merge an Add for such a group. The joiner must then reject the Welcome, and the added leaf stays in everyone's tree as a ghost.
- After the upgrade, **no pre-0.10 group with three or more members can admit anyone** (verified).
- A group whose creator was not yet enrolled can admit members only through the creator. Gnostr enrolls asynchronously, and its create-group paths do not wait for it.
- The README states the consequence inaccurately.

**Test runs.**
- `ctest -R 'marmot|mls|gnostr|groundhog-store'`: 87 run; 86 passed, 1 skipped (keyring).
- ASAN+UBSAN: 26/26, with no reports.
- `leaks`: 0 in 10 binaries.
- 13 mutations: 9 caught, 4 survive. Three of the survivors are test gaps (N7); one guards a redundant condition (see "Do the tests fail without the fixes?").

## Findings

### B1 (High, blocking): Adds whose Welcome every default joiner must reject; closed upgraded groups; inaccurate README

**Where**
- `welcome_tree_bound` (`welcome.c:202`): every non-own leaf needs a valid proof, except the GroupInfo signer when it is the sender.
- `add_members_impl` / `create_group_impl` (`groups.c`): check only the *invitees'* KeyPackages, never whether the joiner will accept the rest of the tree.
- `create_group_impl` (`groups.c:511`): silently creates an **unproven creator leaf** when the instance is not enrolled, even with `allow_unproven_members = false`.
- README "Compatibility → Existing groups": "in practice only an unproven member can admit new members".

**Verified (throwaway test, not committed)**

| Step | What happens | Result |
|---|---|---|
| 1 | Alice, Bob and Charlie form a group with unproven leaves, as a 0.9.0 group looks | — |
| 2 | Everyone switches to the 0.10.0 default | — |
| 3 | Alice (unproven creator) `marmot_add_members(Dave)`, Dave's KeyPackage proven | **`MARMOT_OK`**; merged |
| 4 | Bob and Charlie process the Commit | applied |
| 5 | Dave accepts Alice's Welcome | **`MARMOT_ERR_KEY_PACKAGE_IDENTITY`** (Bob's and Charlie's leaves are unproven and not the sender) |
| 6 | Alice's tree afterwards | 4 leaves; Dave's leaf stays, and every member encrypts to it |

So the README's "only an unproven member can admit" is wrong. Only the sender's own leaf is exempt, so **nobody** can admit a member to an upgraded group that has two or more unproven members besides the sender.

The author's own `test_unproven_creator_admits_through_itself` shows the other half. With an unenrolled creator, admin Bob's `add_members` succeeds and merges, and Charlie's join fails. The inviter gets no signal, and the ghost leaf stays until someone removes it.

**The unenrolled-creator case is reachable in Gnostr.**
- Gnostr enrolls only inside the KeyPackage manager, which runs asynchronously at plugin activation and needs a signer approval.
- `gn-create-group-dialog.c` and `gn-mls-dm-manager.c` call `create_group_async` without checking `has_account_proof`.
- A group created before the approval arrives (or after the user declines, or with a slow NIP-46 signer) gets a permanently unproven creator leaf.
- It stays unproven permanently. The group's leaf key is that run's instance key, which is not stored, and no API adds a proof to an existing leaf.

**The spec.** `identity.md` requires the proof on *every* member leaf. In non-legacy mode libmarmot should not produce a leaf without one, nor publish a Commit that its own join rule makes unusable.

**Required**
1. **The inviter enforces the joiner's rule.** `marmot_add_members()` and `marmot_create_group()` must fail (for example with `MARMOT_ERR_KEY_PACKAGE_IDENTITY`) when the post-Commit tree would hold an unproven leaf other than the inviter's own, unless `allow_unproven_members` is set. Otherwise the operation reports success and leaves a ghost.
2. **No unproven creator by default.** `marmot_create_group()` must refuse when the instance is not enrolled for `creator_pubkey`, unless in legacy mode. Gnostr's create-group paths should enroll first, as the KeyPackage manager does. Groundhog's tests can use legacy mode or enroll.
3. **Correct the README**, "Existing groups": after the upgrade, groups with unproven members cannot admit anyone in default mode. Recommend legacy mode during the transition, or the migration path below.
4. **File a bead for the migration path.** Receivers already accept a member's replaced leaf going from no proof to a valid proof (`leaf_binding_check`: `VALID` passes). What is missing is an API: enroll a proof for the *group's* leaf key, then commit a self-update whose UpdatePath leaf carries it. Until it exists, state plainly that upgraded groups need legacy mode to grow.

### Non-blocking

**N1 (Medium): the Welcome-sender exception rests on a caller contract.** libmarmot sees only the rumor. The exemption is safe only if the caller checked that the rumor `pubkey` equals the seal's and verified the seal signature.
- Gnostr does both: `nip59_giftwrap.c` verifies the seal signature (606) and rejects a pubkey mismatch (538–546).
- `marmot.h` documents the requirement on `marmot_process_welcome()`.
- **Abuse analysis:**
  - a creator who never enrolled can exempt only a leaf naming **its own** seal-authenticated account: M1 below shows the identity comparison is tested;
  - it cannot make another account's proven leaf the GroupInfo signer without that leaf's key;
  - re-sealing someone else's Welcome under its own pubkey fails the identity comparison.
- **Residual risk:** a third-party caller that forwards unauthenticated rumors (for example a bridge, or a test harness copied into production).
- **Suggestion:** take the seal-authenticated sender as an explicit argument (`marmot_process_welcome_from(…, sender_pubkey)`), so the contract is visible in the signature.

**N2 (Medium): MDK 0.8 interop.**
- **The advertising is fine.** MDK v0.8.0 `validate_extensions_tag` requires 0x000a and 0xf2ee and accepts other well-formed ids, so `0x0006 0x000a 0xf2ee` passes. The README's citation is accurate.
- **Parsing the LeafNode is plausible but untested.** MDK 0.8 pins OpenMLS 0.8.1 with default features off (no `extensions-draft`), so a LeafNode extension of type 0x0006 should parse as `Unknown(6)`. It should be accepted, because it is listed in the leaf capabilities. The README says "untested", which is correct.
- **The default policy is the bigger interop change.**
  - A default 0.10.0 client refuses every MDK 0.8 KeyPackage (none carries the proof).
  - It rejects a Welcome from any MDK 0.8 group with a second unproven member besides the sender.
  - Gnostr does not set legacy mode, so it can no longer invite MDK 0.8 (Whitenoise) users, or join their groups beyond a two-member one.
  - That may be the right security trade-off, but it is a product decision and should be recorded as one.
- **Recommendation:** run one cross-implementation check with MDK 0.8: parse a 0.10.0 KeyPackage, and add it to an MDK group.

**N3 (Low): signer-only KeyPackages share one leaf signature key.** `prove_key_package_leaf` (`credentials.c:457`) replaces the KeyPackage's fresh key with the enrolled instance key.
- Every unsigned KeyPackage of one instance run, and every group it creates, uses the same leaf key and proof. The spec allows reusing a proof over an identical binding.
- **Costs:**
  - per-KeyPackage key isolation is lost: one group's stored signature key is the leaf key in the instance's other groups;
  - RFC 9420 §7.3 requires `signature_key` to be unique among a group's members, so two of these KeyPackages added to the *same* group (concurrent invites by two admins) would be rejected by RFC-compliant peers.
- **Options:** a proof per KeyPackage (one signer call each), or document the trade-off.

**N4 (Low, documented): limits of retirement.** Both are documented in the README "Limitations", and both are bounded by the next Commit, so there is no regression from 0.9.0.
- **A silent member keeps the full parent until the next Commit.**
  - A candidate member who could win keeps it until it speaks.
  - So does a member removed by the Commit who could win, for example a removed admin whose key sorts at or below the committer's. Its leaf is blank at the new epoch, so it can never be witnessed.
  - **Answer to the question:** a malicious member can keep the parent alive indefinitely only by staying silent until the next Commit. Any member can end that by committing, and periodic self-update Commits (RFC 9420 PCS advice) bound the exposure in practice.
- **Equivocation.** A member that is witnessed and then publishes a winning competitor splits members that retired from members that have not.
  - Before 0.10.0 the same split needed a further Commit to separate members, so 0.10.0 widens the window rather than adding a new class. It is tracked as nostrc-w1m0.
  - A committer equivocating with a second Commit is handled the same way: the committer is excluded from `pending`.
- **Acceptable.**

**N5 (Info): retirement versus `retained-history.md`.**
- The adopted release condition for candidate-advancement material is "outside the rollback horizon".
- The adopted convergence (`convergence.md`) ranks branches by depth and witness quorum before `CommitOrderingSuffix`.
- libmarmot implements a one-epoch horizon with the suffix alone (accepted in W17). Under *that* rule, `could_win` is exact and early release is safe.
- If libmarmot ever adopts witness-quorum ranking, `could_win` must widen: a branch with quorum can beat a lower suffix.

**N6 (Low, pre-existing): deletion is only as good as the backend.**
- `storage_sqlite.c` runs in WAL mode without `secure_delete`, so the replaced full parent (and every ratchet state W19 deletes) can remain in freed pages or the WAL.
- Groundhog sets `secure_delete=ON`, but old page images stay in the WAL until a checkpoint.
- **Suggestion:** enable `secure_delete` and checkpoint after retirement in the libmarmot backend, or say this in the README's "what a stolen store still exposes".

**N7 (Low): test gaps (surviving mutations).**
- M2: the exemption restricted to the signer leaf. A second unproven leaf naming the sender's own account passes; the harm is limited to the sender's own account.
- M4/M5: the witness requiring the same identity and signature key. Without it, a slot re-filled by the Commit (Remove + Add) could witness for the removed member, which may still hold a winning competitor.
- Add a test for each.

## Answers to the review questions

**Can the Welcome-sender exception be abused? Could a creator who never enrolled impersonate someone else in its own leaf?**
- **No, provided the caller authenticates the rumor pubkey (N1).**
  - The exempt leaf must be the GroupInfo signer, and its credential must equal the rumor `pubkey`. M1 shows that removing this comparison fails "Mallory's leaf claims Alice".
  - `marmot_create_group(creator_pubkey = Alice)` does produce such a leaf. But its Welcome needs a seal signed by Alice, which Gnostr's unwrap verifies.
- **What the exception does cost is B1:** it lets groups exist whose other members can never admit anyone.

**Is the proof bound to the leaf's signature key and the group? Is it replay-safe?**
- **Bound to the leaf key: yes.** The template commits to `mls_signature_key` (the exact LeafNode key), the ciphersuite (0x0001) and the scheme (0x0807). `marmot_account_proof_from_signed` requires exactly that template and its own id. The verification path recomputes it from the leaf.
- **Bound to the group: no, and the spec says it should not be** ("authorizes a long-lived key binding"; reuse across KeyPackages and leaves is allowed).
- **Replay across groups or leaves gains nothing.**
  - Reusing a proof needs the same signature key. Using that leaf needs its private key: the LeafNode signature, and 0.9.0's content signatures.
  - Copying someone's whole published KeyPackage into a group creates a leaf that cannot speak or decrypt. Adding any published KeyPackage is ordinary MLS behaviour.
- **Covered by tests:** a proof replayed over another key, a proof signed by another account, and a second "device" of a member without a proof are all rejected on Commits and in Welcomes, including in legacy mode where the proof is bad.

**Is it interoperable with MDK (the 0x0006 advertising)?**
- The tag is accepted by MDK 0.8 (verified in source).
- The LeafNode dictionary should parse as an unknown, capability-listed extension, but that is untested.
- Default policy refuses MDK 0.8 KeyPackages and most MDK 0.8 Welcomes (N2).

**Is the "could still win" set computed exactly as the tie-break?**
- **Yes, for libmarmot's rule.** `commit_key_cmp` orders by privileged first, then committer key, then digest.
  - A competitor beats a privileged Commit only if it is privileged too (so its author is an admin of the parent GroupData) and its key is ≤ the committer's (equal keys compete on digest).
  - It beats an ordinary Commit if it is privileged (an admin), or ordinary with a key ≤ the committer's.
  - `could_win` (`commits.c:315`) is exactly that.
- **Included:** `cmp <= 0` includes the committer's *other* leaves (same account, another device). M3 shows this is tested.
- **Excluded:** leaves without a 32-byte identity (they cannot commit), us, and the committer's own leaf (see N4).
- **Legacy records** (no trailer) keep the committer pending (M8 tested).
- Tie-break versus the adopted convergence: N5.

**Can a malicious member keep the parent alive indefinitely? Is that acceptable?**
- Until the next Commit, by staying silent. Any member can end that by committing.
- It is no worse than 0.9.0, and acceptable (N4).

**Does strip-to-reader remove everything that re-derives the current epoch?**
- **Yes.**
  - Re-deriving epoch N+1 from the parent and the public Commit needs the parent's `init_secret`, plus the commit secret opened with the own leaf key or cached path keys.
  - `mls_group_strip_to_reader` zeroes all `MlsEpochSecrets` except `sender_data_secret`: init, encryption, exporter, external, confirmation, membership, resumption, authenticator, welcome and joiner secrets.
  - It also zeroes the own signature and encryption keys, the path-key cache and the resumption-PSK cache.
- **What remains:** the public tree and GroupContext, and the secret tree's unconsumed epoch-N ratchets, which is the intended late-read window.
- **No other store record holds a full copy:**
  - the exporter table holds one-way exporter values;
  - nothing creates snapshots (N5 of W19);
  - late decrypts and witness updates re-encode the parent in its current tier.
- M6 (keep `init_secret`) fails the author's B1-attack test ("no init secret … left"), and M12 (the reader tier still judges competitors) fails too.
- The storage-level caveat is N6.

**Do the tests fail without the fixes?** See the table below.

| # | Mutation | Result |
|---|---|---|
| M1 | Welcome exemption ignores the leaf's identity | **caught** (`Mallory's leaf claims Alice`) |
| M2 | exemption for any leaf with the sender's identity | survives (N7) |
| M3 | `could_win` strict (`cmp < 0`) | **caught** |
| M4 | witness ignores signature-key equality | survives (N7) |
| M5 | witness ignores identity and key | survives (N7) |
| M6 | strip keeps `init_secret` | **caught** |
| M7 | `could_win` ignores admins | **caught** (a winning competitor refused) |
| M8 | legacy record assumes the committer's leaf is known | **caught** |
| M9 | Welcome accepts an invalid proof as absent | **caught** |
| M10 | a replaced leaf may drop its proof | **caught** |
| M11 | witness epoch check removed | survives (redundant: the record is always the parent of the current epoch) |
| M12 | reader tier still processes a competitor | **caught** |
| M13 | Welcome tree check removed | **caught** |

The author's own list (six mutations per commit) is consistent with these.

## Verification

- **Build.** `cmake -S . -B /tmp/rr20a-build -G Ninja -DBUILD_GROUNDHOG=ON && ninja` ok. The build rewrote a tracked blueprint `.ui` file; I restored it in the scratch tree.
- **Tests.** `ctest -R 'marmot|mls|gnostr|groundhog-store' -j6`: **87 run; 86 passed, 1 skipped** (`groundhog-store-key-keyring`, no keyring).
- **ASAN+UBSAN** (RelWithDebInfo, `-fsanitize=address,undefined -fno-sanitize-recover=undefined -Wno-macro-redefined`, see W19 N6): `ctest -R 'marmot|mls|groundhog-store-marmot'` **26/26 passed**, with no reports. This covers all `marmot_test_*` (MDK and RFC vectors included), `marmot_gobject_test`, the gnostr MLS plugin tests and `groundhog-store-marmot`, with my repro included.
- **`leaks --atExit`: 0 leaks** in `test_commits` (with the repro), `test_ratchet_persist`, `test_mls_framing`, `test_mls_group`, `test_marmot_interop` (MDK vectors loaded: 10375 asserted checks), `test_protocol`, `test_storage_contract`, `test_mls_key_package`, `test_kp_profile` and `test_marmot_gobject`.
- **Repros and mutations** (throwaway, not committed): the upgraded-group join failure (B1), plus the 13 mutations above.

## Recommendation

**REQUEST CHANGES** for **B1**:
- make the inviter refuse Adds its joiners must reject;
- refuse `marmot_create_group()` without enrollment in default mode (and enroll first in Gnostr);
- correct the README's "Existing groups";
- file the migration bead (self-update adding a proof).

**Can follow:** N1–N7. N2 needs an explicit product decision on MDK 0.8 interop in Gnostr's default mode.
