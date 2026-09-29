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

---

## Addendum: final pass on `4319ea45`, `2bcf7314`, `76936eca` (2026-09-29)

Branch `marmot/w17-welcome-commits` at `76936eca`. No code or beads were changed; repros ran in throwaway worktrees (`/tmp/rr17d`, `/tmp/rr17e`), which were removed afterwards.

**Final verdict: REQUEST CHANGES.** Blocking findings: **C1, C2**. R1 and R2 themselves are resolved.

### R1 and R2: resolved

**R1: a stale pending Commit wins.** Resolved.
- A pending record (v2) is bound to its parent's epoch *and* confirmed transcript hash (`commits.c:544-555`). A same-epoch replacement changes the transcript, so the record becomes STALE.
- Only a LIVE record competes with or defers inbound Commits.
- A STALE merge drops the record and returns `WRONG_EPOCH`.
- My repro is now a regression test. With liveness checked by epoch only, or with a stale merge allowed, `test_commits` fails.

**R2: a pending Commit has no recovery path.** Resolved in libmarmot.
- **Echo merge:** our own echo merges a LIVE record (`:1082-1091`).
- **Idempotent merge:** a MERGED leftover finishes as `MARMOT_OK`.
- **Unmergeable Commit:** one that can no longer pass authorization is dropped.
- **Storage error:** the record stays pending and can still be cleared.
- **Restart path:** `marmot_get_pending_commit` returns the stored signed event and whether it was superseded.

Resolved in Gnostr.
- **Classification:** only NIP-01 `OK false` counts as a rejection. Timeout or disconnect is UNCERTAIN.
- **Resolver:** it republishes, merges on the first OK, and clears only on REJECTED or NO_RELAYS. It runs for every group at start.
- **Honest copy:** "sent, but no relay has confirmed it yet".

I spot-checked six mutations: epoch-only liveness, stale merge allowed, no echo merge, no MERGED detection, record dropped on a storage error, Welcomes not moved. Each fails `test_commits`.

**Republish racing an incoming competitor is correct *if* calls are serialized.**
- If the winner arrives first, the record is STALE: the resolver merges it only to drop it, and never republishes.
- If the winner arrives between `get_pending` and merge, we republish a Commit that loses, and the merge returns `WRONG_EPOCH`.
- A losing competitor is deferred and dropped on merge.

C1 is about the calls not being serialized.

**The stored signed event leaks nothing new.** It is the published envelope: NIP-44 ciphertext under the source epoch's exporter secret (the same bytes relays hold), an ephemeral pubkey and a signature. The ephemeral secret is never stored; it is wiped in `marmot_sign_ephemeral`. The Welcome rumors kept in the pending record and the outbox are local-only. They contain the MLS Welcome (encrypted to the joiner's init key) plus the preview tags the local group record already holds, protected like the rest of the MLS state.

### C1 (High, blocking): concurrent operations on one `Marmot` instance are not serialized

**Where**
- marmot-gobject runs every operation via `g_task_run_in_thread` on GLib's shared pool (`marmot-gobject-client.c`, e.g. `:1062` process_message, `:1231` merge/clear) with no mutex. Neither libmarmot nor its storage backends lock.
- The Gnostr router starts one `process_message_async` per incoming kind:445 without waiting for the previous one.
- The new resolver merges at plugin start and every 20 s, while the relay replay (`limit:500`) is still driving inbound processing.

**Why it matters.** Every multi-step transition in `commits.c` assumes exclusive access to the group's records: persist with rollback, `defer_inbound` rewriting the pending record, merge, outbox.

**Evidence.** A two-thread repro on one instance, `marmot_merge_pending_commit` versus `marmot_process_message` of a competing Commit, run under ThreadSanitizer (`-DSANITIZE=thread`), reports **15 data races** in these paths:
- `mem_mls_store` (including its `realloc`) against `mem_mls_load`;
- `mem_save_exporter_secret` against `mem_get_exporter_secret`;
- reached from `pending_apply` / `marmot_commit_persist` on one side and `marmot_commit_process_inbound` / `marmot_group_reconcile` on the other.

With the memory backend that is undefined behaviour. With a thread-safe backend (SQLite) it is still a logical lost update. For example, a winning competitor persisted between the merge's load and its write is overwritten, and the competitor was already marked processed, so it is never re-applied.

No divergence showed up in 30 runs, but the interleaving is unsynchronized. This is exactly the "republish racing an incoming competing Commit" case.

**Before vs now.** The missing serialization is not new. W17 is what makes concurrent *state transitions* (Commit ingestion, merges, resolvers) routine.

**Fix.** Serialize every libmarmot call per `MarmotGobjectClient`: a `GMutex` held for the whole body of each `*_thread` function, or a dedicated single-worker `GThreadPool`. Also document in `marmot.h` and the README that a `Marmot` instance is not thread-safe. Add a concurrent merge/process test (TSAN-clean) as a regression.

### C2 (Medium-High, blocking): the Welcome outbox loses Welcomes

**(a) libmarmot overwrites the outbox (verified).** `outbox_store()` (`commits.c:727-739`) *replaces* the outbox record, and producers check only for a pending Commit (`groups.c:722-723`), not for unsent Welcomes.

Repro: Alice adds Dave and merges; Dave's Welcome is not yet marked sent (offline, or its send failed). Alice adds Eve and merges. `marmot_get_unsent_welcomes` then returns **only Eve's** Welcome. Dave's is gone, while Dave already occupies a leaf: a ghost member who can never join, and future UpdatePaths encrypt to him.

`marmot_mark_welcomes_sent()` also deletes the whole outbox. Welcomes stored between the application's read and its mark are lost too.

**(b) Gnostr marks Welcomes sent before sending them.** `send_unsent_welcomes()` (`gn-mls-pending-commits.c:45-66`) dispatches `gn_mls_event_router_send_welcome_async(..., NULL, NULL, NULL)` and calls `mark_welcomes_sent` immediately, without waiting for, or checking, the gift-wrap publishes. A failed send loses the Welcome, which is precisely the failure the outbox exists to survive.

**Fix**
- Append to the outbox.
- Remove entries individually, by recipient plus rumor digest, only after that entry's send is confirmed (or refuse a new Commit while the outbox is non-empty).
- In Gnostr, mark each entry after its gift wrap reaches a relay.
- Resends then become possible, so the joiner must also treat duplicates safely (N1).

### Non-blocking

- **N1 (Medium, pre-existing): a duplicate Welcome rolls the joiner back.** `accept_welcome` (`welcome.c`) never deletes `kp_priv`/`kp_full` after joining and does not check whether the group is already active. A second copy of a Welcome (another gift wrap) is offered as a new invitation. Accepting it overwrites `mls_group` with the Welcome-epoch state, which undoes a joiner that has moved on. Add an "already a member of this group" guard, and consider consuming non-last-resort KeyPackage privates. This matters more once C2's resends exist.
- **N2 (Low): resolver lifecycle.**
  - Retries use a fixed 20 s interval, up to 15 per activation, with no backoff or jitter.
  - The `g_timeout` holds a router reference past deactivation. A deactivated context yields `G_IO_ERROR_CANCELLED`, which counts as UNCERTAIN and reschedules, so retries continue for up to five minutes after deactivation.
  - Concurrent resolvers for one group (settings action, timer, start) are not coalesced, giving duplicate publishes and merges and, with the outbox, possible duplicate Welcome sends.
  - Suggested: keep an in-flight set per group, cancel timers on deactivation, and use exponential backoff with jitter.
- **N3 (Low): no outbox flush after an echo merge.** A merge by relay echo happens in `process_message`, and the router does not flush the outbox afterwards. The Welcomes wait for the next resolver retry (≤ 20 s) or the next start. Trigger `send_unsent_welcomes` on `MARMOT_RESULT_COMMIT`.

### Verification

- Build with `BUILD_GROUNDHOG=ON`: ok.
- `ctest -R 'marmot|mls|gnostr' -j6`: **81/81 passed**.
- ASAN+UBSAN (runtime confirmed): `test_commits`, `test_mls_welcome`, `test_mls_group`, `test_protocol`, `test_marmot_interop` and `test_marmot_gobject` all pass with no reports.
- `leaks --atExit`: `test_commits`, `test_mls_group` and `test_protocol` each report **0 leaks**.
- TSAN repro for C1: 15 data-race reports.
- Outbox repro for C2(a): outbox holds only Eve after two merged Adds.

### Final recommendation

**REQUEST CHANGES** for **C1** (serialize per client, plus a TSAN regression) and **C2** (append-only outbox with per-entry confirmation; Gnostr marks each entry only after its send is confirmed).

R1 and R2 are properly fixed and pinned. N1–N3 can follow, but N1 should land together with the C2 resend change.

---

## Addendum 2: final pass on `330da3e9`, `47483248` (2026-09-29)

Branch `marmot/w17-welcome-commits` at `47483248`. No code or beads were changed. Repros ran in throwaway worktrees (`/tmp/rr17f`, `/tmp/rr17g`), which were removed afterwards.

**Final verdict: REQUEST CHANGES.** One blocking finding: **D1**, a deadlock introduced by the new client lock. C1, C2 and N1–N3 are otherwise resolved.

### C1: resolved, except for D1

- **The lock.** A `GMutex` per `MarmotGobjectClient` is held for the whole body of every `*_thread` function and every sync accessor.
- **Direct users.** `marmot_gobject_client_lock()` / `unlock()` cover every direct libmarmot use in Gnostr. I found exactly four sites: `marmot_encrypt_media` and `marmot_decrypt_media` in `gn-mls-media-manager.c`, and `marmot_add_members` and `marmot_leave_group` in `gn-group-settings-view.c`. Each holds the lock only around the libmarmot call and calls no client function inside it. Groundhog has no direct libmarmot calls.
- **Tests.** TSAN on the gobject suite passes 71/71 with 0 warnings. With the lock stubbed out, `/marmot-gobject/client/serializes-merge-and-process` produces **7** TSAN reports, as claimed. The test passes functionally without the lock, so it only catches a regression when CI runs it under TSAN.
- **Ordering.** Lock ordering is trivial: one non-recursive lock and no nesting. `g_task_return_*` runs under the lock, but its callbacks dispatch to the task's context, so that is at most a short main-thread stall.

### D1 (Medium-High, blocking): `::group-updated` and the other client signals can be emitted inline under the lock, and handlers that call the client deadlock

**Where**
- `queue_client_signal()` (`marmot-gobject-client.c:328-338`) is called from `process_welcome_thread`, `accept_welcome_thread`, `process_message_thread` and `pending_commit_thread` while `self->lock` is held (`:816`, `:890`, `:1051`, `:1059`, `:1232`).
- It uses `g_main_context_invoke(NULL, …)`. When no thread owns the global default context, a worker thread acquires it and runs `emit_queued_signal` inline. That happens before a loop runs, after it quits (application shutdown), or in a consumer whose loop runs on a thread-default context.
- The signal is then emitted on the worker, under the lock.

**Consequences**
- Any handler that calls a client function blocks forever on the non-recursive lock. This contradicts the header's own rule not to call client functions while the lock is held.
- GTK handlers run on a worker thread.
- Gnostr's new N3 handler does exactly this: `::group-updated` → `send_unsent_welcomes` → `marmot_gobject_client_get_unsent_welcomes`.

**Verified.** A gobject-test repro (not committed) connects a `::group-updated` handler that calls `marmot_gobject_client_get_group`. It starts `merge_pending_commit_async` with no loop running. Result: `handler entered=1 returned=0`. The worker deadlocks holding the client lock, so every later client call would also block. If the main thread calls into the client during shutdown, the application hangs on quit.

**Fix (small)**
- Never emit inline. Capture the context at construction (`g_main_context_ref_thread_default()`) and attach a `g_idle_source_new()` to it for every queued signal, or use `g_main_context_invoke` only after the locker is released and never from a worker.
- Add the repro above as a regression: a handler that re-enters the client must return.

### C2: resolved (limit tracked in nostrc-ba81)

- **Append-only outbox.** Merges append; entries are de-duplicated by id; `mark_welcomes_sent(ids)` removes only the named entries.
- **Serialized.** Append and mark both run under the client lock.
- **Verified.** My repro now yields `outbox after two merged Adds: 2: Dave Eve`, and after marking Dave's only Eve's remains.
- **Gnostr marks per entry.** It marks each id only after `gn_mls_event_router_send_welcome_finish` succeeds, and a per-id in-flight set prevents duplicate concurrent sends.
- **Remaining limit (nostrc-ba81).** Gift-wrap "success" means dispatched, not relay OK, and default rather than inbox relays are used. A Welcome no relay stored is still marked sent.

**Welcome id collisions.** The id is `SHA-256(recipient ‖ rumor)` (`commits.c`, `welcome_id`). The recipient is a fixed 32 bytes, so the hash input is unambiguous. Distinct Welcomes differ in their ciphertext, so their ids differ; identical copies merge, which is intended. The only collision path is allocation failure: `welcome_id` then writes an all-zero id. Two such entries would de-duplicate one Welcome away on append, and a mark would remove both (Low). Append should fail with `MARMOT_ERR_MEMORY` instead.

### N1–N3: resolved

- **N1 (duplicate Welcome).** `already_member_of()` (`welcome.c`) treats a Welcome as a copy when stored state for the group is at or after the Welcome's epoch *and* our stored leaf's signature key is in the Welcome's tree. It then returns `MARMOT_ERR_WELCOME_ALREADY_ACCEPTED` and retires the copy without touching state. A re-invite after a removal (stored epoch behind the new Welcome) still joins.
- **N2 (resolver lifecycle).**
  - One resolution per group; later callers wait on it.
  - Exponential backoff from 5 s, capped at 300 s, with ≤ 25 % jitter and at most 12 retries.
  - Deactivation removes the timers and does not reschedule on `G_IO_ERROR_CANCELLED`.
- **N3 (flush after echo merge).** `::group-updated` flushes the outbox (but see D1).
- **Minor:** a failed Welcome send is retried only at the next flush trigger (a merge, `::group-updated`, a resolver run or the next start), with no timer of its own (Low).

### Verification

- Build with `BUILD_GROUNDHOG=ON`: ok.
- `ctest -R 'marmot|mls|gnostr' -j6`: **81/81 passed**.
- TSAN (`-DSANITIZE=thread`) on `test_marmot_gobject`: **71/71 ok, 0 warnings**. With the lock removed, the concurrency test produces 7 warnings.
- ASAN+UBSAN: `test_commits`, `test_mls_welcome`, `test_mls_group`, `test_protocol`, `test_marmot_interop` and `test_marmot_gobject` all pass with no reports.
- `leaks --atExit`: `test_commits`, `test_mls_group` and `test_protocol` each report **0 leaks**. `test_marmot_gobject` reports one 48-byte leak, a pre-existing test bug unrelated to this round: `test_client_finalize_releases_storage` takes an extra `g_object_ref` it never drops.

### Final recommendation

**REQUEST CHANGES** for **D1**: never emit client signals inline or under the lock, and add a re-entrant-handler regression test.

Everything else from W17 and W17b (R1, R2, C1's lock coverage, C2, N1–N3) is verified and pinned. The zero-id OOM case, the Welcome-send retry trigger and nostrc-ba81 can follow separately.

---

## Addendum 3: final confirmation of D1 on `542af7ad`, `51bafc38` (2026-09-29)

Branch `marmot/w17-welcome-commits` at `51bafc38`. No code or beads were changed; repros ran in a throwaway worktree (`/tmp/rr17h`), which was removed afterwards.

**Final verdict: APPROVED.**

### D1: resolved

**No emission on a worker or under the lock**
- The only `g_signal_emit` left in `marmot-gobject-client.c` is in `emit_queued_signal`, and the only `g_main_context_invoke` has been removed.
- `queue_client_signal` (still called by workers holding the lock) now only creates a `g_idle_source_new()` and attaches it to the client's captured context. The emission runs when that context iterates, on the thread iterating it, after the worker has released the lock.
- The new regression, `/marmot-gobject/client/signal-handler-may-reenter`, asserts the handler runs on the owner of the default context and only once the context iterates, and that sync client calls do not block meanwhile. It passes.
- With `queue_client_signal` reverted to the old inline `g_main_context_invoke`, the same test deadlocks and is killed by a 40 s `timeout` (rc 124).

**Idle-source lifetime versus client finalize**
- Each queued signal takes strong references to the client and the emitted object, and releases them only after emitting. A pending emission therefore can never touch a finalized client.
- Repro (not committed), under ASAN+UBSAN:
  1. Merge runs; the app drops its last references to the client and store while the `::group-updated` emission is still queued: `finalized=0 emitted=0`.
  2. The context is drained: the signal is delivered, then the client finalizes: `finalized=1 emitted=1`.
  3. No ASAN reports.
- Handlers remain responsible for disconnecting themselves in their own dispose, as the Gnostr views do.

**Residual (Low, non-blocking).** The source has no destroy notify. If the captured context is never iterated again, queued signals keep their references and allocations, and the client's finalize never runs. That happens with a thread-default context whose loop ended, or at process exit. Consider `g_source_set_callback(..., queued, queued_signal_free)`, with the emit path leaving the release to the notify.

**Thread-default semantics for clients created off the main thread**
- `marmot_gobject_client_init` captures `g_main_context_ref_thread_default()`.
- A client created on the main thread, or on any thread without a pushed context (including GTask workers), gets the global default context, so signals go to the application's main loop.
- A client created on a thread that pushed its own context gets that context, so signals are delivered only while that thread iterates it.
- This is the documented contract in `marmot_gobject_client_new()` and the README. GTask callbacks still go to each caller's own context, so if a client is used from several threads, the relative order of signals and async callbacks across contexts is not defined (informational).

### Low items: resolved

- **Welcome-id OOM.** `welcome_id()` returns an error; outbox load and append fail with `MARMOT_ERR_MEMORY` instead of inventing an all-zero id.
- **Welcome resend timer.** A failed Welcome send schedules a per-group resend of the outbox, using the same exponential backoff with jitter. Timers are removed on deactivation.
- **Test leak.** The gobject suite's test leak is fixed: `leaks` reports 0.

### Verification

- Build with `BUILD_GROUNDHOG=ON`: ok.
- `ctest -R 'marmot|mls|gnostr' -j6`: **81/81 passed**.
- TSAN on `test_marmot_gobject`: **73 ok, 0 not ok, 0 warnings**.
- ASAN+UBSAN: `test_commits`, `test_mls_welcome`, `test_mls_group`, `test_protocol`, `test_marmot_interop` and `test_marmot_gobject` (73 ok) all pass with no reports.
- `leaks --atExit`: `test_commits`, `test_mls_group`, `test_protocol` and `test_marmot_gobject` each report **0 leaks**.

### Final recommendation

**APPROVED.** All blocking findings from W17, W17b and its addenda are resolved and pinned by tests: R1, R2, C1, C2 and D1.

**Tracked follow-ups:**
- nostrc-w1m0 (full convergence);
- nostrc-6r6s (inbound kind:445 signature verification; callers verify until then);
- nostrc-ba81 (Welcome delivery confirmation and inbox relays);
- nostrc-xgko (privilege from the proposal set).

**Low follow-up:** the destroy-notify for undispatched signal sources noted above.
