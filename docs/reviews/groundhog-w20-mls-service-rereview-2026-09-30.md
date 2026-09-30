# Re-review: Groundhog W20, GhMlsService (nostrc-qp24.13 part 1)

- **Branch:** `groundhog/w20-mls-service`, re-reviewed at `27ffdb23`: 11 commits on `origin/master` `90521d74`, which already carries libmarmot 0.10.0.
- **Fix commits:**
  - dc97007c: B1 (cursor), M1 (held queue), M3 (join history), M4 (tests)
  - 03b385d9: M2 (inner `h` tag, per-group seen set)
  - 5ba9adc3: `gh_mls_service_set_admins_async`
  - 302f944f: B2 (account-proof enrollment, identity state, NEEDS_UPDATE)
  - 27ffdb23: enrollment test fix
- **Previous review:** `docs/reviews/groundhog-w20-mls-service-review-2026-09-29.md` (commit `c01fdd01`), REQUEST CHANGES.
- **Reviewer:** independent peer review, as required by AGENTS.md.
- **Date:** 2026-09-30.
- **References:**
  - the Marmot spec `foundation/application-messages.md`, fetched during this review;
  - libmarmot 0.10.0 `credentials.c`;
  - bead nostrc-dha5 (GNostrSubscription's 200-event queue).

## Verdict: **REQUEST CHANGES**

The fixes answer the previous review point by point:

- **B1 and B2 are resolved.**
- **M2, M3 are resolved.**
- **M4 is largely resolved.** The new tests fail on most of the regressions they target.
- **M1 is resolved as specified.** Held events are deduplicated, the oldest is evicted, and a pin keeps the cursor behind it. The queue survives flaps.

The rewritten held-event retry, however, has two new blocking defects. I reproduced both:

- **N1:** `retry_held()` re-enters itself through `after_commit()`. The outer loop then pops an empty queue and dereferences NULL. The service crashes (SIGSEGV) on an ordinary catch-up: a member who missed two Commits, with messages between them, reconnects to a relay that answers newest first.
- **N2:** the new "junk after 3 Commits" rule counts every Commit applied, including those applied out of the queue itself. So a genuine event more than three epochs ahead is discarded before its Commit arrives, Commits included. After a 5-Commit backlog the member is stuck one epoch behind for the rest of the session, and the unreadable count is 0.

Neither reaches users today. The service is only built into the app when `GH_FEATURE_ENCRYPTED_GROUPS` is on (`gh-app-outbox.c:172`). Both are in the code this branch exists to land, and one change to `retry_held()` fixes both.

## Status of the previous findings

| Item | Status | Evidence |
|---|---|---|
| **B1** cursor trusts `created_at` | **Resolved in the code; the test does not pin it (N5)** | Only an admitted message or an applied Commit moves the cursor (`process_event()` `:956-963`). Every candidate is bounded to now + 600 s (`:810`, `:960`). A variant of the test with nothing held passes on the fix and fails with the fix reverted. Residual: N6. |
| **B2** no account proof | **Resolved** | Enrollment at each generation (`identity_enroll()` `:2710`, from `update_activity()` `:3113`). KeyPackage and create wait for it (`:2891`, `:1568`). The signed event is checked (`proof_matches()` `:2653`), and libmarmot re-verifies the binding (`marmot_account_proof_from_signed()`). `MARMOT_ERR_KEY_PACKAGE_IDENTITY` maps to `NEEDS_UPDATE` (`:306`). The suite runs against 0.10.0: the build's `marmot-version.h` says 0.10.0. KeyPackage, create, a second admin's invite and joins all pass. Item 4 (signer copy for kind 450) is not done; see N7. |
| **M1** held queue | **Resolved as specified; see N1, N2** | Dedup by id (`:840`). When full, the oldest goes and the cursor is pinned behind it (`:842-848`). The queue survives flaps and switches (`:3083`). The cursor stays behind held events (`:811-814`). Junk is dropped after 3 Commits (`:870`). |
| **M2** same text in two groups | **Resolved for Groundhog senders and the store** | `["h", nostr_group_id]` on the inner kind 9 (`:1990`). The seen key is scoped by group (`gh-store.c:285`, used by T-admit and T-enqueue, the only two writers). Untagged senders (other clients) still collide in libmarmot's global delivered-id set and the model's rumor index: nostrc-d13t, which is open and says so. |
| **M3** join history | **Resolved** | The Welcome rumor's `created_at` (libmarmot's, fixed at the Add and republished byte for byte) is recorded with the invitation (`welcome_sink()` `:2441-2446`) and used at accept (`:2567-2573`). |
| **M4** tests | **Largely resolved; see N5** | `retry_held()`, the rotation after accept, and `send_resume()` are pinned now (all three survived the first review). Still unpinned: all of B1, the held-event clamp, the eviction pin, and a switch while WAITING. No test backfills across two or more Commits, which is where N1 and N2 live. |
| L1–L6, I1, I2 | Tracked | nostrc-vmcd, -8syq, -ljtd, -374t, -8i0b, -13q5, -b0hm, -xyi1 |

Line numbers are in `gnome/groundhog/src/mls/gh-mls-service.c` at `27ffdb23` unless stated.

## Blocking findings

### N1 (High): `retry_held()` re-enters itself, and the outer loop dereferences NULL

**Where:**
- `retry_held()` at `:862-879` takes `n = g_queue_get_length(&group->held)` once, then pops `n` times.
- A held Commit that becomes readable is applied inside that loop: `process_event()` → `after_commit()` (`:1159-1164`) → `retry_held()` on the same queue. The nested pass reads, re-queues or discards everything left.
- Back in the outer loop, the remaining pops outnumber the items. `g_queue_pop_head()` returns NULL, and `:868` reads `held->id`.
- The loop before dc97007c had the same stale count, but it handed a NULL JSON string on. The `Held *` dereference added in dc97007c makes it a certain SIGSEGV.

**Failure scenario:**
1. Bob is offline while Alice renames the group twice and writes once in each new epoch: c1, m1, c2, m2.
2. Bob reconnects. The group relay answers the stored query newest first, as real relays and `wire-relay.h` do: m2, c2, m1 are held, and c1 is applied.
3. `retry_held()` runs with n = 3. It holds m2 again and applies c2. The nested pass reads m1 and m2 and empties the queue.
4. The outer loop's third pop is NULL, and the process crashes.

The same happens live when one Commit is delayed and the next Commit and a message arrive first. Each event commits its own transaction, so nothing already processed is lost. But a crash before EOSE leaves the cursor where it was, and the next start replays whatever remains of the backlog. That replay can crash again.

**Reproduced:** with temporary tests, not committed and reverted afterwards:
- `x-nested-retry`: c1 withheld, then c2 and a message delivered, then c1 released.
- `x-catch-up-2` and `x-catch-up-5`: a backfill released newest first.

All three abort with rc = 139. lldb shows `EXC_BAD_ACCESS (address=0x0)` at `retry_held + 132`, the `held->id` load.

**Fix:**
- Make the retry non-reentrant. While a retry pass runs, `after_commit()` only marks "again". The pass repeats until one applies no Commit (a fixpoint).
- Prefer taking held events oldest `created_at` first, so an epoch's messages are tried before the Commit that closes it.
- Add a test: a newest-first backfill across two or more Commits, with messages in each epoch, delivers everything. The experiments above have the right shape. The `withhold`/`release` helpers from dc97007c make it a few lines.

### N2 (High): the junk rule discards genuine events of a backlog deeper than three Commits

**Where:** `retry_held()` increments `misses` once per pass (`:869-872`) and discards at three. A pass runs for every Commit applied, including Commits applied out of the queue during the same catch-up. An event of epoch E+k needs k Commits, so it misses k−1 times before it becomes readable. Every event four or more epochs ahead is discarded as junk, Commits included. A junk drop does not pin the cursor.

**Failure scenario:**
1. Bob misses five group changes (c1…c5) with a message after each.
2. On the newest-first backfill (N1 bypassed with a local NULL guard), m1–m3 arrive, and m4, c5 and m5 are discarded.
3. Bob stays at epoch "E4" (Alice is at "E5") with `unreadable = 0`.
4. Every later message and Commit of the group is encrypted under epochs he never reaches. They are held and never read for the rest of the session.
5. A restart usually recovers, because the cursor stayed behind c5. Nothing tells the user in the meantime.

**Measured**, with the NULL guard, newest-first, k Commits behind:

| k | result |
|---|---|
| 2 | all messages read |
| 3 | all messages read |
| 4 | m4 missing for the session |
| 5 | m4 and m5 missing; stuck at E4 |

The rule came from the previous review's M1 suggestion ("after N Commits"), and that suggestion did not anticipate catch-up. That is on the review, but the rule as implemented is wrong.

**Fix:** count a miss only when the fixpoint from N1 ends and the event is still unreadable, once per Commit that arrived from a relay, never per Commit applied out of the queue. A whole backlog then resolves in one fixpoint, and junk still ages out after three new Commits. Alternatively, discard junk only when the group is LIVE and the pass applied nothing. Test with the `x-catch-up-5` shape.

## Findings to fix or track before enabling `GH_FEATURE_ENCRYPTED_GROUPS`

### N3 (Medium): events lost below the service move the cursor past themselves (nostrc-dha5; relay result caps)

At EOSE the cursor jumps to the newest accepted event (`:982-985`). Two things lose events before `process_event()` sees them, and the service cannot tell:

**(a) nostrc-dha5.** GNostrSubscription keeps 200 queued events and drops the oldest. A backfill of more than 200 kind 445s:
- loses events silently;
- still ends with EOSE and LIVE;
- the cursor then moves past the lost events for good.

If a lost event is a Commit, every later-epoch event is held until the cap and then evicted, and the next start replays the same oversized burst. `junk-does-not-evict` already has to inject in batches of 100 to work around this.

**(b) Relay result caps (predates this branch; not in the first review).** `group_subscribe()` sends no `limit` and never pages. Relays still cap a stored answer (strfry's default `maxFilterLimit` is 500) and keep the newest. For MLS the cut-off end is the oldest, and the oldest is where the Commit the member needs next sits. Then:
1. Everything returned is of a later epoch, so it is held (256, then evicted).
2. Nothing is accepted, so the cursor never moves.
3. Every re-subscribe returns the same newest page.
4. A backlog beyond the cap therefore never catches up.

**Judgment on nostrc-dha5:** the service needs no workaround of its own before the dha5 fix lands, on two conditions:
1. **The dependency is explicit:** dha5 blocks enabling `GH_FEATURE_ENCRYPTED_GROUPS`. With the flag off, nothing runs the service. A service-side guard could only guess, because the scope reports no loss.
2. **The fix brings its regression test to this suite:**
   - `junk-does-not-evict` goes back to one unbatched burst;
   - an MLS backfill of more than 200 kind 445s spanning two or more Commits arrives complete (after N1 and N2 are fixed).

(b) needs a change here: EOSE-aware paging in `group_subscribe()`, i.e. a `limit`, then `until` = the oldest seen, until a short page. Track it with the flag-enable work.

### N4 (Medium): every joiner holds the Add Commit, which pins its persisted cursor at the join until three more Commits

**Where:**
- A joined group is read from the Welcome time minus the 600 s overlap (`:2567-2573`, `:1022`).
- The Add Commit that admitted the joiner, and anything else in that pre-join window, fail with `MARMOT_ERR_NIP44` (an epoch the joiner never had) and are held (`:938-940`).
- dc97007c made `save_cursor()` clamp to every held `created_at` (`:811-812`).
- `held-until-commit` records the Add Commit ("What Bob holds already"), and N2's rule is what eventually drops it.

**Scenario:** in a group that sees no later Commit (most small groups):
- the joiner's `unreadable` is at least 1 for good;
- the persisted cursor never passes the join, so every start re-fetches the group's whole history since the join, which runs into N3 as it grows.

Part 2 would show "can't be read yet" on every group the user joined.

**Fix:** events at or before the join floor, the Welcome time M3 now records, cannot be of a later epoch. They should neither be held nor clamp the cursor. Mind the second-granularity tie with the Add Commit.

### N5 (Medium, tests): the B1 test does not test B1, and other fixes are unpinned (M4)

- **`future-replay-moves-no-cursor` passes with the whole B1 fix reverted.** That means the accepted-only gate and both future bounds removed: mutation `ab`. The reason is M1: Bob still holds the Add Commit that admitted him (N4), and `save_cursor()` clamps to it, so the ten-years-ahead cursor never lands. The test fails only once the held clamp goes too (mutation `abc`). The dc97007c message says each new test fails with the corresponding regression; for B1 it does not.
- **A variant that first ages the Add Commit out pins B1.** Three renames, then the forgery, a flap, and the history. It passes on the fix and fails with the fix reverted. Promote it, or assert that nothing is held before the forgery.
- **Also unpinned:**
  - either B1 guard alone;
  - the held-event clamp;
  - the eviction pin;
  - `identity_busy` reset at a switch: no test switches while WAITING. My experiment did; it re-enrolls and publishes.
- **Why the cursor is hard to see in these tests:** they use the real clock, and all their events fall inside the 600 s overlap, so cursor movement is invisible. `world_fake_clock` exists; advancing it past the overlap between steps would expose it.
- **No test backfills across two or more Commits, newest first:** N1 and N2.

## Low and informational

- **N6 (Low): the future-skew allowance cancels the overlap.**
  - `GH_MLS_SERVICE_MAX_FUTURE_SKEW` equals `GH_MLS_SERVICE_CURSOR_OVERLAP` (600 s each).
  - An accepted envelope dated now + 600 is enough. It can be a member's, or anyone's re-signed copy of a genuine envelope the reader has not received yet; MLS does not cover the outer `created_at`. It sets the cursor to now + 600, so the next REQ's `since` is the moment of that save, not 600 s before it.
  - Events made just before that moment and not yet delivered then fall outside: a slow relay, or a republished send.
  - The DM inbox clamps to now before subtracting its skew (`gh-dm-inbox.c:869`). Clamp the MLS cursor to now, or make the overlap exceed the skew.
- **N7 (Low): enrollment UX.**
  - **Re-asking.** `update_activity()` resets DECLINED and FAILED at every generation (`:3111-3112`), and a network reconnect is a generation (nostrc-13q5). So a user who declined the kind-450 request is asked again at every suspend/resume or network change, and a reconnect while WAITING cancels the request and sends a new one. This is not a loop: the retry timer's `resume_all()` never calls `identity_enroll()`. But with an approval or NIP-46 signer it is a prompt per reconnect. Re-ask only when the account (re)activates, and give part 2 an explicit "try again".
  - **Silent state changes.** `stop_generation()` (`:3070-3071`) and `update_activity()` (`:3111-3112`) assign `self->identity` without `notify::identity-state`, so a bound UI keeps "Waiting for approval" while offline or switched away.
  - **Signer copy (previous B2 item 4) is not done.** No signer names kind 450. libmarmot does not store the instance key, so an approval-gated signer shows a bare kind-450 request at every launch. Track this for part 2, or ask libmarmot to persist the instance key.
- **N8 (Info):**
  - **Version fallback.** `gh-mls-service.h:5-9` quietly compiles the proof out when the generated `marmot-version.h` is not on the include path. On master only the 0.10 path is live. The tests would notice (no KeyPackage), but an `#error` is cheaper.
  - **Declined invitations.** They keep their welcome-time cursor row (`:2441-2446`; `gh_mls_service_decline_invite()` does not clear it). The row is local-only and small.
  - **`proof_matches()`.** It compares kind, pubkey and content, not tags or `created_at`. That is fine: `marmot_account_proof_from_signed()` re-verifies the binding to the account and the instance key, which is also why disabling `proof_matches()` survives the tests.

## The questions asked

### The enrollment flow

- **Signer prompt loop: none.**
  - `identity_enroll()` is called only from `update_activity()`, once per generation. It returns while a request is in flight (`identity_busy`) or once enrolled, and it skips the signer when this process's instance already holds a proof (`marmot_has_account_proof()`), so a flap after enrollment asks nothing.
  - `identity_signed()` then triggers one `key_package_maybe_publish()`.
  - The 15 s–10 min retry timer (`resume_all()`) never touches enrollment.
  - The remaining repeat prompts are N7 (one per reconnect after a decline) and the KeyPackage path's own L1 (nostrc-vmcd).
- **Account-switch cancellation: correct.**
  - `stop_generation()` bumps `run`, cancels the generation's cancellable (the signer call is bound to it), clears `identity_busy`, and turns WAITING into NONE.
  - A late answer fails the `run` check.
  - A signature by another key also fails `proof_matches()`, and libmarmot's own check.
  - Switching back asks again.
  - Experiment: hold the signer, restart, switch to another account and back, release. It ends ENROLLED with a KeyPackage published.
  - Two gaps: no test covers a switch while WAITING (mutation `t` survives), and the state change is not notified (N7).
- **Kind 450 is never published.**
  - The template goes only to the account signer and then to `marmot_set_account_proof()`; it has no path to `gh_relay_publish_*`.
  - `account-proof-enrollment` asserts that none of the four test relays ever stored a kind 450.
  - With a NIP-46 signer the request travels NIP-44-encrypted in a kind 24133 to the bunker, like any signature request.
  - The proof's signature does leave the device inside the member's KeyPackage and leaf. That is the point of the proof, not a leak.

### The inner `h` tag

- **Spec.** `foundation/application-messages.md`: "Decoders do not police inner tag names. Tags carry application content; the active transport binding builds the outer envelope's routing tags from group state, never from the inner event, so an inner tag never affects delivery." The tag is allowed.
  - Groundhog rightly does not require it on receipt; that would refuse other clients' untagged messages.
  - libmarmot builds the outer `h` from group state (`messages.c` routing), not from the inner event.
- **Leak: none to relays.**
  - The tag is inside the MLS application message, inside the kind-445 NIP-44 envelope.
  - Its value is the `nostr_group_id`, which the outer kind 445 already carries in its public `h`.
  - It is not the MLS group id.
  - Members already know it. On disk it sits in the encrypted store's `raw_json`.
  - For part 2: a message forwarded or quoted verbatim out of the group would carry the routing id. Nothing does that today.
- **What it fixes:** Groundhog-to-Groundhog collisions end to end, plus any sender in the store (per-group seen key). The global rumor index in libmarmot and the model is nostrc-d13t.

### Cursor clamping edge cases

These hold:
- rejected, duplicate and own-echo envelopes move nothing;
- every candidate is bounded to now + 600;
- held and evicted events keep the cursor behind them;
- the Welcome time is clamped to [1, now];
- the cursor only moves forward.

Edge cases found:
- **N6:** skew equals overlap.
- **N4:** the joiner's Add Commit pins the cursor.
- **N3:** a lost or truncated backfill is passed at EOSE.
- **The eviction pin is session-only and never clears.** It is not persisted, but the cursor it bounded is. After one overflow, the cursor cannot pass the oldest evicted event until restart, and the restart refetches from there. That is the intent, and it is bounded.
- **In-session recovery of evicted events needs a restart.** Re-subscribing from the pin once the queue drains would recover them sooner.
- **A failed relay counts as settled at EOSE (`:982` counts both EOSE and failure).** The cursor then moves while one group relay is down, and what only that relay had is skipped. This predates the branch, and 600 s of overlap does not cover an outage.

### The held-queue 3-Commit junk rule

- Deduplication, evict-oldest, and "survives flaps" are right and tested: mutations d, e and f are caught.
- The rule itself is wrong as implemented: it counts per Commit applied, not per new Commit (N2).
- It shares a loop with the re-entrancy crash (N1).
- Junk dropped by the rule does not pin the cursor. That is right for junk, but with N2 it also applies to genuine events.

## Verification performed

| Check | Result |
|---|---|
| `git submodule update --init third_party/nostrdb third_party/nsync` | ok |
| `cmake -S . -B /tmp/w20mr2 -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w20mr2` | Builds clean against libmarmot 0.10.0: the generated `marmot-version.h` says 0.10.0, so `GH_MLS_SERVICE_ACCOUNT_PROOF` = 1. The build rewrote `gnostr-profile-edit.ui`; I restored it. |
| `ctest --test-dir /tmp/w20mr2 -R 'groundhog-\|marmot\|mls' -j6` | **94/94 passed**, 4 skipped as on base: launch, store-key-keyring, background-gui, notifier-gui. Includes `groundhog-mls-service` (2.6 s), `-privacy-mls`, `-store`, `-store-marmot`, `marmot_test_*`. Re-run after all experiments: 27/27 of the MLS, store and marmot subset. |
| `python3 scripts/check-unsequenced-args.py` | clean |
| `scripts/linux-gate.sh` (arm64, GCC, Ubuntu 24.04), on a clean snapshot of the tree | Build ok. Smoke: 419 run, all pass. The MLS suites are not excluded. |
| Experiments: temporary tests in `test_mls_service.c`, reverted | **N1:** `x-nested-retry`, `x-catch-up-2` and `x-catch-up-5` die with SIGSEGV; lldb puts it at `retry_held + 132`. **N2:** with a local NULL guard, the depth table above. **B1:** the nothing-held variant passes on the fix and fails with it reverted. **Enrollment:** a switch while WAITING re-enrolls and publishes; a decline is re-asked on reconnect (N7). |
| Mutations: each applied, test binaries rebuilt, `groundhog-mls-service` and `-privacy-mls` run (plus `-store` for store mutations), then restored | table below |

| Mutation | Result |
|---|---|
| B1: rejected envelopes move the cursor | survives |
| B1: no future bound in `process_event()` / anywhere | survives / survives |
| **B1: the whole fix reverted (both guards)** | **survives**: masked by the held Add Commit (N5) |
| B1 reverted and the held clamp removed | caught: future-replay-moves-no-cursor |
| M1: `save_cursor()` ignores held events | survives |
| M1: eviction pin removed | survives |
| M1: no dedup of held ids | caught: held-until-commit |
| M1: a flap clears the held queue | caught: held-until-commit |
| M1: a full queue drops the new arrival | caught: junk-does-not-evict |
| M1: junk never dropped | caught: junk-does-not-evict |
| `retry_held()` a no-op | caught: held-until-commit (survived in the first review) |
| no KeyPackage rotation after accept | caught: group-lifecycle (survived before) |
| `send_resume()` a no-op | caught: send-republished-after-restart (survived before) |
| M3: accept ignores the Welcome time / the time is never recorded | caught / caught: join-reads-from-welcome |
| M2: no inner `h` tag | caught: same-text-two-groups |
| M2: seen key not scoped by group | caught: store mls-seen-per-group |
| B2: create without the identity gate | caught: account-proof-enrollment |
| B2: `NEEDS_UPDATE` mapping removed | caught: unproven-invitee-needs-update |
| B2: KeyPackage without the identity gate | survives (libmarmot refuses an unproven KeyPackage itself: defence in depth) |
| B2: `proof_matches()` skipped | survives (libmarmot re-verifies) |
| `identity_busy` not reset at a switch | survives (no switch-while-WAITING test) |
| DECLINED kept across generations | survives (no test either way) |

## What was checked and is correct

- **Cursor rules (B1).**
  - `EventOutcome` makes "held", "accepted" and "other" explicit.
  - A retried event is never re-held by `process_event()`; the caller decides.
  - The bound is applied both where `newest` is taken and in `save_cursor()`.
  - The cursor only moves forward.
- **Held queue (M1).**
  - `held_ids` owns nothing, and every path that pops a record removes its id.
  - Eviction pins at the evicted `created_at`, which is already bounded.
  - The queue outlives generations but not the group object; it is freed in finalize.
- **Account proof (B2).**
  - The job holds a weak ref and the run number.
  - The signer call is bound to the generation's cancellable.
  - CANCELLED maps to NONE, not DECLINED.
  - The template is freed on every path.
  - KeyPackage and create wait for the proof, and create reports `NOT_ENROLLED` synchronously with honest copy.
  - Against 0.9 the state is `NOT_REQUIRED` and nothing changes.
  - `NEEDS_UPDATE` covers KeyPackages, Commits and Welcome trees alike through `marmot_fail()`.
- **Admins (5ba9adc3).**
  - `check_change()` requires an active group and the caller to be an admin.
  - New admins must be current members (lowercased, unique, 1–1000).
  - The change goes through the same staged, published-then-merged Commit as every other.
  - Tested, including a non-member being refused and a second admin's invite with 0.10.0 proofs.
- **Per-group seen set (M2).**
  - The key is domain-separated; T-admit and T-enqueue are the only writers; the id-only pre-check now defers to T-admit (same transaction).
  - MLS has not shipped, so no stored rows need migrating.
- **Welcome time (M3).**
  - It is recorded in the same transaction as the Welcome's receipt and its wrap's seen mark.
  - It is clamped to [1, now] and cleared at accept.
  - It falls back to the old window when absent.
- **Test fixtures.** `wire-relay.h` `withhold`/`release`/`withhold_new` change nothing unless a test asks, and the relay already answers stored queries newest first, as real relays do.

## Required before approval

1. **N1:** make the held-event retry non-reentrant (a fixpoint), preferably oldest first.
2. **N2:** count junk misses per Commit that arrives from a relay, never per Commit applied out of the queue.
3. A test for both: a newest-first backfill across at least four Commits, with a message in each epoch, delivers everything and ends at the right epoch.
4. **N5:** make the B1 test test B1 (nothing held before the forgery). A fake-clock test for the held clamp and the pin is recommended.

Before enabling `GH_FEATURE_ENCRYPTED_GROUPS`, fix or file beads for:
- **N3:** nostrc-dha5 as a blocker, its MLS regression test, and EOSE-aware paging;
- **N4**;
- **N6**;
- **N7**.

No service-side workaround is needed for nostrc-dha5 beyond that dependency.

**REQUEST CHANGES**

---

## Final pass: `0e8efe19` (b674059e, 0e8efe19)

The review branch was rebased onto `0e8efe19`.

**Verification:**

| Check | Result |
|---|---|
| Build (`/tmp/w20mr2`, libmarmot 0.10.0) | clean; `gnostr-profile-edit.ui` restored |
| `ctest -R 'groundhog-\|marmot\|mls' -j6` | 94/94 passed, 4 skipped as on base |
| `check-unsequenced-args.py` | clean |
| `scripts/linux-gate.sh` (arm64, GCC) on a clean snapshot | build ok; 419 smoke tests pass |

**My re-review repros**, re-inserted unchanged (no NULL guard) and reverted afterwards:

| Repro | Result |
|---|---|
| `x-nested-retry` | passes (it crashed at `27ffdb23`) |
| `x-catch-up-2`, `-4`, `-5`, `-8` (newest-first backlog, first Commit last) | every message read; final epoch reached; `unreadable = 0` |

**Mutations** (applied, rebuilt, run, restored):

| Mutation | Result |
|---|---|
| whole B1 fix reverted | caught: future-replay-moves-no-cursor (N5 closed) |
| `retry_held()` recursive again (no `retrying` guard) | caught: catch-up-4-commits |
| a junk miss per queued Commit (N2 reverted) | caught: catch-up-4-commits |
| the floor ignored in `save_cursor()` / the floor removed entirely | caught / caught: join-commit-pins-no-cursor |
| the floor's "drop before the join" removed alone | survives (the `save_cursor()` exclusion covers the test) |
| the failed-relay gate removed | caught: failed-relay-holds-the-cursor |
| N7: a reconnect resets DECLINED | caught: account-proof-enrollment |
| N7: a reconnect cancels a WAITING request | survives (no test flaps while the signer waits) |
| N6: the +600 s allowance restored | survives (tests assert `cursor <= now` only on the B1 path) |

### Item by item

- **N1: closed.** `retry_held()` is a fixpoint with a `retrying` guard. A nested call only sets `retry_again`, and the pass list is detached, so the loop never walks what a nested call freed. Each pass sorts oldest first.
  - **It terminates.** Another pass runs only when a Commit was applied out of the queue during the previous one. That Commit is freed, so every repeat strictly shrinks the finite queue. Nothing is added during a pass: `process_event(…, retry)` never holds, and relay callbacks are asynchronous. The bound is |held| + 1 passes.
  - While a pass has the list detached, `save_cursor()` sees only re-queued items. That is safe because the pass is oldest first: an accepted event's date is ≤ every item still waiting in the pass.
- **N2: closed.** Misses are added once after the fixpoint, one per fresh Commit (from a relay, or our own merged). Commits applied out of the queue don't count. A backlog of any depth resolves in one call.
  - Residual: a message four or more epochs ahead, whose three intermediate Commits arrive *separately and live*, still ages out. That is rare, and bounded by the session.
- **N4: closed, with a Low residual.**
  - A held event dated before the join floor is dropped. One in the join's own second is kept but never holds the cursor back.
  - A legitimate event is affected only if it is (a) undecryptable on arrival (a later epoch than the joiner's current one) and (b) dated before the floor.
  - The floor is the inviter's rumor time, clamped to the joiner's now. So (b) needs an inviter clock ahead of another member's by more than the time from the join to the next Commit.
  - If that happens, the event is dropped unpinned, and a newer accepted event can then move the cursor past it.
  - Suggest a small margin: drop only below floor − 60 s, and exclude [floor − 60 s, floor] from the clamp. Low; for the paging work.
- **N5: closed.** The test ages the Add Commit out, asserts `unreadable == 0`, and asserts the cursor directly. It fails on the B1 revert.
- **N6: closed.** The cursor and held dates are bounded by now, and `GH_MLS_SERVICE_MAX_FUTURE_SKEW` is gone, so an accepted future-dated event leaves the full 600 s overlap. Only the re-added allowance is unpinned (above).
- **Failed-relay rule: correct, but not bounded in time.**
  - The cursor moves only while every group relay is EOSE-and-connected (1) or could never be subscribed (3).
  - It does not stall delivery: live events from the other relays are still read.
  - But a relay that stays down freezes the cursor for as long as it is down, across restarts. Every start then refetches everything since the relay died. That makes nostrc-cpwf's paging carry the load, and it makes a relay-cap truncation (N3b) likelier after an offline period.
  - **Pre-existing sibling:** a group listing more than 16 relays, possible when another client made it (`group_refresh()` takes libmarmot's list uncapped), never reaches LIVE. Relays past the 16th never enter `settled`, so its cursor never moves.
  - Neither needs to block this branch. Both should be tracked before the flag: let a relay that has failed for longer than a bound (say 24 h, or N consecutive subscriptions) stop holding the cursor; and cap or subscribe the relay list consistently.
- **N7: closed.**
  - The proof request runs under `identity_cancellable`, per *account* generation (`identity_new_generation()`). A flap neither cancels it nor resets DECLINED; a switch or reactivation cancels it and resets to NONE, with notify, so the new generation asks once.
  - Late answers are matched by `identity_generation`. An answer that arrives while offline enrolls, and the KeyPackage follows on reconnect via `resume_all()`.
  - `gh_mls_service_retry_identity()` gives part 2 its "Try Again".
  - Dispose cancels.
  - Residual (Low): a signer *transport* failure is also recorded as DECLINED, and after this change it waits for "Try Again" rather than the next reconnect. Distinguishing a denial from a failure (FAILED → re-ask on reconnect) would be kinder to NIP-46 users.
- **N3: recorded correctly.** nostrc-9xf5 depends on nostrc-cpwf, nostrc-dha5 and nostrc-kzun (`blocks`), and its notes say not to set the flag until all three close. `gh-features.h` names the three next to `GH_FEATURE_ENCRYPTED_GROUPS 0`.

### Follow-ups (non-blocking; file before enabling the flag)

1. Bound how long a failed group relay can hold the cursor, and handle groups with more than 16 relays (Medium; fits nostrc-cpwf or a sibling bead).
2. Add a clock-skew margin to the N4 join floor (Low).
3. Tell a signer failure apart from a user's decline (Low).
4. Tests: a flap while WAITING; the future bound on the non-B1 path (Info).

The blocking findings N1 and N2 are fixed, and each fix is pinned by a test that fails on its revert. N4–N7 are fixed, and N3 is gated on the right beads.

**APPROVED**
