# W22 review: MLS blockers (removal, count-free banner, stalled relay, flip conditions)

- **Reviewer:** independent peer reviewer (AGENTS.md "Pre-Push Requirements → Peer Review")
- **Branch reviewed:** `groundhog/w22-enable-encrypted-groups` at `e73d1528`, 4 commits on `15954a4f` (origin/master)
- **Review branch:** `groundhog/w22-review`
- **Date:** 2026-09-30
- **Verdict (initial, at `e73d1528`):** **REQUEST CHANGES**. Blocking finding: **B1**, a removal that loses the tie-break ends the group permanently if it arrives first.
- **Verdict (final pass, at `1d9af674`):** **REQUEST CHANGES**. B1 and C1 are closed; new blocking finding **B2**. See "Final pass" at the end.
- **Also found:** **C1**, a pre-existing heap-use-after-free in the encrypted-group UI, found by ASAN. It is outside the diff, but should be fixed before the flip.

**Commits**

| Commit | Bead | Change |
|---|---|---|
| `9e0ecac9` | nostrc-xrya | libmarmot: an admin's authenticated Commit removing us ends the group (inactive, `marmot_get_group_removal()`); Groundhog `GhMlsGroup:end` / `:removed-by`, no reading or sending, history kept |
| `022bbdda` | nostrc-oya4 | count-free "Waiting for an earlier change…" (`decrypt-pending`); the join-second rule; junk ids remembered |
| `77168cc9` | nostrc-iihf | only stored-answer events refresh a stalled relay's quiet timer |
| `e73d1528` | nostrc-8kb2 | flip conditions and bead deps; a real `gh-send-ui.c` routing test |

The flag stays at 0 (charter §7.9, nostrc-7gx7): confirmed in `gh-features.h`.

**Checked against:** RFC 9420 §6.1–6.2 (PublicMessage authentication), §12.1.3 (Remove), §12.4.2 (UpdatePath and its recipients); Marmot convergence as libmarmot implements it (the W17 one-epoch `CommitOrderingSuffix` rule).

No code or beads were changed. Repros and mutations ran in a throwaway worktree (`/tmp/rr22`), which was removed afterwards.

## Summary

**The removal check itself is sound.**
- `mls_group_commit_removes_self` reuses the full Commit authentication: framing for this group and epoch; the sender bound to the FramedContent; the committer's signature over the pre-Commit GroupContext; the membership tag.
- It then requires a well-formed inline proposal list with an UpdatePath, and an inline Remove of our occupied leaf by another member.
- `removal_key` requires the committer to be an admin of the pre-Commit GroupData, and orders the removal as privileged.
- **A member cannot fake a removal:** a non-admin's forged removal is refused, as the author's test shows.
- **A member cannot suppress one:** an ordinary Commit never beats a privileged one, and only another admin's privileged Commit with a lower key can win. That is convergence, not suppression.

**Blocking: B1.** What decides the outcome is which Commit arrives first. If the losing removal arrives first, the group ends permanently. `evict` makes the group inactive, and from then on `marmot_process_message` refuses *everything* with `USE_AFTER_EVICTION`, including the winning competitor for that epoch.
- Verified: the removed member stays ended and shows "You were removed by Alice", while the canonical epoch still has its leaf and the others keep encrypting to it.
- The mirror order behaves correctly.
- The tie-break branches of the removal path are also untested.

**The Groundhog fixes are right and tested.**
- Ended groups stop reading and sending, and hold nothing.
- The count-free banner.
- The join-second and junk memory rules.
- iihf.
- Each fix fails a test when reverted, except the membership backstop.

**Test runs.**
- ctest: 97 run; 93 passed, 4 skipped as on base.
- ASAN+UBSAN: every run test passes except `groundhog-mls-ui-gui /enrollment`, which is **C1** (pre-existing). The other GUI subtests are clean.
- `leaks`: 0 in libmarmot and marmot-gobject. `groundhog-mls-service` leaks only in the libnostr/libgo relay transport (pre-existing, N7).

## Findings

### B1 (High, blocking): a losing removal that arrives first ends the group for good

**Where**
- `marmot_commit_process_inbound` (`commits.c`), live path: on `MARMOT_ERR_MLS_PROCESS_MESSAGE` plus `mls_group_commit_removes_self`, then `removal_key`. It then either defers (our pending Commit wins) or calls `evict` (`commits.c:1538`).
- `process_group_event` (`messages.c:820`): an inactive group returns `MARMOT_ERR_USE_AFTER_EVICTION` before any Commit processing.

**Verified (throwaway test, not committed).** Trio fixture: Alice and Bob are admins, and Bob's key sorts lower, so his privileged Commit beats Alice's.
- In epoch 2, Alice commits "remove Charlie".
- Concurrently, Bob commits a rename (also privileged). **Bob's rename is the canonical epoch 3.**

| Order at Charlie | Charlie's outcome | Canonical epoch 3 (Alice, Bob) |
|---|---|---|
| Alice's removal, then Bob's rename | removal → `COMMIT`, **inactive, removed by Alice** (record persisted); Bob's winning rename → **`MARMOT_ERR_USE_AFTER_EVICTION`**; Bob's next message → `USE_AFTER_EVICTION` | 3 member leaves, **Charlie still in**; Alice's pending removal lost to Bob's rename |
| Bob's rename, then Alice's removal | rename applied; removal → `MARMOT_ERR_WRONG_EPOCH` (it loses to the applied Commit), active; reads Bob's message | same |

**So an honest two-admin race leaves the group diverged.**
- Charlie permanently believes he was removed: Groundhog persists `GH_MLS_GROUP_END_REMOVED`, closes the subscription and shows "You were removed from this group by Alice."
- The group still has his leaf. Every member keeps encrypting to it, and it stays until an admin removes it again, which nothing prompts.
- The same applies to Commits deferred behind our own pending Commit. `marmot_commit_clear_pending` replays them in arrival order, so a deferred losing removal replayed before its winner evicts first. The winner then reaches `marmot_commit_process_inbound` on a group already marked inactive.

**The tie-break branches are untested.** Two mutations survive:
- L3: our pending Commit never beats a removal;
- L4: a previous-epoch removal that *loses* to the applied Commit evicts anyway.

The second is exactly my second repro's case (`WRONG_EPOCH`), and no author test covers it.

**Required**
1. **Keep an ended-by-removal group able to judge competitors for the removal's epoch.**
   - Store the removal's `MarmotCommitKey` with the record (the epoch is already there), and keep the epoch-E state.
   - While the group is inactive *by removal*, route Commits of epoch E past the `USE_AFTER_EVICTION` guard.
   - One that beats the removal key and applies re-activates the group, clears the removal record and applies it.
   - One that loses → `WRONG_EPOCH`.
   - The competitor set is yuj2's `could_win` for a privileged key: admins of the pre-Commit GroupData whose key sorts at or below the remover's, other than the remover. When it is empty, the removal is final at once and N1's deletion can run.
   - Groundhog must follow a re-activation: end `NONE`, resubscribe.
2. **Replay deferred Commits so the winner is judged first**, or judge the deferred set as a whole. That way, clearing a pending Commit cannot evict ahead of a winner.
3. **Tests:**
   - both arrival orders of a losing removal (the table above);
   - our own pending Commit beating a removal (L3);
   - a previous-epoch removal that loses (L4) and one that wins;
   - a deferred losing removal replayed before its winner.

### C1 (High, pre-existing, outside the diff; fix before the flip): use-after-free in `sync_identity`

- **Found by:** ASAN on `groundhog-mls-ui-gui`, subtest `/groundhog/mls-ui-gui/enrollment`.
- **Where:** `gh-mls-new-group-page.c:176–188` (from `ba0fb88a`, unchanged here).
- **The bug:**
  - `before` borrows the row's title pointer (`adw_preferences_row_get_title`).
  - `adw_preferences_row_set_title()` then frees that string.
  - `g_strcmp0(before, copy.title)` reads the freed memory. ASAN: heap-use-after-free, read of size 1, freed in `adw_preferences_row_set_title`.
- **When it fires:** every time the identity copy changes while the group is shown (here "waiting for the signer" → "denied"). The plain build passes only because the freed bytes happen to still compare.
- **Fix:** `g_autofree gchar *before = g_strdup(...)`.
- **Why it was missed:** the W21 UI review did not run the GUI suite under ASAN.

### Non-blocking

**N1 (Medium): key material is kept after removal.**
- `evict` deletes our pending Commit and marks the group inactive, but keeps:
  - the live epoch-E `mls_group` (its unconsumed ratchets and exporter secret);
  - the retained parent (epoch E-1; if still at the full tier, it re-derives epoch E from the public Commit);
  - the exporter-secret table.
- **Epoch E+1 is not derivable:** its commit secret comes only through the UpdatePath, which excludes us. Nothing future leaks.
- **But** a stolen store still opens epoch E, including already-read messages when the parent is full.
- `marmot_leave_group` has the same property (pre-existing).
- **Suggestion:** once a removal is final (B1: no competitor could win), delete the retained parent and the live state's secrets. The group record and history can stay. Do the same on leave.

**N2 (Low): a removed member can check less than the others.**
- It cannot verify the confirmation tag or the UpdatePath, and does not run `marmot_commit_authorize`. So an admin can end the group *for you alone* with a removal Commit the others reject, for example with a bad path.
- That requires an admin, who could remove you anyway. But it leaves your leaf in their tree, the same ghost as B1.
- Document it. Mutation L6 (UpdatePath requirement dropped) survives: add a pathless-removal test.

**N3 (Low): tamper-safety of the ended state.**
- The record (`mls_group_removed`: version, remover, epoch) is written only after an authenticated admin removal, in the message's transaction. On load, its length and version are checked.
- A network peer cannot write or clear it. Only a real Welcome clears it (mutation L5 survives: add a re-invite test). Local integrity is the store's job (SQLCipher in Groundhog).
- **One wrong message:** a malformed record makes `marmot_get_group_removal` fail, and Groundhog then reports "You left this group", which is false. Show a neutral "This group ended" for that case.

**N4 (Low): the count-free wording is mostly honest, not entirely.**
- Dropping the number is right: the held type is sealed, so no count of held events is a count of messages.
- But "Waiting for an earlier change to this group" asserts a cause. Junk anyone can post with the public `h` is held too, and shows the banner until `GH_MLS_SERVICE_JUNK_AFTER_COMMITS` (3) fresh Commits arrive, which in a quiet group means indefinitely. The commit message's "true whether … junk" does not hold for junk.
- **Suggestion:** a cause-neutral line, for example "Some messages can't be read yet."
- **Related:** after iihf, `on_group_update` and `keep_backfill` still pass `update->backfill`, not `update->stored`, as the join-second rule's "stored answer" flag. A live event arriving while a page is pending counts as stored.

**N5 (Low): an ended group can re-hold events.**
- `retry_held` detaches the queue. If a removal Commit is applied mid-pass, `group_refresh` → `drop_held` clears it, but the loop then pushes the *remaining* pass entries back (`!group->active`).
- `unreadable` is then non-zero on an ended group until the next refresh. `decrypt-pending` stays false.
- **Fix:** discard the rest of the pass once the group is inactive.

**N6 (Low): the membership backstop is untested and probably unreachable.**
- `group_refresh`'s "an applied Commit that left the account out of the members" (`gh-mls-service.c:884`) marks the group REMOVED in Groundhog only, while libmarmot stays active.
- An applied Commit always contains our own leaf, so this can fire only if `self->account` disagrees with the leaf identity, and then wrongly. G5 (removed) survives.
- Remove it or pin it with a test.

**N7 (Info, pre-existing): relay-transport leaks.** `leaks` on `test-groundhog-mls-service` reports 10,343 leaks (1.28 MB). Every root is in libnostr/libgo: `nostr_filters_new`, `go_context_background` / `go_context_with_cancel`, `nostr_relay_write`, `new_error`, `nostr_connection_write_message`, `go_channel_create`. None come from gh-mls, libmarmot or anything in this diff. Worth a bead.

## Answers to the review questions

**Can a malicious member fake or suppress your removal?**
- **Fake: no** (non-admin). The signature is bound to the committer's leaf, the membership tag to the epoch, and the committer must be an admin (mutations L1/L2 fail the author's test).
- An admin can end it for you alone (N2).
- **Suppress: no**, except by a lower-keyed admin's privileged Commit, which legitimately wins.
- **Delaying:** withholding the Commit only delays the removal. The group then shows the decrypt-pending banner, which is honest in that case.

**A removal in a competing Commit that later loses: un-ended, or wrongly ended?**
- **Wrongly ended, depending on arrival order (B1).**
- If the winner arrives first, the losing removal gets `WRONG_EPOCH` (correct).
- If the removal arrives first, it evicts, and the winner can never be processed. Nothing un-ends it except a new Welcome.

**State leakage after removal (key deletion, retained parent)?**
- Nothing of epoch E+1 is derivable.
- Epoch E stays fully in the store (live state, retained parent, exporter secret) (N1).

**Is the persisted ended state tamper-safe?** Against the network, yes: an authenticated admin removal writes it, and only a real Welcome clears it. Locally it is as safe as the store. A malformed record misreports as "You left" (N3).

**Is the count-free wording honest?** It is more honest than the count, but still names a cause that junk does not have (N4).

**Groundhog lifetime and cancellation.** There is no use-after-free in the diff.
- `retry_held` detaches its queue before calling `process_event`, so `drop_held` inside a nested refresh frees only records already handed back.
- `flush_backfill` holds a ref and checks `active` per event.
- The ended group unsubscribes and refuses sends.
- The one lifetime bug found (C1) is pre-existing. N5 is a state glitch, not a lifetime bug.

**Do the tests fail without the fixes?**

| # | Mutation | Result |
|---|---|---|
| L1 | removal path off | **caught** |
| L2 | removal needs no admin | **caught** (the forged removal is accepted) |
| L3 | our pending Commit never beats a removal | survives (B1) |
| L4 | a previous-epoch removal always evicts | survives (B1) |
| L5 | a Welcome keeps an old removal | survives (N3) |
| L6 | a removal without UpdatePath is accepted | survives (N2) |
| L7 | evict keeps our pending Commit | survives |
| G1 | an ended group keeps its held events | **caught** (`group-lifecycle`: unreadable 4) |
| G2 | no junk memory | **caught** (`decrypt-pending-honest`) |
| G3 | no join-second rule | **caught** (`decrypt-pending-honest`) |
| G4 | iihf reverted | **caught** (`busy-group-stalled-relay`) |
| G5 | membership backstop removed | survives (N6) |
| G6 | removal record ignored (always LEFT) | **caught** (`group-lifecycle`) |

`groundhog-composer /delegates` (8kb2) is the author's routing test. It ran and passed; I did not mutate it.

## Verification

- **Build.** `cmake -S . -B /tmp/rr22-build -G Ninja -DBUILD_GROUNDHOG=ON && ninja` ok.
- **Tests.** `ctest -R 'marmot|mls|groundhog-' -j6`: **97 run; 93 passed, 4 skipped** (`groundhog-launch`, `-store-key-keyring`, `-background-gui`, `-notifier-gui`, as on base).
- **ASAN+UBSAN** (RelWithDebInfo, `-fsanitize=address,undefined -fno-sanitize-recover=undefined -Wno-macro-redefined`, polled synchronously):
  - the same regex: 92 of 93 run tests passed, with the same 4 skipped;
  - `groundhog-mls-ui-gui` aborted on **C1** (`/enrollment`, heap-use-after-free);
  - rerun with `-s /groundhog/mls-ui-gui/enrollment`, its other subtests (`flag-new-group`, `new-group`, `group-info`, which carries the removed-member copy) pass with no reports;
  - no other reports.
- **`leaks --atExit`:**
  - **0 leaks** in `test_commits` (with the B1 repro), `test_protocol`, `test_ratchet_persist`, `test_mls_group`, `test_storage_contract` and `test_marmot_gobject`;
  - `test-groundhog-mls-service`: all 25 subtests ok, with transport-only leaks (N7).
- **Repros and mutations** (throwaway, not committed): the B1 race in both orders, plus the 13 mutations above.

## Recommendation

**REQUEST CHANGES** for **B1**:
- keep an ended-by-removal group able to accept a winning competitor for the removal's epoch, and re-activate on it;
- replay deferred Commits winner-first;
- test both orders, L3 and L4.

**Before the flip:** fix **C1**, a one-line `g_strdup`. It is pre-existing, but it is in the encrypted-group UI.

**Can follow:** N1–N7.

---

## Final pass (2026-09-30): `488eb031`, `ced4d3a6`, `ab223646`, `1d9af674`

**Scope**
- `488eb031`: the wire relay sends nothing on a closed connection (nostrc-2opq).
- `ced4d3a6`: C1.
- `ab223646`: B1, N1 and N3 in libmarmot:
  - the removal record v2 (epoch, flags, committer, digest);
  - a non-final removal still judges its epoch;
  - winner-first deferred replay;
  - a final removal forgets the keys.
- `1d9af674`: B1 in Groundhog (listen while not final; re-activate and resubscribe; the cursor held), plus N4–N6.
- N2 is filed as nostrc-lvdp and N7 as nostrc-jw23.

Scratch worktree: `/tmp/rr22f`, with an ASAN build dir. No code or beads changed.

**Verdict: REQUEST CHANGES.**
- **B1 is closed:** both orders, and L3/L4/L5/L7 are now caught.
- **C1 is closed.**
- **New blocking finding B2:** the finality rule is safe but never becomes true in common groups. There, the removed epoch's keys are never deleted, and Groundhog keeps listening with its cursor pinned at the removal, so it re-fetches all later traffic every session.

### B1: closed (repro rerun)

My throwaway repro (trio: Alice and Bob admins, Bob's key lower; Alice removes Charlie while Bob renames in the same epoch):

| Order at Charlie | Result |
|---|---|
| removal, then Bob's rename | removal → inactive, **not final**; Bob's rename → `MARMOT_RESULT_COMMIT`, **group active again, removal record gone** |
| Bob's rename, then removal | rename applied; removal → `MARMOT_ERR_WRONG_EPOCH`, active |
| both, afterwards | Alice follows Bob's rename; Charlie reads Bob in the canonical epoch; all three exchange messages |

**Mutations (the author's tests alone).** All four W22 survivors are now caught:
- L3 ("the removal waits behind ours");
- L4 ("the losing removal");
- L5 ("re-invited: not removed");
- L7 (our pending Commit dropped).

**New guards:**
- caught:
  - F1, contested ignores admins, so every removal is final ("Bob could still win the epoch");
  - F4, a final removal keeps the exporter secrets ("exporter secret of epoch 2 kept");
  - F5, a winner does not re-activate;
- survive:
  - **F2**, finality ignores a full retained parent;
  - **F3**, deferred Commits replayed in arrival order (my B1 required item 2, implemented but untested);
  - **F6**, a rival removal never replaces the stored one.

**Groundhog:**
- H1 (re-activation without resubscribing) and H2 (not listening while non-final) fail `losing-removal-reactivates`;
- **H3** (the cursor not pinned while contested) survives.

**C1:** `before` is now a `g_strdup` copy. ASAN is clean on `groundhog-mls-ui-gui /enrollment`.

### Finality condition: correct, but not live (B2)

- **Correctness.** `removal_contested()` is yuj2's `could_win` for a privileged key: any admin of the judged state's GroupData with a key at or below the remover's, other than us and the remover's account.
  - Same-account devices of the remover are excluded, as the committer's are elsewhere.
  - For a removal of the current epoch, `removal_final()` also requires the retained parent to be absent or READER, so the Commit that led there can no longer be replaced.
  - **A removal is never final while a lower-keyed admin exists or the parent is full. That is the right safety condition.**
- **Liveness.** Nothing ever re-evaluates it. Finality is computed only in `evict` and when a rival removal replaces the stored one.
  - After eviction the removed member processes no application messages (`USE_AFTER_EVICTION`), so the parent never retires.
  - A lower-keyed admin never stops counting: the removed member can never observe that admin reaching the next epoch.

### B2 (High, blocking): a contested removal stays non-final forever: keys kept, and Groundhog re-fetches everything since the removal

**Verified (throwaway test).** The ordinary case: Alice, the higher-keyed of two admins, removes Charlie with no race. Bob applies it, then the group carries on with 5 renames and 5 messages. At Charlie, after all of that:
- `removal_final` is **still FALSE**;
- the removed epoch's `mls_group` state is **still stored**, so N1 is not achieved in this case;
- all 10 later events return `MARMOT_ERR_USE_AFTER_EVICTION`.

This applies to every group where the remover is not the lowest-keyed admin, and to every removal whose retained parent was still full at eviction. That is not an edge case.

**Groundhog consequence** (`gh-mls-service.c`):
- `listening()` keeps a non-final removal subscribed.
- `process_event` sets `pinned` for every `USE_AFTER_EVICTION` event of it, which is every event of every later epoch.
- `save_cursor` never passes `pinned`. So the persisted cursor stays at the first post-removal event **indefinitely**.
- Every session, reconnect or resubscribe re-fetches and trial-decrypts the group's entire post-removal traffic, growing with the group's activity. Relays keep receiving REQs for the group's `h` from an account that was removed.
- The pin is what H3 removes, and nothing tests it.
- This is the "resubscribe storm" asked about: not repeated subscribes (a re-activation subscribes once), but an unbounded re-fetch per subscription.

**Required.** A path to finality that does not need to read later epochs, plus bounded listening:
1. **Finality that eventually holds.**
   - A winner for the removal's epoch is NIP-44-openable with that epoch's exporter secret, which we keep.
   - Events we cannot open at all are of later epochs: evidence the group moved on.
   - Finalizing after a bounded number of such events (compare `GH_MLS_SERVICE_JUNK_AFTER_COMMITS`) through a libmarmot call that marks the removal final and runs `forget_keys` bounds the exposure. Its residual risk is a winner arriving after the group was already several epochs past it, which the one-epoch horizon refuses for everyone else anyway.
   - Any equivalent epoch-based rule works. A wall-clock bound does not, for the same reasons as yuj2.
2. **Bounded listening.** Do not pin the cursor for events that cannot open under the removed epoch, or cap the pin with the same bound, so that a non-final removal cannot hold the cursor for ever.
3. **Tests:**
   - a contested removal becoming final (and its keys going) under the new rule;
   - Groundhog's cursor, pinned while contested and released once final (H3);
   - F2, F3 and F6.

### Key deletion: complete and safe when it runs

- `forget_keys` deletes, in the removal's transaction:
  - the live `mls_group`;
  - the retained parent;
  - `delete_exporter_secret` for epochs 0 through `group->epoch + 1`.
- The exporter secrets are the ones that open the group's kind:445 envelopes. The last removed epoch is `group->epoch`; `+1` covers a transition interrupted before the record was updated.
- The group record, history and the removal record stay.
- After this, a final removal judges nothing (`USE_AFTER_EVICTION`, tested), and a re-invite's Welcome builds fresh state and clears the record (tested).
- `marmot_leave_group` still keeps its keys, as documented (the member may be re-added).
- **Safe:** nothing reads those records for an ended, final group, and `forget_keys` runs only on final.
- **The gap is B2:** it runs too rarely.

### Other checks

- **Re-activation racing a later Commit.**
  - A winner is applied through `marmot_commit_persist` from the judged base: the current state, or the full retained parent for a parent-epoch winner. The group enters the winner's epoch, active.
  - Commits of the winner's branch then take the normal path. Groundhog resubscribes once, from the held cursor, and orders stored events as usual.
  - A late winner, arriving after the others went two epochs past it, re-activates Charlie on the winner's side. That matches the one-epoch horizon's existing split behaviour for every member (W17), not a new defect.
- **Deferred replay.** `deferred_order` ranks the deferred Commits of the current epoch by their ordering key judged on the current state, removals included (via `removal_key`); the others follow in arrival order. That is correct, but untested (F3).
- **The corrupted record** reads `GH_MLS_GROUP_END_UNKNOWN`, "ended on this device" (N3 closed).
- **N4:** a shown Held counts for 15 minutes at most (`GH_MLS_SERVICE_PENDING_SHOWN_S`), and the join-second rule now uses `update->stored`. Its timer is removed in finalize.
- **N5:** an ended mid-pass group frees the rest of the pass.
- **N6:** the backstop is removed.
- **Resubscribe storms:** none from re-activation (one `group_subscribe`), and a non-final group stays subscribed rather than cycling. The unbounded re-fetch is B2.

### Verification (tip `1d9af674`)

- **Build.** `cmake -S . -B /tmp/rr22f-build -G Ninja -DBUILD_GROUNDHOG=ON && ninja` ok.
- **Tests.** `ctest -R 'marmot|mls|groundhog-' -j6`: **97 run; 93 passed, 4 skipped** (as on base).
- **ASAN+UBSAN** (RelWithDebInfo, `-fsanitize=address,undefined -fno-sanitize-recover=undefined -Wno-macro-redefined`, polled synchronously): the same regex, **93 passed, 4 skipped, 0 reports**. This includes `groundhog-mls-ui-gui` with `/enrollment` (C1) and my repros.
- **`leaks --atExit`:**
  - **0 leaks** in `test_commits` (with the repros), `test_protocol`, `test_ratchet_persist`, `test_mls_group`, `test_storage_contract` and `test_marmot_gobject`;
  - `test-groundhog-mls-service`: 27 subtests ok, and every leak root is in the libnostr/libgo transport (`nostr_filters_new`, `go_context_*`, `nostr_relay_write`, `new_error`, `nostr_connection_write_message`, `nostr_subscription_free_async`), i.e. nostrc-jw23.
- **Repros and mutations** (throwaway, not committed):
  - B1 in both orders, and the never-final removal (B2);
  - mutations L3/L4/L5/L7, F1–F6 and H1–H3, as above.

### Recommendation

**REQUEST CHANGES** for **B2**:
- give a contested removal an epoch-based path to finality, so its keys go;
- bound Groundhog's listening and cursor pin for it;
- test that, and F2, F3, F6 and H3.

B1, C1, N1 (when final), N3–N6: done.
