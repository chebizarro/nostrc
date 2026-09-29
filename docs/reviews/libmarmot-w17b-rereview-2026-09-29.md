# libmarmot W17b re-review: signed kind:445, publish-before-merge, multi-member Commits

- **Reviewer:** independent peer reviewer (AGENTS.md "Pre-Push Requirements → Peer Review")
- **Branch:** `marmot/w17-welcome-commits` at `cf15b129`, on the reviewed `7e9f8808`
- **Commits:**
  - `62a37712` (B2, MLS multi-member Commits)
  - `cf9e7d87` (B1, B2, N2, N3, N5)
  - `cf15b129` (B3, README and manifest)
- **Previous review:** `docs/reviews/libmarmot-w17-review-2026-09-29.md` (`6dfe3538`)
- **Date:** 2026-09-29
- **Verdict:** **REQUEST CHANGES**. Blocking findings: **R1, R2**, both in the new pending-Commit state machine.

No code or beads were changed. Repros and mutations ran in throwaway worktrees (`/tmp/rr17b`, `/tmp/rr17bcf`), which were removed afterwards.

## Summary

**The previous findings are addressed, and each fix is pinned by a test that fails without it.**
- Every kind:445 is signed with a fresh, wiped ephemeral key.
- Commits are merged only after a relay's OK.
- Multi-member changes are one Commit with one Welcome.
- The README states the permanent-split limits and the tie-break basis.
- Crash reconcile, strict rollback and every earlier mutation survivor are pinned.

**But the new pending-Commit state machine has two defects, both verified.**
- **R1: a stale pending Commit wins.** A pending Commit stays "live" after the state it was built on is replaced by a winning previous-epoch Commit. It then defers the group's real next Commit and merges onto the wrong parent. That splits a member off a group that libmarmot had just converged.
- **R2: a pending Commit has no recovery path.** After a crash, or a publish whose outcome is unknown, the committer is wedged. The relay echo of our own pending Commit proves a relay stored it, but it is not used to merge. Gnostr never resolves a leftover pending record and clears on ambiguous failures.

## Previous findings: status

| W17 finding | Status | Evidence |
|---|---|---|
| **B1**: unsigned kind:445, merge before publish | **Resolved, with new defects R1/R2** | See below. |
| **B2**: multi-entry producers fork | **Resolved** | See below. |
| **B3**: undisclosed permanent splits | **Resolved** | See below. |
| **N2**: crash between MLS-state and record writes | **Resolved** | `marmot_group_reconcile()` runs before process / create-message / media / Commit ops. Persist order means the record can only lag. Disabling it fails `test_commits`. |
| **N3**: best-effort rollback | **Resolved** | NOT_FOUND vs read error; failed undo → `MARMOT_ERR_STORAGE`; fault-injection test. Both mutations fail `test_commits`. |
| **N5**: test survivors | **Resolved** | See below. |
| **N1**: losing-branch invalidation and deferral | Tracked | nostrc-w1m0 (P1), documented. |
| **N4**: privilege from state difference | Tracked | nostrc-xgko (P3). |

**B1 detail**
- `marmot_sign_ephemeral()` (`commits.c:438-455`) uses one CSPRNG secp256k1 key per event, checks it, wipes the key and its hex copy, and verifies the signature.
- It is used for Commit events and in `marmot_create_message()`.
- Add, remove and rename now stage a pending Commit. Gnostr publishes to one relay at a time until an OK, then merges; Welcomes go out only after a successful merge.
- `WRONG_EPOCH` shows honest copy ("another member's change reached the group first").
- `test_events_signed_by_fresh_ephemeral_keys` fails when signing is removed from either path.

**B2 detail**
- `mls_group_add_members` puts N Adds, one UpdatePath and one Welcome in a single Commit. Each joiner gets its own lowest-common-ancestor path secret, and duplicates or more than 64 entries are refused.
- `mls_group_remove_members` is the mirror image.
- `create_group`, `add_members` and `remove_members` now produce one Commit.
- `test_multi_member_commits_converge` covers a three-invitee create, a two-member add and a two-member remove.
- nostrc-wc6v is still open and should be closed when this lands.

**B3 detail**
- The README "Known limitations" section covers the depth-2 race (as verified last round), a Commit a relay stored after we cleared it, joiners on a losing branch, and messages across a switch.
- The mixed-implementation note explains why `CommitOrderingSuffix` was chosen over legacy MIP-03's `created_at`/event-id rule.
- w1m0 is now P1.

**N5 detail.** Keys are deterministic (`member_rekey_order`). All five previous survivors now fail `test_commits`: committer identity, GroupData removal, outer/inner epoch match, own-leaf guard, persistence undo. The privileged step fails deterministically.

## Findings

### R1 (High, blocking): a stale pending Commit defers the canonical branch and merges onto the wrong parent

**Where**
- `commits.c:851`: `live = p.post.epoch == cur.epoch + 1`. Liveness is decided by epoch number alone.
- The replacement branch (`:869-898`) never supersedes the pending record.
- `marmot_commit_merge_pending` (`:672-698`) merges whenever `p.post.epoch == cur.epoch + 1`.

**Scenario (verified).** Trio group at epoch 2. Of Bob and Charlie, W has the lower key and L the higher.

| Step | Event | Result |
|---|---|---|
| 1 | L and W self-update concurrently | |
| 2 | Alice receives L's Commit first | applies it (epoch 3, L branch) |
| 3 | Alice renames | pending Commit, built on L's state |
| 4 | W's Commit reaches Alice | wins the one-level ordering; Alice's live state is replaced |
| 5 | Check | all three converged: epoch 3, identical exporter secret `768b…` |
| 6 | W commits again (`c_X`, the group's real next Commit) | Alice gets `MARMOT_ERR_OWN_COMMIT_PENDING` (-44): deferred behind the stale rename |
| 7 | Relay OK for the rename; `marmot_merge_pending_commit` | returns **0** and applies the post-state built on L's dead branch |

**Final state:** Alice is at epoch 4 with exporter `4ce1…`. Bob and Charlie are at epoch 4 with `0b7e…`. Charlie cannot decrypt the rename (`MARMOT_ERR_NIP44`).

**Why this matters.** This is not the documented depth-2 limitation: without the stale pending record, Alice had already converged. The merge also stores a retained parent (`cur`, W's branch) that is not the parent of the state it installs.

**Fix**
- Bind the pending record to its parent. Store `pre`'s epoch authenticator, or its confirmed transcript hash and tree hash.
- Treat the record as live, and allow a merge, only when that binding equals the current state. Otherwise the merge returns `MARMOT_ERR_WRONG_EPOCH` (superseded) and drops the record.
- In the replacement branch, supersede the pending record and re-process its deferred Commits.
- Add the scenario above as a regression test.

### R2 (High, blocking): a pending Commit cannot be recovered after a crash or an ambiguous publish

**Where**
- `commits.c:852-858`: our own echo returns `OWN_MESSAGE` and does not merge.
- `groups.c:715-716`: any pending record blocks new Commits.
- There is no public "has pending" query (`marmot_commit_has_pending` is internal).
- The pending record does not keep the signed event, so the app cannot republish it after a restart.
- Gnostr calls merge or clear only from the publish callback (`gn-group-settings-view.c:386-435`, `:579-609`).
- `gn_mls_publish_until_ack` moves on after *any* error (`gn-mls-commit-publish.c:31-47`), and all failures lead to clear.

**Scenario 1: crash while pending (verified).**
1. Alice renames, leaving the Commit pending.
2. A relay stores it and Bob and Charlie apply it, but Alice's process dies before merge.
3. On restart, relay backfill delivers Alice's own Commit: `OWN_MESSAGE`, not merged.
4. Her next rename fails with `MARMOT_ERR_OWN_COMMIT_PENDING`.
5. Bob's next Commit is undecryptable (`MARMOT_ERR_NIP44`).

Alice stays at epoch 2 indefinitely, and Gnostr has no path that resolves it. Commits from others that lose to the stale pending one are deferred instead of applied (up to `PENDING_MAX_DEFERRED`, after which they are dropped).

**Scenario 2: ambiguous publish.**
- A relay stores the Commit, but the OK is lost to a timeout or disconnect, and the remaining relays fail.
- Gnostr clears the Commit and tells the user "Could not publish the invitation" or "The group was not renamed".
- In fact the members apply it, and the committer is split off. This is the README's "relay stored a Commit we cleared" case.

It is avoidable. Gnostr treats "definitely rejected" (NIP-01 `OK false`) the same as "unknown", and the echo that would settle it is ignored.

**Fix (minimum)**
- **Merge on echo.** When the echo's digest matches a pending record that is live and still parent-bound (after R1), merge it. A relay-stored copy proves the Commit was published, and this alone repairs both scenarios once the echo arrives.
- **Gnostr: clear only when certain.** Clear only when every relay answered `OK false`. On timeout or disconnect, keep the Commit pending and say "may have been sent". Resolve it when the echo merges it, or on retry.
- **Restart path.** Expose a has-pending query or a pending event getter, and keep the signed event in the pending record so it can be republished. Then Gnostr can republish or keep waiting instead of staying wedged.

### Non-blocking

- **N-a (Low): idempotent merge is untested.** The "merge is idempotent after a crash between persist and pending delete" path (`commits.c:676-683`) survives mutation: forcing `merged = false` passes every test. Add a crash-injection test.
- **N-b (Low): the ephemeral key stays on the stack.** `nostr_event_sign()` (libnostr `event.c:703-785`, existing code) keeps `privkey_bin` and the keypair on the stack without wiping them. `marmot_sign_ephemeral` wipes its own copies, but the key survives inside libnostr. Impact is low, because the key only authenticates an already-published transport envelope. Wipe those locals in libnostr, or sign through a wiping path.
- **N-c (Low): some merge failures leave the Commit pending.** A merge failure other than `WRONG_EPOCH` leaves the record in place with no recourse. Examples: storage errors, or `marmot_commit_authorize` failing against `cur`. The Gnostr add path then shows "Could not add the member" while later Commits return `OWN_COMMIT_PENDING`. Offer clear on such errors, or make the merge drop a record that can never apply.
- **N-d (Info): the add path bypasses marmot-gobject.** Gnostr's add path calls `marmot_merge_pending_commit` directly, so marmot-gobject's `::group-updated` is not emitted for adds. The view emits `member-added` itself, so this is a consistency note only.

## nostrc-6r6s (receivers do not verify the kind:445 id or signature): should not block

**Spec.** `transports/nostr.md` says receivers MUST verify the kind:445 id and signature before decrypting, so libmarmot is non-conforming until 6r6s lands. The bead is rightly open.

It should not block this branch, for three reasons:

1. **The signature proves nothing about the sender.** It comes from a fresh ephemeral key that anyone can generate. Marmot sender authenticity comes from the NIP-44 layer, keyed by the member-only exporter secret, and from MLS signatures and membership tags. Anyone can re-wrap a real ciphertext in a validly signed new envelope, so verification would not stop replays under new ids either. libmarmot already de-duplicates Commits by `SHA-256(MLSMessage)` and app messages through MLS generations.
2. **Gnostr's main path already verifies.** Events reach the plugin through nostrdb ingestion. nostrdb verifies note signatures by default (`ndb_backend.c:270-272`: `ndb_default_config`, `flags = 0`, no `NDB_FLAG_SKIP_NOTE_VERIFY`), so unsigned or mis-signed events do not reach libmarmot there.
3. **Nothing is marked processed on failure.** Processed markers keyed by the unverified event id are written only after an event succeeds. A forged or mismatched id cannot mark someone else's real event as processed.

**Remaining risk.** Spec conformance, and API consumers that feed libmarmot unverified JSON.

**Recommendations**
- Keep it P2, and fix it before any release that documents `marmot_process_message` for raw relay input.
- Until then, the 0.5.0 README should tell callers to verify id and signature first. It currently only says receivers "do not yet verify".
- When implementing it, note that Gnostr's gift-wrap route to kind:445 (`gn-mls-event-router.c`, the kind:445 branch after unwrapping) delivers NIP-59 *rumors*, which are unsigned by design. Verification must not break, or should consciously drop, that path.

## Verification

- Build: `cmake -S . -B … -G Ninja -DBUILD_GROUNDHOG=ON && ninja` ok.
- `ctest -R 'marmot|mls|gnostr' -j6`: **81/81 passed**. Includes `marmot_test_commits` (12 cases), `marmot_gobject_test`, `gnostr-test-mls-commit-publish`, `gnostr-test-mls-group-error` and `groundhog-store-marmot`.
- ASAN+UBSAN (runtime confirmed): `test_commits`, `test_mls_welcome`, `test_mls_group`, `test_protocol` and `test_marmot_interop` all pass with no reports.
- `leaks --atExit`: `test_commits`, `test_mls_group` and `test_protocol` each report **0 leaks**.
- Mutations: of 17 applied to the new pending, signing, reconcile and rollback logic and the previous survivors, 16 fail `test_commits`. The one survivor is N-a.
- Repros (throwaway tests, not committed): the R1 stale-branch merge and the R2 crash-while-pending wedge, with the outcomes described above.

## Recommendation

**REQUEST CHANGES.**

**Must fix before push**
- **R1:** bind the pending Commit to its parent; supersede it on replacement; merge returns `WRONG_EPOCH` when the parent changed.
- **R2:** merge on the echo of our own pending Commit; have Gnostr clear only after definite rejections; add a restart path (has-pending query, or keep the signed event for republish).

Add both repros as regression tests.

**Can follow separately:** N-a through N-d, and nostrc-6r6s as analysed above.

Everything addressed from W17 (B1 signing and publish-before-merge, B2, B3, N2, N3, N5) is in good shape.
