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
