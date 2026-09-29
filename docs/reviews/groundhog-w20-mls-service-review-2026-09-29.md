# Review: Groundhog W20, GhMlsService (nostrc-qp24.13 part 1)

- **Branch:** `groundhog/w20-mls-service`, reviewed at `b3676b23`: 6 commits on `a15489c9`.
- **Commits:**
  - e6aeff41: libmarmot `marmot_get_group_members()`
  - 44952498: plumbing
  - 33f4c56c: GhMlsService
  - f692de9a, f517d4cc, b3676b23: test fixes
- **Reviewer:** independent peer review, as required by AGENTS.md.
- **Date:** 2026-09-29.
- **References:**
  - the privacy charter `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`: §2.2, §3.9 D5, §4.3, §4.4, PD-8/PT-8, G23
  - the libmarmot README changelog, 0.5.0–0.9.0
  - the Marmot spec `transports/nostr.md`, fetched 2026-09-29

## Verdict: **REQUEST CHANGES**

The architecture is sound. There is one Marmot instance per account store, and every call runs on the main context. All MLS state lives in GhStoreMarmot, and every multi-record change is wrapped in one store transaction. The privacy boundaries are enforced and tested:

- consent before any KeyPackage lookup;
- Welcomes only as gift wraps, and only to the invitee's 10050;
- kind 445 only under fresh keys, with no account AUTH on group relays.

Two findings block, and four should be fixed or tracked before the feature flag is turned on:

- **B1:** a hostile relay, any outsider using a relay that accepts future timestamps, or any member can permanently silence a group by pushing its read cursor into the future.
- **B2:** GhMlsService does not work with libmarmot 0.10.0, which lands alongside it. Merged with `marmot/w20-identity-parent`, the service cannot create a KeyPackage. I verified this.

## Verification performed

| Check | Result |
|---|---|
| `git submodule update --init third_party/nostrdb third_party/nsync` | ok |
| `cmake -S . -B /tmp/w20mr -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w20mr` | builds clean; the build rewrote `gnostr-profile-edit.ui`, which I restored |
| `ctest --test-dir /tmp/w20mr -R 'groundhog-\|marmot\|mls' -j6` | 93/94 passed; 4 skipped (as on base). `groundhog-relay-wire` aborted in `/tor/groundhog/relay/offline-at-start`: `soup_server_listen_local` could not re-bind its port under `-j6`. It passes 3/3 when run alone, so this is port contention, not this branch (the branch's `wire-relay.h` change only adds `close_on_event` handling to `serve`). `marmot_test_commits` (with the new `test_group_members_follow_the_epoch`), `groundhog-mls-service`, `groundhog-privacy-mls`, `groundhog-store-marmot`, `groundhog-conversations`, `groundhog-dm-inbox`, `groundhog-composer` and `groundhog-privacy-summary` pass. |
| `python3 scripts/check-unsequenced-args.py` | clean |
| `scripts/linux-gate.sh` (arm64, GCC, Ubuntu 24.04) | build ok. Smoke: 419 run, all pass. `test_nostr_gtk_bind_latency_budget` and `groundhog-background` failed in the parallel run and passed alone; both are unrelated. |
| Mutation: `retry_held()` made a no-op | **both MLS suites still pass** (M4) |
| Mutation: no KeyPackage rotation after accept, and `send_resume()` returns at once | **both MLS suites still pass** (M4) |
| Trial merge with `marmot/w20-identity-parent` (libmarmot 0.10.0), then `groundhog-mls-service` | **fails**: `Groundhog could not make a KeyPackage: Making a KeyPackage: key package identity mismatch`, then `/groundhog/mls-service/key-packages` times out (B2). The merge was aborted and the tree restored. The only textual conflict is the RUN list in `libmarmot/tests/test_commits.c`, which is trivial. |

## Blocking findings

### B1 (High): the group read cursor trusts relay-supplied `created_at` without a bound, so one event can silence a group permanently

**Where:**
- `gnome/groundhog/src/mls/gh-mls-service.c:825-826`: `group->newest` takes every non-held event's `created_at`, including events libmarmot rejected.
- `:837-838`: in the LIVE state, every non-held event saves the cursor.
- `:859`: at EOSE the cursor is set to `newest`.
- `save_cursor()` at `:731-742` has no upper bound.
- `group_subscribe()` at `:896-897` then uses `since = cursor - 600`.

**Failure scenario:**
1. Anyone who can read a group relay copies any kind 445 of the group, re-signs its unchanged content with a fresh key and `created_at = now + 10 years`, and publishes it. The group's `#h` is public, and libmarmot 0.8.0 names exactly this re-wrap as possible.
2. libmarmot decrypts the envelope (the exporter-secret layer is intact) and rejects the replay with `MARMOT_ERR_MLS` (generation already consumed) or reports `MARMOT_RESULT_OWN_MESSAGE`. Neither is `MARMOT_ERR_NIP44`, so the event is not held.
3. Its `created_at` goes into `newest` and the cursor.
4. From the next re-subscribe on (restart, network change, account switch, or a relay-set change by Commit), the REQ carries `since ≈ 2036`. Relays apply `since` to live events too, so the group never delivers another event: not history, and not live.

Other ways in:
- A member can do the same with a valid message dated in the future.
- A malicious group relay needs nothing more than a replay.

The charter lists A1 relay operators as able to "replay, inject".

Other readers already guard against this:
- `gh-dm-inbox.c:869` uses `MIN(checkpoint, now) - GH_DM_INBOX_WRAP_SKEW`.
- `gh-nip29-service.c:978` uses `MIN(created_at, now + GH_NIP29_MAX_FUTURE_SKEW_SECONDS)`.

The Marmot transport binding says `since` "is a fetch hint only".

**Fix:**
- Bound every cursor candidate to `now + skew`, as NIP-29 does.
- Advance `newest` and the cursor only for events libmarmot accepted: an application message admitted, or a Commit applied. Rejected, duplicate and garbage envelopes must not move it.
- Add a test: a far-future replay followed by a restart still receives the next message.

### B2 (High, blocks landing alongside libmarmot 0.10.0): GhMlsService never enrolls the account-identity proof

**Where:**
- `gnome/groundhog/src/mls/gh-mls-service.c:2591` calls `marmot_create_key_package_unsigned()`.
- `:1387` calls `marmot_create_group()`.
- `gh_mls_service_new()` at `:2830-2838` calls `marmot_new()`, but nothing ever calls `marmot_account_proof_template()` or `marmot_set_account_proof()`.

**Failure scenario:**
In 0.10.0 (`31767a66`, nostrc-7vyi), `marmot_create_key_package_unsigned()` returns `MARMOT_ERR_KEY_PACKAGE_IDENTITY` when the instance holds no account proof. Once both branches are on master:

- The service can never publish a KeyPackage. Its state goes to FAILED; I reproduced this.
- Nobody can invite a Groundhog user.
- Groups the account creates start with an unproven creator leaf, so joiners accept that leaf only in Welcomes the creator itself sends ("only the creator could admit members"). A second admin's invitations fail.
- Invitees whose KeyPackages carry no proof (0.9.0 and older clients) fail the Add with `MARMOT_ERR_KEY_PACKAGE_IDENTITY`. `marmot_fail()` at `:247-267` maps that to a generic `GH_MLS_COMMIT_ERROR`, not to something like "this person needs to update".

The identity commit's message says "Groundhog needs a rebuild only". That is true of Groundhog before this branch, but not of GhMlsService.

**Required change:**
1. On every service start, enroll per generation. The instance key is not stored, so re-enroll after every `marmot_new()`:
   - call `marmot_account_proof_template(account)`;
   - have the account signer sign it with `gh_account_controller_sign_with_cancellable_async`, bound to the generation;
   - check that the result is the template's kind 450 by the account;
   - call `marmot_set_account_proof()`.
2. Gate `key_package_maybe_publish()` and `create_group_now()` on `marmot_has_account_proof()`.
3. Never publish the template.
4. Consider signer-UX copy for the kind-450 request.
5. Map `MARMOT_ERR_KEY_PACKAGE_IDENTITY` on Add to `NO_KEY_PACKAGE` or a new "needs an update" code.
6. Add a test that runs the service against 0.10.0: KeyPackage, create, a second admin's invite, and a join.

The retained-parent retirement (`ddc9dad1`, nostrc-yuj2) needs no service change. GhStoreMarmot stores `mls_group_parent` opaquely, and retirement happens inside `marmot_process_message()`.

## Findings to fix or track before enabling

### M1 (Medium): the future-epoch hold queue can be filled by anyone, drops overflow silently, has no dedup, and is lost while the cursor moves past it

**Where:**
- `gh-mls-service.c:816-818`: every `MARMOT_ERR_NIP44` is held. That includes events that are simply not for us: any validly signed kind 445 carrying the group's public `h` tag.
- `:831-835`: at 256 the event is dropped without trace. The `unreadable` notification still fires.
- `:749-759`: no dedup by event id. A re-subscribe re-fetches the last 600 s and re-queues held events.
- `:2757`: every network flap or generation stop clears the queue.

**Failure scenario:**
1. An outsider publishes 256 junk kind 445s with the group's `h`. They fill Alice's queue for the session.
2. After that, every genuine future-epoch message is dropped. This happens routinely in a backfill, because relays return newest first, so a Commit's later-epoch messages arrive before the Commit.
3. The Commit is then processed and advances `newest` and the cursor past the dropped messages.
4. After a restart the dropped messages fall outside `since` and are never fetched again. A network flap between "held" and "Commit arrives" loses them the same way.

Each later Commit also re-runs up to 256 junk events, each through a store transaction, a reconcile and trial decryption.

**Fix:**
- Dedup held events by id.
- Keep the cursor at or below the oldest held `created_at` (or persist the held ids), so that anything dropped or lost is fetched again.
- Prefer evicting junk (the oldest, or events whose trial decryption fails against every known epoch after N Commits) over dropping new arrivals.
- Test it (see M4).

### M2 (Medium): identical messages in two groups collide, so the second is silently dropped

**Where:**
- `gh-mls-service.c:1808-1815`: the inner kind 9 has only `pubkey`, `created_at`, `content` and no tags.
- libmarmot 0.9.0 keeps its inner-event duplicate markers in `mls_processed_messages` by inner id alone (`libmarmot/src/messages.c:1031-1037`, `1184-1186`).
- The store's `seen` namespace `GH_STORE_SEEN_MLS_MESSAGE` (`gh-store-mls.c:216`) is also keyed only by the inner id.

**Failure scenario:** Alice posts "ok" to groups A and B within the same second. Bob, a member of both, receives the second as `MARMOT_RESULT_OWN_MESSAGE` (a "duplicate"): it is never stored or shown, and nothing reports it.

**Fix:** bind the inner event to its group, for example with an `["h", <nostr_group_id>]` tag checked on receive, or scope the duplicate markers by group in libmarmot. Scoping by group is the more robust fix; file it against libmarmot as well.

### M3 (Medium): a join backfills from accept time, not from the Welcome

**Where:** `gh-mls-service.c:2384-2385` sets `since = now - 2 d` when the invitation is accepted.

**Failure scenario:** Bob accepts an invitation that arrived four days ago. The members' messages from days 0–2 of his epoch are readable by him and still on the relays, but they are never fetched.

**Fix:** use the Welcome rumor's `created_at` (or the Add Commit's), minus the overlap, bounded as in B1.

### M4 (Medium): the tests don't pin several advertised guarantees

Mutations survive both suites:
- `retry_held()` as a no-op: held events are never retried after a Commit;
- no KeyPackage rotation after a Welcome is accepted;
- `send_resume()` disabled: sends are never republished after a restart.

Also untested:
- that a crash between the send transaction and the publish republishes the same kind 445;
- a Welcome republished byte for byte after a restart;
- the 256 cap;
- cursor behaviour (B1, M1, M3).

`test_restart_mid_commit` (`tests/mls/test_mls_service.c:376-381`) says "Exactly one Add Commit was ever published", but it only asserts that the staged id is among those published. A rebuilt second Commit would pass. Assert the count too.

## Low and informational

- **L1 (Low): each KeyPackage publish retry mints a new KeyPackage and asks the signer again.**
  - Where: `:2501-2503` schedules a retry, and `resume_all()` → `key_package_maybe_publish()` at `:2586-2613` makes a new key.
  - Scenario: a write relay that refuses (paid relay, auth-required) produces a signer request every 15 s … 10 min. With a NIP-46 or approval signer that is a prompt storm.
  - Each attempt also stores another `kp_priv`/`kp_full` pair. libmarmot deactivates but never deletes them, and never deletes a consumed `kp_priv` after a join either (`welcome.c:425-470`), so private KeyPackage material accumulates.
  - Fix: republish the already-signed event instead of minting a new one. Track deletion of spent or rotated private material in libmarmot (MIP-00 hygiene).
- **L2 (Low): a relay-list change doesn't republish the KeyPackage.**
  - Where: `on_relays_changed()` at `:2806` → `key_package_maybe_publish()` returns PUBLISHED at `:2581-2583` while the cursor is younger than 28 days.
  - Scenario: after the user changes 10002/10050, the KeyPackage stays missing from the new write relays for up to 28 days, and inviters get "hasn't set up encrypted groups".
- **L3 (Low): a Commit round that can't start is neither completed nor retried.**
  - Where: `round_start()` at `:1118-1121` (publish creation fails) and `:1135-1138` (no URL added) frees the round without `schedule_retry()` and leaves `group->waiters` pending.
  - Scenario: a group whose stored relay list has no usable URL; the caller's GTask never completes until the next generation change.
- **L4 (Low): `welcome_sink()` doesn't check whose Welcome it is.**
  - Where: `:2239-2274` checks neither `welcome->account_pubkey == self->account` nor `running()`.
  - Scenario: an inbox that has already switched accounts while the old store is still open would record another account's Welcome as failed in this store. That other account is unharmed, because its wrap is not marked seen in its own store, but the check is one line.
- **L5 (Low): the inner `created_at` is unbounded** (`gh-message.c:593-640`). A member's message dated years ahead sorts above everything in the room forever. Bound it as NIP-29 does (`GH_NIP29_MAX_FUTURE_SKEW_SECONDS`).
- **L6 (Low): a network flap reports as an account change, and the caller's cancellable is ignored.**
  - `update_activity()` treats a network flap as a generation stop. In-flight create/add/remove calls fail with "The account changed; …" (`:2747-2749`), which is misleading.
  - KeyPackage lookups use `self->cancellable` (`:1326-1328`), so cancelling the caller's `GCancellable` does not stop a create or add in progress.
- **I1 (Info): Tor isolation for publishes.** Kind 445 publishes (Commits and sends) use GhRelayPublish's random per-publish label, not the charter §4.3 `acct/mls/<hash(group)>` label that the subscription uses. That is stricter (no linkage) and fine for privacy, but either the charter row or a comment should say so.
- **I2 (Info): KeyPackage relays.** KeyPackages go to the 10002 write relays and the 10050 inbox. The Marmot binding needs only the write set, so the extra 10050 copy is harmless, and lookups use discovery relays plus 10002 write relays, as specified. Welcomes go to the invitee's 10050 only, which matches the spec and the charter.
- **I3 (Info): invitations from anyone.** Welcomes from anyone are stored as pending invitations and emit `invite-received`. Nothing is fetched and nothing is joined without the user, which is fine. The part-2 UI must still treat invitations from non-contacts as requests (PD-8: hidden notifications, no profile fetch).
- **I4 (Info): the `groundhog-relay-wire` flake.** It fails under parallel ctest (port re-bind in `wire-relay.h:568`). This predates the branch; worth a bead.

## What was checked and is correct

- **Thread safety.** There is one `Marmot` per account store, created in `gh_mls_service_new()`, and every libmarmot call is synchronous on the main context. No marmot-gobject client shares the storage, and no other `gh_store_marmot_new()` caller exists, so the README 0.5.0 contract ("serialize every call on one instance") holds without the gobject lock. `gh_mls_service_get_marmot()` is documented "one thread".
- **Atomicity.**
  - Receives: `process_event()` wraps `marmot_process_message()` (the relay path; the id and signature are verified first) and the T-admit in one outer transaction, and rolls back when admission fails.
  - Sends: `gh_mls_service_send()` puts the outbox row, `marmot_create_message()` (the ratchet step) and the sealed kind 445 in one transaction, committed before any publish (README 0.9.0 N1).
  - Also single transactions: group creation plus room row; a Welcome's mark-sent plus outbox settle; a Welcome's receipt plus its wrap marked seen.
- **Commits.** Commits are staged through gh-mls-commits (T-mls), merged only on a relay OK, and republished from the stored bytes after a restart (the test's `relay_stored` wait fails if they are rebuilt). No Welcome is sent before the merge: the restart test asserts `invites == 0`.
- **Privacy.**
  - Consent: checked for every invitee before any lookup. Requests and strangers are refused, and the privacy test proves no REQ mentions them.
  - Lookups: CONTACT_DIRECTORY AUTH (ephemeral), in fresh scopes.
  - Welcomes: sealed by the account, gift-wrapped with `no_self_copy` and RECIPIENT_WRAP AUTH, and published only to 10050. There is no 10050 fallback, and the test asserts no bare kind 444 anywhere.
  - Kind 445: under fresh keys only, and the group relay saw no account AUTH (tested).
  - Local data: no plaintext or secrets outside `store.db*` and none in the logs (canary test); the cursor scope and isolation label are domain-separated hashes of the group id.
- **Subscription.** One live REQ per group, event driven, no polling; EOSE per relay moves SYNCING to LIVE, and DISCONNECTED is set when every relay fails. The scope holds a ref across callbacks, so re-subscribing from inside `after_commit()` is safe.
- **Lifetimes.** Account switch and dispose cancel lookups, signer jobs (checked by `run`), deliveries, rounds, the retry timer and subscriptions, and complete waiters. Tested in `test_account_switch`.
- **Composer and model.** `recipients_of()` now returns NULL for every non-NIP-17 backend: NIP-17 is unchanged, and NIP-29 was already NULL. The group-delegate refactor keeps NIP-29 behaviour and never routes MLS to the NIP-17 delegate. MLS rooms are never requests, and the notifier treats them as groups. `groundhog-conversations`, `-composer`, `-dm-inbox`, `-nip29*` and `-privacy-summary` pass.
- **libmarmot `marmot_get_group_members()`.** Additive, read-only and correctly leaf-ordered and deduplicated. Its test covers pending versus merged Removes and bad arguments. It merges with 0.10.0; only the test RUN list conflicts.

## Required before approval

1. B1: bound the cursor and advance it only on accepted events; add a replay/future-date test.
2. B2: enroll the account proof through the signer per generation, gate KeyPackage and group creation on it, map the identity error, and test against 0.10.0. Alternatively, land the branches in an order where this branch is rebased on 0.10.0 with the enrollment in place.
3. Before enabling `GH_FEATURE_ENCRYPTED_GROUPS`, fix or file beads for M1–M4.
