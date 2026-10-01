# W24b slice J review: adopted KeyPackage transport lifecycle

- Slice: J (bead nostrc-0bdg), branch `marmot/w24b-kp-lifecycle`, commit `55db53e5` on master `7f73738f`
- Reviewer: independent peer review (AGENTS.md), 2026-09-30
- Spec: marmot `07da8ffb` foundation/key-packages.md ("Selection and lifecycle", "Failure behavior"), transports/nostr.md ("KeyPackage publication", "Publish targets and acknowledgements"); MDK v0.11.0 `946e0547` (marmot-app `key_package_records.rs`, transport-nostr-adapter `key_package.rs`)

## Verdict: CHANGES-REQUIRED

The libmarmot side is sound:
- Private material is deleted transactionally:
  - inside the accept transaction for a consumed single-use key;
  - inside its own transaction on confirm and sweep.
- A failed Welcome changes nothing.
- The ACK-tie is correct.
- The created_at rule is correct.
- The strict-tag work is correct.
- Every claimed fix is pinned by a test that fails when the fix is reverted.

Two Groundhog-level problems block approval:
- **H1:** any KeyPackage rotation other than the join-triggered one now destroys pending invitations. This is a regression from master, reproduced below.
- **H2:** an account without a kind 10002 list silently stops publishing KeyPackages. Groundhog never publishes a 10002, so freshly onboarded users become uninvitable, with no UI signal and no tracking bead.

## Verification performed

| Check | Result |
|---|---|
| macOS build (`cmake -G Ninja -DBUILD_GROUNDHOG=ON`, `ninja`) | OK |
| ctest: `marmot*`, `groundhog-mls*`, `groundhog-privacy*`, `groundhog-store*`, `groundhog-ui*` | 42/42 passed (keyring test skipped) |
| `scripts/check-unsequenced-args.py` | clean |
| `scripts/linux-gate.sh --sanitizers` | passed, 52 tests, including `groundhog-mls-kp-lifecycle{,-adopted}` |
| libmarmot tests under ASAN+UBSAN+LSan in the Linux CI image | 24/24 passed, including `marmot_test_kp_lifecycle` |
| Trial merge J+I (`marmot/w24b-wn-components`), build and lifecycle/adopted/service/privacy tests | doc-only conflicts; `credentials.c` auto-merges; 8/8 passed |
| Trial merge J+H (`marmot/w24b-adopted-commits`) | mechanical conflicts only (see "Merge risk") |

Revert spot-checks (each patch was applied alone, then the binary was rebuilt and the test run):

| Reverted | Caught by |
|---|---|
| `marmot_kp_lifecycle_consumed()` call in `accept_welcome_internal` | `test_kp_lifecycle` (single-use deleted after join) |
| older-entry deletion in `confirm_impl` | `test_kp_lifecycle` (k1 deleted on k2's confirmation) |
| expiry deletion in `sweep_impl` | `test_kp_lifecycle` (deleted at not_after) |
| strictly-newer `created_at` | `test_kp_lifecycle` (the replacement is newer) |
| Groundhog `key_package_confirm()` on first OK | `rotation-ack-tied` |
| publish to inbox (10050) relays instead of write relays | `write-relays-only` |
| trusting phase-1 (discovery relay) KeyPackages / asking 30443 in phase 1 | `lookup-privacy` |
| dropping the group-relay exclusion | `lookup-privacy` |
| join rotates immediately (no pending-invitation wait) | `pending-invitations-defer-rotation` |
| in-flight rotation lost (`key_package_done` re-publish and cursor guard) | `rotation-ack-tied` |

## Findings

### H1 (High): a lifetime or manual rotation while an invitation is pending makes that invitation unacceptable. This is a regression.

`gnome/groundhog/src/mls/gh-mls-service.c:5119` (age check in `key_package_maybe_publish`), `:5161` (`key_package_rotate`), `:5185` (`gh_mls_service_rotate_key_package`), and `libmarmot/src/kp_lifecycle.c:386`.

**What happens.** A received invitation is stored pending as the raw Welcome. Accepting it re-opens the Welcome with the KeyPackage's `kp_priv` (`welcome_open`). Only the join-triggered rotation checks `invitations_pending()`. The other two paths call `key_package_maybe_publish()` with no such check:
- the 28-day lifetime rotation;
- the user's "rotate" action.

The replacement's first relay OK then calls `marmot_key_package_confirm_published()`, which deletes the old key. On master, `kp_priv` was never deleted after creation, so a pending invitation was always acceptable.

**Failure scenario, reproduced.** I added the probe test in the appendix to `test_mls_kp_lifecycle.c`:
1. Alice invites Bob, and the invitation is pending on Bob's side.
2. Bob's KeyPackage rotates (`gh_mls_service_rotate_key_package`, which takes the same path as the 28-day rotation).
3. The replacement is confirmed.
4. Bob clicks Accept.

Result: `init key ... still held: 0`, and then `The invitation could not be accepted: matching key not found`. The invitation then disappears from `list_invites`.

How often this bites: with a 28-day rotation period, an invitation left pending for d days hits a rotation with probability of roughly d/28. That is about 10% for 3 days. Nothing tells the user or the inviter.

**Required fix.** The spec forbids keeping the key past the confirmed replacement ("Local retention policy MUST NOT extend either bound"). The fix therefore has to hold back the replacement *publish*, not keep the key:
- Apply the pending-invitation wait to every rotation path, not only the join path. Gate `key_package_maybe_publish()`'s age-based branch and `key_package_rotate()` on `invitations_pending()`.
- Bound the wait so the old KeyPackage is still current. Its Lifetime is 84 days and rotation is at 28, so there is room. For example, defer at most until `published_at + lifetime + max_defer` with `max_defer` ≤ 7 days, and also never past `not_after` − 1 day.
- For a user-requested rotation with an invitation pending, either refuse with an explanation or warn that the pending invitation will be lost.
- Add the probe as a regression test, asserting the accept joins, plus a test where the deferral cap expires.

### H2 (High): no kind 10002 means no KeyPackage, silently. Groundhog never publishes a 10002. This is a UX regression and a charter mismatch.

`gnome/groundhog/src/mls/gh-mls-service.c:4835` (`key_package_relays`), `:5110` (`NO_RELAYS`); `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md:557`.

**What changed.** KeyPackages now go only to the account's 10002 write-capable relays. Before, they also went to its 10050 inbox relays. Groundhog's only list publisher is `GhInboxSetup`, and it publishes kind 10050 only. Nothing in `src/` builds a kind 10002 event. As a result:
- An account whose 10002 was never published by another client gets `GH_MLS_KEY_PACKAGE_NO_RELAYS`.
- Nothing reads that state: no UI consumer of `key-package-state` exists.
- The user cannot be invited to an encrypted group and is never told.

Lookups changed too. KeyPackages are now trusted only from the invitee's 10002 write relays (phase 2). Two freshly onboarded Groundhog users who each published only a 10050, with `adopt_discovery` making the inbox relays their discovery relays, could invite each other on master and cannot now. The inviter sees "hasn't set up encrypted groups".

This is the correct spec behaviour (transports/nostr.md "KeyPackage publication"). Two things are still missing:
- The product is not yet complete for non-technical users.
- Charter row §4.3 (line 557, "Own list publish (10050, 10002, KeyPackage 30443) | own write + inbox relays") was not amended. The code's comments cite that row while contradicting it.

The feature is still behind `GH_FEATURE_ENCRYPTED_GROUPS=0`, so this need not be fixed in this slice. It must be tracked and must block the flag flip.

**Required before merge:**
- File a P1 bead that blocks the encrypted-groups flip.
- Amend charter line 557 with a decision row: KeyPackages go to the 10002 write set only; 10050 is for Welcomes.

**Recommended fix (charter-consistent).** Extend onboarding (`GhInboxSetup` and its confirm page) to publish a kind 10002 under the existing OWN_LIST_PUBLISH purpose, with the same PD-13 consent the 10050 already gets ("Let people find your encrypted-group keys on these relays"):
- Publish only when own-list discovery (`GhAccountRelays`, OWN_LIST_DISCOVERY) finished with EOSE from the discovery relays and found **no** 10002.
- Never overwrite or narrow an existing 10002. It is replaceable and other clients own it.
- If a 10002 exists but has no write-capable entry, offer to add the chosen relays as unmarked `r` entries, preserving every existing tag.
- Use the relays the user chose for messages, as unmarked `r` entries. Add no hard-coded hosts (P1).
- Surface `GH_MLS_KEY_PACKAGE_NO_RELAYS` in the UI, for example in Settings and on the "new group" and "invite" entry points: "People can't invite you to encrypted groups yet. Set up". Link it to that flow.
- Do not fall back to 10050 publishing. It is not where adopted peers (MDK 0.11, White Noise) look, and it would re-widen the egress set the charter now restricts.

### M1 (Medium): a join-owed rotation can be deferred indefinitely and is re-evaluated only on accept, decline, or restart.

`gnome/groundhog/src/mls/gh-mls-service.c:4282` (`key_package_rotate_after_join`).

**Scenario.** Bob joins group A while invitation B is pending, so the cursor is set to 1 and no replacement is published. Two things then go wrong:
- B can leave the pending state without an accept or decline. The probe shows a failed accept drops it, and refusals can drop it too. In that case the owed rotation waits until the next restart or the 28-day rotation.
- If Bob simply ignores B, the spent last-resort KeyPackage stays published and its init key stays held for up to 28 days. That key already protects one recorded join secret.

This stays within the spec's bound (MAY keep until the confirmed replacement or not_after), but it stretches the post-join SHOULD-replace window with no cap. **Fix:** use the same bounded deferral as H1, measured from the join. Also re-run `key_package_rotate_after_join(self, FALSE)` whenever a pending Welcome leaves the pending state for any reason, including a failed accept and a refusal.

### L1 (Low): `invitations_pending()` fails open.

`gnome/groundhog/src/mls/gh-mls-service.c:4259`.

When `marmot_get_pending_welcomes()` returns an error, it returns FALSE, so the rotation runs and may delete the key pending invitations need. It should fail closed: wait, keep the cursor at 1, and re-evaluate later. `page.limit = 1000` is fine.

### L2 (Low): libmarmot's own SQLite backend does not erase deleted keys.

`libmarmot/src/storage_sqlite.c:1380`.

Groundhog's store sets `secure_delete=ON` and truncates the WAL, so Groundhog's deletions really erase. libmarmot's `storage_sqlite.c`, used by marmot-gobject and gnostr, sets only `journal_mode=WAL`. A "deleted" `kp_priv` survives in free pages and the WAL. With SQLCipher, it survives decryptably for anyone who later obtains the DB key, which is exactly the forward-secrecy threat. Those clients do not call confirm or sweep yet (nostrc-5plt). **Fix:** add `PRAGMA secure_delete=ON` there, or make it a precondition in nostrc-5plt.

### L3 (Low): the expiry sweep only runs at publish checks.

`gnome/groundhog/src/mls/gh-mls-service.c:5107`.

`key_package_sweep()` runs only from `key_package_maybe_publish()`, which is triggered by start, retry, rotation, accept, and decline. Take an account that falls into `NO_RELAYS` (see H2): it schedules no retry, so in an app left running, a KeyPackage made earlier can outlive `not_after` until the next restart. The spec says it MUST be deleted at `not_after`. **Fix:** arm a timer for the earliest `not_after` in the record, or sweep from the existing retry or resume tick regardless of relay state.

### L4 (Low): the consumer does not check `mls_extensions` or `mls_proposals` against the decoded KeyPackage.

`libmarmot/src/credentials.c:1317`.

MDK 0.11 (`require_multi_value_key_package_tag_matches`) rejects a 30443 whose `mls_extensions` or `mls_proposals` tag is not exactly the decoded KeyPackage's set. libmarmot checks only the id-list form. A KeyPackage that MDK peers reject can therefore be selected by Groundhog. Real capability checks use the decoded leaf, so the impact is interop and consistency only. The new `mls_ciphersuite` singleton rule is consistent with the spec ("MLS ciphersuite id", singular) and with MDK's producer. Note that MDK's consumer reads only the first value, so libmarmot is stricter, which is fine.

### N1 (Nit)

`gnome/groundhog/src/mls/gh-mls-service.h:308`: the `GH_MLS_KEY_PACKAGE_NO_RELAYS` comment still says "no own write or inbox relay".

### N2 (Nit)

`gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c:1-7`: the header comment is garbled. "Every account's kind 10002 splits its relays" appears twice, the first version is stale, and a fragment is orphaned.

### N3 (Nit)

`MarmotKeyPackageInfo` rows (ref, relays, created_at) are never pruned when their private material is deleted. They grow by one per rotation and keep a local rotation history. They are not secret, but `life_seed()` walks them all.

## Focus-area notes

1. **Forward secrecy and key lifecycle.** Key deletion is correct as specified:
   - A consumed single-use key is deleted inside the accept transaction (`welcome.c` → `marmot_kp_lifecycle_consumed`). A rollback restores it.
   - Last-resort keys go at the confirmed replacement (`confirm_impl` deletes every lower `seq`) or at `not_after` (sweep).
   - A failed Welcome never reaches the hook, and accept-failure never rotates.

   The 32 cap evicts only the oldest entry, never the newest confirmed one. Since confirm already removed everything older than the newest confirmed entry, the victim is always the oldest unconfirmed newer one. No path keeps a key forever once Groundhog confirms:
   - Entries with `not_after == 0`, which arise only from unparsable seeded KeyPackages, are older than any new KeyPackage, so the next confirmation removes them.
   - Callers that never confirm (marmot-gobject and Gnostr) keep up to 32 keys, as before (nostrc-5plt).

   Deleting too early is the H1 problem. Per-retry minting of fresh KeyPackages is tracked in nostrc-9c06.

   `kp_life` and `kp_priv` are outside the snapshot allowlist (`MLS_STATE_SCOPE`), so an epoch rollback cannot resurrect a key.
2. **UX regression.** Confirmed. See H2 for the fix.
3. **Privacy.**
   - Phase 1 asks discovery relays for `{kinds:[10002]}` only.
   - Phase 2 asks only `write`-marked or unmarked `r` entries (read-only ones are skipped).
   - KeyPackages count only from phase 2.
   - Group relays (new or existing) are excluded in both phases.
   - Each phase is a fresh scope under `GH_AUTH_PURPOSE_CONTACT_DIRECTORY` (ephemeral AUTH, never the account).
   - The publish path uses OWN_LIST_PUBLISH to the write set only.

   Each of these is pinned by a revert-checked test. Residual risk: the discovery relays still learn whom you look up. That is accepted by charter §2.2, and the invitee's write relays learn the same.
4. **Strict tags.**
   - 30443: `d`, `mls_protocol_version`, `i`, singleton `mls_ciphersuite` matching the KeyPackage, id-list form for all four lists, exact private-use `app_components` including 0x8009, no `encoding`, no `relays`, and `i` verified against the decoded ref.
   - 444 rumor: exactly one `e` (exactly one lowercase value) and exactly one `relays` (one or more values).

   All match the spec table. Remaining gap: L4.
5. **Concurrency.**
   - A rotation requested while a publish is in flight now survives: the cursor is not recorded and `key_package_done` republishes. This is pinned.
   - `key_package_in_flight` is cleared on done, on start failure, and on `stop_generation`, so a stale OK cannot confirm the wrong ref (`job->ref` is also checked against the signed event's `i`).
   - The join wait persists across restarts through `mls/key-package-joined`, which is pinned. See M1 and L1 for its gaps.
6. **Merge risk with H and I: low.**
   - J+I: conflicts only in `VERSION_MANIFEST.md` and `libmarmot/README.md` (keep both). `credentials.c` auto-merges: I's capability constants sit beside J's new `Marmot *m` and `last_resort` parameters. The merged tree builds and passes the lifecycle, adopted, service and privacy tests.
   - J+H: conflicts in `VERSION_MANIFEST.md`, `libmarmot/README.md`, `libmarmot/CMakeLists.txt` and `meson.build` (adjacent test registrations, keep both). `welcome.c` and `gh-mls-service.c` auto-merge.
   - The H-vs-I conflicts (`test_mdk011_interop.c`, Groundhog `CMakeLists.txt`, the interop README) are not J's.

## Versioning

Everything is folded into the unreleased libmarmot 0.12.0 and groundhog 0.12.0, with decision rows in `VERSION_MANIFEST.md`. This is correct per AGENTS.md: new public API and a behaviour change inside an unreleased MINOR, with no ABI or wire change.

## Appendix: H1 reproduction (probe test, not committed)

```diff
diff --git a/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c b/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c
index f94d011c..19a262b3 100644
--- a/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c
+++ b/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c
@@ -590,6 +590,47 @@ test_pending_invitations_defer_rotation(void)
   world_down(&w);
 }
 
+
+/* REVIEW PROBE: a rotation (manual, or the 28-day lifetime one, both through
+ * key_package_maybe_publish) while an invitation is pending. */
+static void
+test_probe_rotation_with_pending_invite(void)
+{
+  World w;
+  world_split_lists = TRUE;
+  const guint keys[] = { ALICE, BOB };
+  world_up(&w, keys, G_N_ELEMENTS(keys));
+  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
+  wait_published(alice);
+  wait_published(bob);
+  g_autofree gchar *ref1 = NULL;
+  newest_key_package(&w.w, BOB, &ref1, NULL, NULL);
+  g_autofree gchar *id1 = g_strdup(gh_mls_service_get_key_package_id(bob->service));
+  accept_contact(alice, BOB);
+  g_autoptr(GError) error = NULL;
+  g_assert_nonnull(create_attempt(alice, "One", (const guint[]){ BOB }, 1, &error));
+  g_assert_no_error(error);
+  InvitesWait one_wait = { bob, 1 };
+  spin_until(invites_at_least, &one_wait, "the invitation");
+  g_assert_true(gh_mls_service_rotate_key_package(bob->service, &error));
+  g_assert_no_error(error);
+  RotatedWait done = { bob, id1 };
+  spin_until(rotated, &done, "Bob's rotation");
+  g_test_message("PROBE: init key of the pending invitation's KeyPackage still held: %d",
+                 has_init_key(bob, ref1));
+  g_autofree gchar *one = invite_from(bob, ALICE);
+  GhMlsGroup *g = gh_mls_service_accept_invite(bob->service, one, &error);
+  g_test_message("PROBE: accept of the pending invitation: %s (%s)", g ? "JOINED" : "FAILED",
+                 error ? error->message : "-");
+  {
+    g_autoptr(GError) e2 = NULL;
+    g_autoptr(GPtrArray) inv = gh_mls_service_list_invites(bob->service, &e2);
+    g_test_message("PROBE: invitations still listed after the failed accept: %u", inv ? inv->len : 0);
+  }
+  g_assert_nonnull(g);
+  world_down(&w);
+}
+
 /* `key` runs an older client: a KeyPackage without the account proof on W. */
 static void
 inject_unproven_key_package(World *w, guint key)
@@ -703,6 +744,7 @@ main(int argc, char **argv)
   g_test_add_func(KP_TEST("pending-invitations-defer-rotation"),
                   test_pending_invitations_defer_rotation);
   g_test_add_func(KP_TEST("failed-welcome-preserves"), test_failed_welcome_preserves);
+  g_test_add_func(KP_TEST("probe-rotation-pending"), test_probe_rotation_with_pending_invite);
 #endif
   gint rc = g_test_run();
   mls_world_finish();
```


---

## Addendum (2026-10-01): re-review of the fixes on tip `1fbf3742`

Fix commits: `ac7a133b` (H1, M1, L1, L3), `99533977` (L2, L4, N3), `5d89ec0d` (H2, N2), `1fbf3742` (versions). The review branch was rebased onto `1fbf3742`.

### Final verdict: CHANGES-REQUIRED (narrow)

Every original finding is fixed, and every fix I reverted is caught by a test. One new problem blocks approval:
- **R1 (High):** the new onboarding relay-list offer can overwrite a user's existing kind 10002. I reproduced this. It breaks the "never overwrite" requirement and the onboarding switch's own promise.

The fix is small, local to `GhInboxSetup`, and should also address R2 and R3. The H1/M1 lifecycle work and the libmarmot changes are approved as they stand.

### Verification

| Check | Result |
|---|---|
| macOS build at `1fbf3742` | OK |
| ctest: marmot, groundhog mls/privacy/store/ui/onboarding/preferences/inbox/account | 50/50 passed (keyring skipped); `groundhog-mls-kp-lifecycle` now 11 cases |
| `scripts/check-unsequenced-args.py` | clean |
| `scripts/linux-gate.sh --sanitizers` | passed, 52 tests |
| libmarmot under ASAN+UBSAN+LSan (Linux CI image) | 24/24 passed |
| Docker volumes | none left behind |

Revert spot-checks (each patch applied alone, then rebuilt and the test run):

| Reverted | Caught by |
|---|---|
| `key_package_hold()` call in `key_package_maybe_publish` | `pending-invitations-defer-rotation` (and the held tests) |
| the cap (`now >= deadline`) | `hold-cap-expires` |
| `relay_list_needed` ignores an existing list | `relay-list-never-replaced` |
| L4 exact-set match | `test_kp_lifecycle` (extra extension refused) |
| N3 info-row deletion | `test_kp_lifecycle` (no rotation history) |
| L1: treat UNKNOWN as "none" | **not caught**: no test reaches the listing-error path (Nit A3) |

Review probes (appendix B; not committed):

| Probe | Result |
|---|---|
| P1: the original H1 probe, adapted. Invitation pending, rotate, accept. | Rotation held (`held=1`), key kept, accept **JOINED**. The replacement then publishes and retires the old key. **H1 fixed.** |
| P2: cap 8 s. Hold, wait 5 s, restart, measure until the replacement settles. | Published 8.8 s after the first hold, 3.7 s after the restart. The hold start survives a restart and is not reset. **M1 fixed.** |
| P3: fresh world. The user's existing 10002 (`wss://my-real-outbox.example`) is on the chosen message relay X, not on the discovery relay E. | `relay_list_needed()` = TRUE, the switch is shown (on by default), and after Publish (`DONE`) relay X holds **only Groundhog's new 10002**. The user's list was replaced. **R1.** |

### Original findings

- **H1: fixed.** Every rotation path goes through `key_package_hold()`: lifetime, manual, and join (`key_package_rotate()` now serves the join too). While an invitation is pending, a due replacement is not published. The key is never kept past the confirmed replacement, which respects the spec's bound. The old KeyPackage stays published and usable, and `gh_mls_service_get_key_package_held()` exposes the state. Tests cover the manual path (`rotation-held-for-pending-invitation`) and the 28-day path (`lifetime-rotation-held`).
- **M1: fixed.** The cap is min(first hold + `key_package_max_hold` (7 days), earliest `not_after` − 1 day), using libmarmot's new `marmot_key_package_next_expiry()`. The start is persisted in `mls/key-package-held`, and P2 confirms it survives a restart. The hold is re-evaluated on accept, decline, a failed accept (new), start, and a timer (at most every 600 s and at the deadline). It is cleared on the replacement's first OK. Timers are removed in `stop_generation()`, which `dispose` runs. When the cap ends, a pending invitation fails, as the spec's bound requires (pinned by `hold-cap-expires`).
- **L1: fixed.** A listing error is `INVITES_UNKNOWN`, which holds the replacement (bounded by the cap). See A3 for the missing test.
- **L2: fixed.** `storage_sqlite.c` sets `PRAGMA secure_delete=ON`, pinned by a pragma test through a test-only handle. See A4 for a WAL residual.
- **L3: fixed.** The sweep runs at every start (`resume_all`) and on its own timer at the earliest `not_after`, independent of relay state.
- **L4: fixed.** `mls_extensions` and `mls_proposals` must be exactly the decoded leaf's capability sets. MDK 0.11 derives its tag from the same leaf list (`advertised_capabilities_of_leaf`), and neither MDK nor its openmls (`59e7d3b`) adds GREASE by default, so genuine MDK KeyPackages still validate. See A2.
- **N1, N2, N3: fixed.** N3: `delete_private()` also removes the info row through the new storage hook, inside the same transaction. The Groundhog, memory, SQLite and nostrdb backends implement it. nostrdb uses one LMDB transaction per operation, like its other operations, and has no outer `begin`.
- **H2: mostly fixed; see R1–R3.**
  - The kind 10002 is offered only when `GhInboxSetupConfig.offer_relay_list` is set (that is, with `GH_FEATURE_ENCRYPTED_GROUPS`).
  - The offer also requires an active account and the matching generation, `GhAccountRelays` == COMPLETE, and no 10002 seen in any form. A list that does not parse now counts as existing.
  - The list is signed after the 10050 as a separate signer request; declining it keeps the 10050. It names the chosen relays as `write` and goes to the same targets under OWN_LIST_PUBLISH (account AUTH only on challenge, R1/R6). It is re-checked when the signer answers (`SKIPPED`).
  - There is no 10050 fallback.
  - The result page is honest about failure.
  - Charter §4.3 row 557 is amended with a decision paragraph.
  - Preferences › Network › Encrypted Groups shows the live state, and NO_RELAYS gets [Set Up] (`win.setup-inbox`).
  - `fresh-accounts-invite-each-other` passes: two accounts with no lists onboard, both publish KeyPackages to their new write relay, and they invite each other both ways. That test starts with a discovery relay already configured; see R2.

### MarmotStorage append (`delete_key_package_info`): acceptable

- The field is appended at the end of the struct, optional (NULL means rows stay, as before 0.12.0), and treats `STORAGE_NOT_FOUND` as success.
- Every in-tree implementer either `calloc`s the struct (Groundhog, the tests) or uses a libmarmot constructor, so nothing reads uninitialised memory.
- A backend compiled against the 0.11 header and run with 0.12 would be read past its end. Two things prevent this from happening silently:
  - libmarmot's SOVERSION is `0.MINOR` (`CMakeLists.txt:116`, `meson.build:156`), so 0.12 gets a new soname and old binaries won't load it;
  - the README and `VERSION_MANIFEST.md` row say "custom backends must be rebuilt", citing the 0.7.0 transaction-hook precedent.
- Folding this into the unreleased 0.12.0 MINOR is correct per AGENTS.md: new API, an ABI change of a public struct in 0.x, no wire or state-format change. marmot-gobject and Gnostr: rebuild only. That is stated correctly.

### New findings

**R1 (High): the relay-list offer can overwrite an existing kind 10002.**

`gnome/groundhog/src/app/gh-inbox-setup.c:1021-1031` (`gh_inbox_setup_relay_list_needed`) and `:935-939` (targets). P3 reproduces this.

The offer's only evidence that "no list exists" is `GhAccountRelays` on the **discovery** relays. There are two gaps:
- `compute_state()` (`gh-account-relays.c:71-87`) reports COMPLETE when at least one source sent EOSE and the others **failed**. A discovery relay that was down, and holds the user's list, counts as "found none".
- The list is published to the chosen **message relays** too, and those were never asked.

A 10002 is replaceable, so any target holding the user's real list replaces it with Groundhog's newer one, which names only the message relays as `write`. That breaks the user's outbox routing in every other client reading that relay. The switch is on by default, and its subtitle promises "Groundhog never changes a relay list you already have".

Realistic case: an existing Nostr user who configured a discovery relay that doesn't carry their 10002, then picks their usual relay as a message relay.

Fix:
1. When the signer answers, before publishing, send one REQ `{kinds:[10002], authors:[me], limit:1}` to **every publish target**. These relays are already consented, so this is still own-list discovery (ephemeral, no account AUTH).
2. Publish only if every target sent EOSE with no 10002. Otherwise use `SKIPPED` ("you already have a relay list") or `FAILED` ("couldn't confirm you have none").
3. Have `relay_list_needed()` require every discovery source to have sent EOSE, with none failed.
4. Add P3 as a regression test.

**R2 (Medium): a real first run never sees the offer.**

`gnome/groundhog/src/ui/gh-onboarding-view.c:1015` and `gh-inbox-setup.c:1030`.

`discovery-relays` is empty by default (PD-13), so `GhAccountRelays` is NO_SOURCES and the offer is hidden; the `first-run` onboarding test asserts exactly that. A new non-technical user finishes onboarding with no 10002 and is still uninvitable. They can only discover this by opening Preferences and pressing [Set Up]. That second pass works: with `adopt_discovery`, the message relays become the discovery relays, discovery completes, and the offer appears.

Fix: when Publish adopted the message relays as discovery relays, keep the result page open until `GhAccountRelays` completes on them. Then offer the relay list in place, with the same consent and the same R1 check. Alternatively, let the confirm page itself run the R1 target query against the chosen relays, which the user is confirming. Add a first-run test that ends with the account invitable.

**R3 (Low): misleading NO_RELAYS copy.**

`gnome/groundhog/src/ui/gh-preferences-dialog.c` (`sync_key_package`).

NO_RELAYS also covers an account whose 10002 exists but has only read-only, or unparsable, entries. The copy says "your account has no relay list". [Set Up] then cannot help, because `relay_list_needed()` is FALSE when a list exists, so the user goes round in a loop. Distinguish the case with `gh_account_relays_has_relay_list()`: "Your relay list names no relay you publish to. Add one in the app you manage it with." Hide [Set Up] there.

### Nits

- **A2:** MDK filters GREASE extension ids out of the `mls_extensions` tag (`!ext.is_grease()`); libmarmot's exact-set check does not. Interop is fine today, because nothing GREASEs, but a GREASE-ing producer would be accepted by MDK and refused by libmarmot. Skip RFC 9420 GREASE values (`0x?A?A`) on the leaf side to mirror MDK exactly.
- **A3:** L1's listing-error path has no test. Reverting it to fail-open is not caught. A failing-storage hook in the world could pin it.
- **A4:** under WAL, `secure_delete` overwrites the DB pages, but the deleted row's earlier frames stay in `-wal` until a checkpoint rewinds it. Groundhog truncates its WAL; libmarmot's SQLite backend does not. Consider `wal_checkpoint(TRUNCATE)` after confirm and sweep, or document it.
- **A5:** while a replacement is held, Preferences says "People can invite you to encrypted groups". That is true, but `get_key_package_held()` is not shown anywhere; consider surfacing it. Also, any pending Welcome, including one from a stranger holding our public KeyPackage, holds the rotation. This is bounded by the 7-day cap and within the spec, so it is acceptable.

## Appendix B: re-review probes (not committed)

```diff
diff --git a/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c b/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c
index 323cd8dd..6af5ef52 100644
--- a/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c
+++ b/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c
@@ -936,6 +936,111 @@ test_relay_list_never_replaced(void)
   }
   world_down(&w);
 }
+
+/* REVIEW PROBE P1: the original H1 probe (rotate with a pending invitation,
+ * then accept), adapted: the replacement must wait and the accept join. */
+static void
+probe_p1(void)
+{
+  World w;
+  world_split_lists = TRUE;
+  const guint keys[] = { ALICE, BOB };
+  world_up(&w, keys, G_N_ELEMENTS(keys));
+  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
+  wait_published(alice);
+  wait_published(bob);
+  g_autofree gchar *ref1 = NULL;
+  newest_key_package(&w.w, BOB, &ref1, NULL, NULL);
+  g_autofree gchar *id1 = g_strdup(gh_mls_service_get_key_package_id(bob->service));
+  accept_contact(alice, BOB);
+  g_autoptr(GError) error = NULL;
+  g_assert_nonnull(create_attempt(alice, "One", (const guint[]){ BOB }, 1, &error));
+  g_assert_no_error(error);
+  InvitesWait one_wait = { bob, 1 };
+  spin_until(invites_at_least, &one_wait, "the invitation");
+  g_assert_true(gh_mls_service_rotate_key_package(bob->service, &error));
+  drain();
+  g_test_message("PROBE P1: held=%d key=%d", gh_mls_service_get_key_package_held(bob->service),
+                 has_init_key(bob, ref1));
+  g_autofree gchar *one = invite_from(bob, ALICE);
+  GhMlsGroup *g = gh_mls_service_accept_invite(bob->service, one, &error);
+  g_test_message("PROBE P1: accept %s (%s)", g ? "JOINED" : "FAILED", error ? error->message : "-");
+  g_assert_nonnull(g);
+  RotatedWait done = { bob, id1 };
+  spin_until(rotated, &done, "the replacement after the accept");
+  g_test_message("PROBE P1: after replacement key=%d", has_init_key(bob, ref1));
+  g_assert_false(has_init_key(bob, ref1));
+  world_down(&w);
+}
+
+/* REVIEW PROBE P2: the cap counts from the first hold, across a restart. */
+static void
+probe_p2(void)
+{
+  World w;
+  world_split_lists = TRUE;
+  world_key_package_max_hold = 8;
+  const guint keys[] = { ALICE, BOB };
+  world_up(&w, keys, G_N_ELEMENTS(keys));
+  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
+  wait_published(alice);
+  wait_published(bob);
+  g_autofree gchar *id1 = g_strdup(gh_mls_service_get_key_package_id(bob->service));
+  accept_contact(alice, BOB);
+  g_autoptr(GError) error = NULL;
+  g_assert_nonnull(create_attempt(alice, "One", (const guint[]){ BOB }, 1, &error));
+  InvitesWait one_wait = { bob, 1 };
+  spin_until(invites_at_least, &one_wait, "the invitation");
+  gint64 t0 = g_get_monotonic_time();
+  g_assert_true(gh_mls_service_rotate_key_package(bob->service, &error));
+  drain();
+  g_assert_true(gh_mls_service_get_key_package_held(bob->service));
+  while (g_get_monotonic_time() - t0 < 5 * G_USEC_PER_SEC)
+    g_main_context_iteration(NULL, FALSE);
+  app_restart(bob);
+  gint64 t1 = g_get_monotonic_time();
+  g_test_message("PROBE P2: held after restart=%d",
+                 gh_mls_service_get_key_package_held(bob->service));
+  wait_settled(bob, id1);
+  gint64 t2 = g_get_monotonic_time();
+  g_test_message("PROBE P2: published %.1fs after the first hold, %.1fs after the restart (cap 8s)",
+                 (t2 - t0) / 1e6, (t2 - t1) / 1e6);
+  g_assert_cmpint((t2 - t0) / G_USEC_PER_SEC, <, 12);
+  world_down(&w);
+}
+
+/* REVIEW PROBE P3: the user's existing 10002 lives on the chosen message
+ * relay X, not on the discovery relay E. */
+static void
+probe_p3(void)
+{
+  World w;
+  world_fresh_lists = TRUE;
+  const guint keys[] = { ALICE };
+  world_up(&w, keys, G_N_ELEMENTS(keys));
+  App *alice = &w.apps[ALICE];
+  seed_list(&w.x, ALICE, 10002, "wss://my-real-outbox.example");
+  drain();
+  GhInboxSetupConfig config = { .accounts = alice->accounts, .account_relays = alice->relays,
+                                .settings = alice->settings, .offer_relay_list = TRUE };
+  g_autoptr(GhInboxSetup) probe = gh_inbox_setup_new(&config);
+  gboolean offered = gh_inbox_setup_relay_list_needed(probe);
+  g_test_message("PROBE P3: offered=%d", offered);
+  g_object_run_dispose(G_OBJECT(probe));
+  if (offered) {
+    g_autoptr(GhInboxSetup) setup = onboard(alice, w.x.url, TRUE);
+    g_test_message("PROBE P3: relay list state=%d", gh_inbox_setup_get_relay_list_state(setup));
+    g_autoptr(GPtrArray) lists = published(&w.x, 10002);
+    for (guint i = 0; i < lists->len; i++) {
+      NostrEvent *l = g_ptr_array_index(lists, i);
+      NostrTag *r = nostr_tags_get(nostr_event_get_tags(l), 0);
+      g_test_message("PROBE P3: X holds 10002 created_at=%" G_GINT64_FORMAT " r=%s",
+                     (gint64)nostr_event_get_created_at(l), nostr_tag_get(r, 1));
+    }
+    g_object_run_dispose(G_OBJECT(setup));
+  }
+  world_down(&w);
+}
 #endif
 
 #if GH_MLS_ADOPTED_KEY_PACKAGES
@@ -977,6 +1082,9 @@ main(int argc, char **argv)
   g_test_add_func(KP_TEST("fresh-accounts-invite-each-other"),
                   test_fresh_accounts_invite_each_other);
   g_test_add_func(KP_TEST("relay-list-never-replaced"), test_relay_list_never_replaced);
+  g_test_add_func(KP_TEST("probe-p1"), probe_p1);
+  g_test_add_func(KP_TEST("probe-p2"), probe_p2);
+  g_test_add_func(KP_TEST("probe-p3"), probe_p3);
 #endif
   gint rc = g_test_run();
   mls_world_finish();
```


---

## Final addendum (2026-10-01): R1–R3 and nits on tip `fc11dd79`

Fix commits: `7f5ce63c` (A2, A4), `63762b83` (R1, R2, R3, A3, A5), `fc11dd79` (charter, versions, changelog). The review branch was rebased onto `fc11dd79`.

### Final verdict: APPROVE-WITH-NITS

R1, R2 and R3 are fixed and hold up under the variants below. The ADD_WRITE design (editing the user's existing 10002) is safe and consistent with the charter.

One residual remains (F1, Low): a list that appears **while the signer prompt is open** is still published over. It needs another client to publish the account's list during that prompt. It should be closed before `GH_FEATURE_ENCRYPTED_GROUPS` flips, but it does not block this slice.

### Verification

| Check | Result |
|---|---|
| macOS build at `fc11dd79` | OK |
| ctest: marmot, groundhog mls/privacy/store/ui/onboarding/preferences/inbox/account | 50/50 passed (keyring skipped) |
| `check-unsequenced-args.py`, `gnome/groundhog/tests/check_privacy.py` | clean |
| `scripts/linux-gate.sh --sanitizers` | passed, 52 tests. One unrelated flake: libnostr `test_relay_teardown_leaks` failed in the parallel run and passed alone, with no sanitizer report (the gate notes 1 rerun in its last 20 runs) |
| libmarmot under ASAN+UBSAN+LSan | 24/24 passed |
| Docker volumes | none created |

### R1: rerun of the overwrite probe, plus variants

I added a probe-only wire-relay knob (`late_10002_ms`: answer kind-10002 REQs after a delay) for the timing variants. Probes are in appendix C and not committed.

| Probe | Setup | Result |
|---|---|---|
| V1 (P3 rerun) | The user's 10002 only on message relay X; discovery relay E has none | Offer shown (CREATE), then **SKIPPED** at the pre-publish check. X still holds only the user's list; nothing published anywhere. **Fixed.** |
| V2 | X connected but answers kind-10002 REQs only after 25 s; no list anywhere | **FAILED** at the 15 s deadline ("couldn't confirm… Nothing was published"); no 10002 on E or X |
| V3 | The user's list only on X, which answers after 25 s | **FAILED** at 15.0 s; the user's list on X intact; nothing on E |
| V4 | The user's list only on X, which answers after 6 s, inside the deadline | **SKIPPED** at 6.0 s; the list intact |
| V5 | Discovery = {E, H}; the user's list only on H, which answers after 6 s | Polled for 5 s while H was silent: **never offered** (DISCOVERING, then `all_answered`); after H answered, offer NONE |
| V6 | ADD_WRITE; base (read-only list) on E; a **newer** list from another client on target X | **SKIPPED**; the other client's list intact |
| V7 | CREATE; another client publishes the user's first 10002 to X **after the check, while the signer request is open** | **DONE**: Groundhog's list (dated 5 s later) was published to X, so it supersedes the other client's list on any NIP-01 relay. This is **F1** |

Revert spot-checks, each in `gh-relay-list-setup.c`:

| Reverted | Caught by the author's tests? |
|---|---|
| ignore a target that holds a list (`found`) | yes: `relay-list-on-a-message-relay-kept` |
| a failed or closed target counts as "none" | yes: `relay-list-unconfirmed-not-published` |
| offer on partial discovery (`all_answered` dropped) | yes: `relay-list-not-offered-on-partial-discovery` |
| ADD_WRITE drops non-`r` tags | yes: `relay-list-write-relay-added` |
| a **silent** target counts as "none" at the deadline | **no**: only my V2 (F2) |
| ADD_WRITE ignores a **newer** list on a target | **no**: only my V6 (F2) |

How the R1 fix works:
- The offer now needs own-list discovery COMPLETE with **every** discovery relay answering (`gh_account_relays_get_all_answered()`).
- Before signing, every publish target (chosen relays ∪ own write ∪ discovery) gets its own REQ `{kinds:[10002], authors:[me]}` under OWN_LIST_DISCOVERY (ephemeral AUTH only, never the account).
- A target holding a list → SKIPPED. A target that fails, closes, or stays silent past `GH_RELAY_LIST_SETUP_CHECK_S` (15 s) → FAILED, with nothing signed or published.
- The offer is checked again right before the signer request.

### R2: a first run with empty `discovery-relays`: fixed

At Publish nothing is known, so the relay list stays NONE. With `adopt_discovery`, the message relays become the discovery relays. Once GhAccountRelays has asked them, the onboarding result page offers "Publish Relay List" (CREATE) or "Add My Relays" (ADD_WRITE). That offer has its own consent button and runs the same check of every target.

Two tests cover this:
- `first-run-accounts-invite-each-other` (`world_no_discovery`): no discovery relay at all; the offer appears after adoption; both accounts end up invitable and invite each other.
- `groundhog-onboarding first-run-offers-relay-list`: the result page, with the default empty setting.

Preferences [Set Up] remains the fallback for anyone who skips it.

### R3: the ADD_WRITE edit: safe and consistent with the charter

I judge it an **additive edit of the freshest known list, under consent, never a replacement**:
- **When it is offered.** Only when the account's 10002 exists but names **no** write-capable relay (only `read` entries, or none usable). A list that already has a write relay is never offered an edit, so Groundhog never narrows or re-orders a working outbox setup. In that case the account is already invitable.
- **The base is the freshest known list.** GhAccountRelays keeps the newest 10002 by NIP-01 (created_at, then lower id) across the discovery relays. The pre-publish check then makes any **other** list on any target, at least as new as the base, a SKIPPED (V6; equal timestamps count as newer). Older lists on targets are superseded correctly.
- **The edit is additive.** `build_extended()` copies every tag verbatim, in order: non-`r` tags (e.g. `client`), unparsable `r` entries, every other relay. It keeps the content. The only changes:
  - each chosen relay is appended as `["r", url, "write"]`;
  - if it was already listed as `read`, that entry becomes unmarked (read and write), so nothing is lost.

  `created_at` = max(now, base + 1). The author's `relay-list-write-relay-added` test pins tag order and content.
- **It requires consent:**
  - the confirm-page switch, retitled "Add These Relays to Your Relay List", whose text says everything else is kept;
  - the result-page button "Add My Relays", the Preferences [Add Relays] button and the NO_WRITE_RELAYS copy;
  - plus a separate Nostr Signer request.
- **Charter:** the §4.3 decision paragraph records it ("can, with consent, have the chosen relays added to it as `write` entries, every other tag kept"). It is published under OWN_LIST_PUBLISH and checked under OWN_LIST_DISCOVERY, so no new egress purpose or identity is introduced.
- **Residual:** F1's race applies here too. A concurrent edit made in another client during the signer prompt would be lost, as in any read-modify-write over replaceable events. See F1.

R3 Preferences: NO_RELAYS is split into NO_RELAYS ([Set Up]) and NO_WRITE_RELAYS ([Add Relays], accurate copy). The relay step now fulfils the latter, so there is no loop. Pinned in `groundhog-preferences key-package-row`.

### Nits from the previous round: all fixed

- **A2:** `marmot_mls_is_grease()` (RFC 9420 §13.5 pattern, 0x0A0A–0xEAEA) is skipped for `mls_extensions` only. Proposals are compared whole, exactly as MDK's `advertised_capabilities_from_caps()` does. Tested.
- **A3:** a test hook makes the invitation listing fail, and the rotation is held (`invitation-listing-error-holds`).
- **A4:** after a `kp_priv` delete, `storage_sqlite.c` runs `wal_checkpoint(TRUNCATE)` when in autocommit. This backend has no `begin`/`commit` hooks, so libmarmot's transactions are no-ops on it and the checkpoint does run. The storage contract test checks that a canary is in neither the DB file nor the `-wal`.
- **A5:** a held replacement is shown in Preferences (`HELD`), live through the new `key-package-held` property.

### Remaining findings

- **F1 (Low): the existence check is not repeated after the signer answers.**
  - `gh-relay-list-setup.c:262-285` (`sign_list`) and `:236-260` (`on_signed`): `check_settled()` closes the check REQ and asks the signer, and the signer's answer goes straight to `start_publish()`. With a remote signer the prompt can stay open for minutes.
  - If another client publishes the account's first 10002 (CREATE), or edits it (ADD_WRITE), on a target during that window, Groundhog's newer list supersedes it. V7 reproduces this.
  - **Fix (either):**
    - keep the check subscription open through SIGNING and abort (SKIPPED) on any `other_list()` event;
    - re-run the target check after `on_signed` and publish only if it is still clean.
  - File a bead blocking the encrypted-groups flip.
- **F2 (Low): two guards have no test.** The deadline path (a connected but silent target) and ADD_WRITE's "newer list on a target" are each caught only by my probes V2 and V6. Adopt them, with a wire-relay delay knob like `late_10002_ms`, or an equivalent `withhold`-based variant.
- **F3 (Nit): the ADD_WRITE switch defaults to on.** On the confirm page, `relay_list_switch` is `active: true` for both modes. Consent is still explicit (a visible switch, Publish, and a separate signer prompt), but an edit of a user-owned list would be better opt-in: default it to off for ADD_WRITE.
- **F4 (Nit): charter table row not updated.** The "Own list discovery" row still lists only `discovery-relays` as its relays. The pre-publish check now also asks the publish targets under that purpose. The decision paragraph says so; the row's cell should too.
- **F5 (Nit, inherent): a relay can hide the list and still answer.** A relay that serves the author's 10002 only to authenticated readers but still answers EOSE to an ephemeral reader is indistinguishable from "none". This is rare for a public kind and has no fix short of account AUTH, which the charter forbids for discovery. Mention it in the gh-relay-list-setup.h comment.

## Appendix C: final re-review probes (not committed)

```diff
diff --git a/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c b/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c
index 5c02f1cc..defeda86 100644
--- a/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c
+++ b/gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c
@@ -1208,6 +1208,225 @@ test_relay_list_never_replaced(void)
   }
   world_down(&w);
 }
+
+/* ---- REVIEW PROBES (final re-review) ---- */
+static guint
+probe_lists_on(WireRelay *relay, guint key, const gchar *want_r)
+{
+  guint n = 0;
+  for (guint i = 0; i < relay->stored->len; i++) {
+    WireStored *st = g_ptr_array_index(relay->stored, i);
+    if (nostr_event_get_kind(st->event) != 10002 ||
+        g_strcmp0(nostr_event_get_pubkey(st->event), hex[key]) != 0)
+      continue;
+    NostrTag *r = nostr_tags_get(nostr_event_get_tags(st->event), 0);
+    g_test_message("PROBE   10002 on %s: created_at=%" G_GINT64_FORMAT " first r=%s", relay->url,
+                   (gint64)nostr_event_get_created_at(st->event), r ? nostr_tag_get(r, 1) : "-");
+    if (!want_r || (r && g_strcmp0(nostr_tag_get(r, 1), want_r) == 0))
+      n++;
+  }
+  return n;
+}
+
+static void
+probe_state(const gchar *name, GhInboxSetup *setup, gint64 t0)
+{
+  GhRelayListSetup *l = gh_inbox_setup_get_relay_list(setup);
+  const GError *e = l ? gh_relay_list_setup_get_error(l) : NULL;
+  g_test_message("PROBE %s: relay list state=%d (DONE=%d FAILED=%d SKIPPED=%d) after %.1fs: %s",
+                 name, gh_inbox_setup_get_relay_list_state(setup), GH_INBOX_SETUP_RELAY_LIST_DONE,
+                 GH_INBOX_SETUP_RELAY_LIST_FAILED, GH_INBOX_SETUP_RELAY_LIST_SKIPPED,
+                 (g_get_monotonic_time() - t0) / 1e6, e ? e->message : "-");
+}
+
+/* V1: the re-review's P3, as written then (seed_list, no extra tags). */
+static void
+probe_v1(void)
+{
+  World w;
+  world_fresh_lists = TRUE;
+  const guint keys[] = { ALICE };
+  world_up(&w, keys, G_N_ELEMENTS(keys));
+  App *alice = &w.apps[ALICE];
+  seed_list(&w.x, ALICE, 10002, "wss://my-real-outbox.example");
+  drain();
+  g_test_message("PROBE V1: offer=%d", offer_of(alice));
+  gint64 t0 = g_get_monotonic_time();
+  g_autoptr(GhInboxSetup) setup = onboard(alice, w.x.url, TRUE);
+  probe_state("V1", setup, t0);
+  g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==, GH_INBOX_SETUP_RELAY_LIST_SKIPPED);
+  g_assert_cmpuint(probe_lists_on(&w.x, ALICE, NULL), ==, 1);
+  g_assert_cmpuint(probe_lists_on(&w.x, ALICE, "wss://my-real-outbox.example"), ==, 1);
+  g_object_run_dispose(G_OBJECT(setup));
+  world_down(&w);
+}
+
+/* V2: a target that is connected but never answers in time (no list anywhere). */
+static void
+probe_v2(void)
+{
+  World w;
+  world_fresh_lists = TRUE;
+  const guint keys[] = { ALICE };
+  world_up(&w, keys, G_N_ELEMENTS(keys));
+  App *alice = &w.apps[ALICE];
+  w.x.late_10002_ms = 25000;
+  gint64 t0 = g_get_monotonic_time();
+  g_autoptr(GhInboxSetup) setup = onboard(alice, w.x.url, TRUE);
+  probe_state("V2", setup, t0);
+  g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==, GH_INBOX_SETUP_RELAY_LIST_FAILED);
+  WireRelay *relays[] = { &w.e, &w.x };
+  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
+    g_assert_cmpuint(probe_lists_on(relays[i], ALICE, NULL), ==, 0);
+  g_object_run_dispose(G_OBJECT(setup));
+  world_down(&w);
+}
+
+/* V3: the user's list only on a target that answers after the deadline. */
+static void
+probe_v3(void)
+{
+  World w;
+  world_fresh_lists = TRUE;
+  const guint keys[] = { ALICE };
+  world_up(&w, keys, G_N_ELEMENTS(keys));
+  App *alice = &w.apps[ALICE];
+  seed_list(&w.x, ALICE, 10002, "wss://my-real-outbox.example");
+  w.x.late_10002_ms = 25000;
+  gint64 t0 = g_get_monotonic_time();
+  g_autoptr(GhInboxSetup) setup = onboard(alice, w.x.url, TRUE);
+  probe_state("V3", setup, t0);
+  g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), !=, GH_INBOX_SETUP_RELAY_LIST_DONE);
+  g_assert_cmpuint(probe_lists_on(&w.x, ALICE, NULL), ==, 1);
+  g_assert_cmpuint(probe_lists_on(&w.x, ALICE, "wss://my-real-outbox.example"), ==, 1);
+  g_assert_cmpuint(probe_lists_on(&w.e, ALICE, NULL), ==, 0);
+  g_object_run_dispose(G_OBJECT(setup));
+  world_down(&w);
+}
+
+/* V4: the user's list only on a target that answers late, but in time. */
+static void
+probe_v4(void)
+{
+  World w;
+  world_fresh_lists = TRUE;
+  const guint keys[] = { ALICE };
+  world_up(&w, keys, G_N_ELEMENTS(keys));
+  App *alice = &w.apps[ALICE];
+  seed_list(&w.x, ALICE, 10002, "wss://my-real-outbox.example");
+  w.x.late_10002_ms = 6000;
+  gint64 t0 = g_get_monotonic_time();
+  g_autoptr(GhInboxSetup) setup = onboard(alice, w.x.url, TRUE);
+  probe_state("V4", setup, t0);
+  g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==, GH_INBOX_SETUP_RELAY_LIST_SKIPPED);
+  g_assert_cmpuint(probe_lists_on(&w.x, ALICE, "wss://my-real-outbox.example"), ==, 1);
+  g_assert_cmpuint(probe_lists_on(&w.x, ALICE, NULL), ==, 1);
+  g_object_run_dispose(G_OBJECT(setup));
+  world_down(&w);
+}
+
+/* V5: a discovery relay H that holds the list answers late: no offer
+ * before it answered, none after. */
+static void
+probe_v5(void)
+{
+  World w;
+  world_fresh_lists = TRUE;
+  const guint keys[] = { ALICE };
+  world_up(&w, keys, G_N_ELEMENTS(keys));
+  App *alice = &w.apps[ALICE];
+  seed_list(&w.h, ALICE, 10002, "wss://my-real-outbox.example");
+  w.h.late_10002_ms = 6000;
+  const gchar *discovery[] = { w.e.url, w.h.url, NULL };
+  g_settings_set_strv(alice->settings, "discovery-relays", discovery);
+  gint64 t0 = g_get_monotonic_time();
+  guint offered_while_waiting = 0;
+  while (g_get_monotonic_time() - t0 < 5 * G_USEC_PER_SEC) {
+    g_main_context_iteration(NULL, FALSE);
+    offered_while_waiting += offer_of(alice) != GH_RELAY_LIST_OFFER_NONE;
+  }
+  spin_until(has_relay_list, alice, "the late list");
+  g_test_message("PROBE V5: offered while H was silent: %u times; after: offer=%d",
+                 offered_while_waiting, offer_of(alice));
+  g_assert_cmpuint(offered_while_waiting, ==, 0);
+  g_assert_cmpint(offer_of(alice), ==, GH_RELAY_LIST_OFFER_NONE);
+  world_down(&w);
+}
+
+/* V6: ADD_WRITE with a NEWER list (another client's edit) on target X. */
+static void
+probe_v6(void)
+{
+  World w;
+  world_fresh_lists = TRUE;
+  const guint keys[] = { ALICE };
+  world_up(&w, keys, G_N_ELEMENTS(keys));
+  App *alice = &w.apps[ALICE];
+  seed_relay_list(&w.e, ALICE, w.r.url, "read");             /* base: now - 3600 */
+  spin_until(has_relay_list, alice, "the read-only list");
+  NostrTags *tags = nostr_tags_new(0);
+  nostr_tags_append(tags, nostr_tag_new("r", "wss://edited-elsewhere.example", "read", NULL));
+  g_autofree gchar *newer = sign_event(ALICE, 10002, g_get_real_time() / G_USEC_PER_SEC - 60,
+                                       "", tags);
+  wire_relay_inject(&w.x, newer);
+  g_test_message("PROBE V6: offer=%d (ADD_WRITE=%d)", offer_of(alice), GH_RELAY_LIST_OFFER_ADD_WRITE);
+  gint64 t0 = g_get_monotonic_time();
+  g_autoptr(GhInboxSetup) setup = onboard(alice, w.x.url, TRUE);
+  probe_state("V6", setup, t0);
+  g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==, GH_INBOX_SETUP_RELAY_LIST_SKIPPED);
+  g_assert_cmpuint(probe_lists_on(&w.x, ALICE, "wss://edited-elsewhere.example"), ==, 1);
+  g_assert_cmpuint(probe_lists_on(&w.x, ALICE, NULL), ==, 1);
+  g_object_run_dispose(G_OBJECT(setup));
+  world_down(&w);
+}
+
+static gboolean
+probe_signing(gpointer data)
+{
+  GhRelayListSetup *l = gh_inbox_setup_get_relay_list(data);
+  return l && gh_relay_list_setup_get_state(l) >= GH_RELAY_LIST_SETUP_SIGNING;
+}
+
+static gboolean
+probe_setup_finished(gpointer data)
+{
+  GhInboxSetupState st = gh_inbox_setup_get_state(data);
+  GhRelayListSetup *l = gh_inbox_setup_get_relay_list(data);
+  return (st == GH_INBOX_SETUP_DONE || st == GH_INBOX_SETUP_FAILED) &&
+         (!l || gh_relay_list_setup_get_state(l) >= GH_RELAY_LIST_SETUP_DONE);
+}
+
+/* V7: another client publishes the user's first 10002 to target X while
+ * Groundhog's signer request is open (after the check). */
+static void
+probe_v7(void)
+{
+  World w;
+  world_fresh_lists = TRUE;
+  const guint keys[] = { ALICE };
+  world_up(&w, keys, G_N_ELEMENTS(keys));
+  App *alice = &w.apps[ALICE];
+  GhInboxSetupConfig config = { .accounts = alice->accounts, .account_relays = alice->relays,
+                                .settings = alice->settings, .offer_relay_list = TRUE };
+  g_autoptr(GhInboxSetup) setup = gh_inbox_setup_new(&config);
+  const gchar *chosen[] = { w.x.url, NULL };
+  g_autoptr(GError) error = NULL;
+  g_assert_true(gh_inbox_setup_start_full(setup, chosen, FALSE, TRUE, &error));
+  spin_until(probe_signing, setup, "the relay list signer request");
+  GhRelayListSetupState at = gh_relay_list_setup_get_state(gh_inbox_setup_get_relay_list(setup));
+  NostrTags *tags = nostr_tags_new(0);
+  nostr_tags_append(tags, nostr_tag_new("r", "wss://other-client.example", NULL));
+  g_autofree gchar *json = sign_event(ALICE, 10002, g_get_real_time() / G_USEC_PER_SEC - 5, "",
+                                      tags);
+  wire_relay_inject(&w.x, json);
+  spin_until(probe_setup_finished, setup, "the setup");
+  g_test_message("PROBE V7: injected at state=%d (SIGNING=%d); final relay list state=%d",
+                 at, GH_RELAY_LIST_SETUP_SIGNING, gh_inbox_setup_get_relay_list_state(setup));
+  guint kept = probe_lists_on(&w.x, ALICE, "wss://other-client.example");
+  g_test_message("PROBE V7: the other client's list still on X: %u", kept);
+  g_object_run_dispose(G_OBJECT(setup));
+  world_down(&w);
+}
 #endif
 
 #if GH_MLS_ADOPTED_KEY_PACKAGES
@@ -1260,6 +1479,15 @@ main(int argc, char **argv)
   g_test_add_func(KP_TEST("relay-list-write-relay-added"), test_relay_list_write_relay_added);
   g_test_add_func(KP_TEST("first-run-accounts-invite-each-other"),
                   test_first_run_accounts_invite_each_other);
+#endif
+#if GH_TEST_HAVE_INBOX_SETUP && !GH_MLS_ADOPTED_KEY_PACKAGES
+  g_test_add_func(KP_TEST("probe-v1"), probe_v1);
+  g_test_add_func(KP_TEST("probe-v2"), probe_v2);
+  g_test_add_func(KP_TEST("probe-v3"), probe_v3);
+  g_test_add_func(KP_TEST("probe-v4"), probe_v4);
+  g_test_add_func(KP_TEST("probe-v5"), probe_v5);
+  g_test_add_func(KP_TEST("probe-v6"), probe_v6);
+  g_test_add_func(KP_TEST("probe-v7"), probe_v7);
 #endif
   gint rc = g_test_run();
   mls_world_finish();
diff --git a/gnome/groundhog/tests/relay/wire-relay.h b/gnome/groundhog/tests/relay/wire-relay.h
index d48d4e8b..34971a3f 100644
--- a/gnome/groundhog/tests/relay/wire-relay.h
+++ b/gnome/groundhog/tests/relay/wire-relay.h
@@ -86,6 +86,7 @@ struct _WireRelay {
   GHashTable *withheld;      /* ids kept but served to nobody until released */
   gboolean withhold_new;     /* every event kept from now on is withheld */
   guint max_limit;           /* serve: a REQ's stored answer per filter, at most; 0: none */
+  guint late_10002_ms;       /* REVIEW PROBE: answer kind-10002 REQs this late */
   gboolean stall_pages;      /* serve: a REQ with an until is never answered (no EOSE) */
   guint stalled_reqs;
   gint64 max_future_seconds; /* serve: an EVENT dated further ahead of the clock is
@@ -359,6 +360,32 @@ wire_answer_req(WireRelay *relay, SoupWebsocketConnection *connection, const gch
   wire_send(connection, eose);
 }
 
+/* REVIEW PROBE: a relay that answers kind-10002 REQs late. */
+static G_GNUC_UNUSED gboolean
+wire_filters_want_kind(NostrFilters *filters, int kind)
+{
+  for (size_t i = 0; i < filters->count; i++)
+    for (size_t k = 0; k < nostr_filter_kinds_len(&filters->filters[i]); k++)
+      if (nostr_filter_kinds_get(&filters->filters[i], k) == kind)
+        return TRUE;
+  return FALSE;
+}
+
+typedef struct { WireRelay *relay; SoupWebsocketConnection *c; gchar *sub; } WireLate;
+
+static gboolean
+wire_late_fire(gpointer data)
+{
+  WireLate *late = data;
+  NostrFilters *filters = g_hash_table_lookup(wire_subs(late->c), late->sub);
+  if (filters && wire_open(late->c))
+    wire_answer_req(late->relay, late->c, late->sub, filters);
+  g_object_unref(late->c);
+  g_free(late->sub);
+  g_free(late);
+  return G_SOURCE_REMOVE;
+}
+
 /* A newly kept event goes to every live REQ that matches it. */
 static G_GNUC_UNUSED void
 wire_broadcast(WireRelay *relay, WireStored *stored)
@@ -475,7 +502,15 @@ wire_serve_message(WireRelay *relay, SoupWebsocketConnection *connection, const
     } else {
       NostrFilters *filters = g_steal_pointer(&req->filters);
       g_hash_table_replace(wire_subs(connection), g_strdup(sub_id), filters);
-      wire_answer_req(relay, connection, sub_id, filters);
+      if (relay->late_10002_ms && wire_filters_want_kind(filters, 10002)) {
+        WireLate *late = g_new0(WireLate, 1);
+        late->relay = relay;
+        late->c = g_object_ref(connection);
+        late->sub = g_strdup(sub_id);
+        g_timeout_add(relay->late_10002_ms, wire_late_fire, late);
+      } else {
+        wire_answer_req(relay, connection, sub_id, filters);
+      }
     }
     nostr_envelope_free(envelope);
     return TRUE;
```
