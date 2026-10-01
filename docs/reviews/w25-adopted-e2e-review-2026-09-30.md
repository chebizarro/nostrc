# W25 slice K review: Groundhog speaks the adopted profile end to end

- **Branch:** `marmot/w25-adopted-e2e`: `30dbb1b3` (libmarmot), `1ab02f1c` (groundhog), `f5b4a121` (harness), on master `543ca0b6`
- **Beads:** nostrc-lf62, nostrc-8u53, nostrc-lse9 (follow-ups nostrc-c0yo, nostrc-cyxb)
- **Brief:** /tmp/w25/K.md
- **Reviewer:** independent peer review (AGENTS.md), worktree `/tmp/rv-w25-adopted-e2e` at `f5b4a121`
- **Re-review (addendum, tip `de4290d5`):** **CHANGES-REQUIRED (narrow), one item (M4)**; every earlier finding is fixed. See the addendum at the end.
- **Original verdict:** **CHANGES-REQUIRED (narrow).** Fix M1 and M2 before merge. M3 is a merge-order plan; the rest can be follow-ups. No Blocker or High.

The core of the slice is sound. Before this branch, both producers wrote into one `d` slot, and an ACK of either format retired the other's keys. libmarmot now gives each profile its own slot, confirmation and expiry. Groundhog runs one publication per format. New Group and Group Info pick the format, and the service and libmarmot both enforce it. The MDK 0.11 cases are genuine live runs in both directions, including the GUI accept.

Two claims the branch makes do not hold in every case:
- The MDK 0.8 KeyPackage does not always stay the newest (M1, reproduced).
- New Group's honest-copy fallback can be bypassed by a silent downgrade at creation (M2).

## What I ran

| Check | Result |
|---|---|
| `cmake -G Ninja -DBUILD_GROUNDHOG=ON`, `ninja` (macOS 27 env) | clean |
| `ctest -R "marmot_\|gnostr-test-mls\|groundhog-store-marmot\|groundhog-mls\|groundhog-privacy-mls"` | 37/37 pass, including `marmot_test_kp_lifecycle`, `marmot_test_kp_profile`, `groundhog-mls-service`, `groundhog-mls-kp-lifecycle{,-adopted}`, `groundhog-mls-ui-gui`, `groundhog-privacy-mls` |
| `python3 scripts/check-unsequenced-args.py` | clean |
| MDK 0.8 matrix `groundhog-mdk-interop`, image built from this branch (`0.8.0-rv-w25k`) | pass: Groundhog publishes both formats and legacy does not regress |
| MDK 0.11 matrix, image built from this branch's driver-0.11 (`0.11.0-rv-w25k`) | control, groundhog-invites-mdk, mdk-invites-groundhog, mdk-invites-groundhog-gui, adopted-welcome, white-noise-welcome and adopted-commits **pass**; mdk09-probe **Skipped** (the expected XFAIL) |
| -gui rerun with `-V` | the real `GhMlsInvitesDialog` lists "Made by White Noise", "From npub1… · 2 members" and accepts. MDK sees 2 slots and picks the adopted one |
| `scripts/linux-gate.sh --sanitizers` (clean detached worktree at `f5b4a121`) | 52/52 sanitizer tests pass (ASAN/UBSAN/LSAN), including `groundhog-mls-service`, `groundhog-mls-kp-lifecycle{,-adopted}`, `groundhog-mls-ui` and `groundhog-privacy-mls`. No Docker volume created |
| Revert spot-checks | (1) profile-blind `confirm_impl` makes `test_kp_lifecycle` fail at :610, "confirming either profile preserves the other". (2) Join rotates every format: `formats-own-lifecycle` fails at :1071, `has_init_key(bob, a_ref1)`. (3) Group Info format check removed: `mls-ui-gui/new-group-formats` fails at :1451, `add_reason` visible. All restored |
| Scratch case for M1 (not committed; text below) | fails: `newest format after decline = ADOPTED` |

## Focus questions

1. **Key lifecycle with two slots.**
   - *Can an ACK of one profile delete the other's keys?* No. `confirm_impl` (libmarmot/src/kp_lifecycle.c:395-417) retires only older entries of the same profile, and Groundhog confirms the slot's own `ref` (`key_package_confirm(slot)`). Reverting the filter fails the libmarmot test.
   - *Expiry and hold.* Per-profile expiry (`marmot_key_package_next_expiry_for_profile`) drives a per-format hold verdict. A format with nothing to protect, such as the first adopted KeyPackage after an upgrade, is not held. The hold start is one shared persisted cursor.
   - *Restart and upgrade.* Per-format cursors (`""` keeps the MDK 0.8 key, so upgrades keep their state; `"adopted"` is new). A rotation persists as cursor 0.
   - *Join rotation.* Rotates the joined group's format (revert-tested).
   - *Bounded record.* It now protects each profile's newest confirmed entry.
   - *Gap.* The MDK 0.8 "companion" of an adopted publish is never recorded as due (M1).
   - *CMake rename.* A fresh configure and a stale cache both resolve to ON. `GH_MLS_ADOPTED_KEY_PACKAGES=1` reaches the service and the `groundhog` target, because libmarmot is added at CMakeLists.txt:402, before Groundhog at :468. No script or workflow still passes the old name. Nit N2.
2. **Privacy.**
   - *Charter.* Not documented there; only in gh-mls-service.h and a code comment (L1).
   - *Relay targets.* Both formats go only to 10002 write relays. `write-relays-only` checks both formats on W and H, and checks for no `30443` frame at all on R/X/G/E.
   - *Signer prompts.* Two concurrent signer requests per round (adopted, then MDK 0.8).
   - *Should legacy be opt-in?* Keeping it on by default is defensible while `GH_FEATURE_ENCRYPTED_GROUPS=0` and nostrc-cyxb is open, but it needs a sunset condition (L1).
3. **Interop.**
   - *Genuine runs.* All five cases are live runs over local relays against the pinned MDK v0.11.0 (`946e0547`).
   - *Driver selection.* driver-0.11's `select_key_package` mirrors marmot-app's `preferred_fresh_key_package_from_records` and `key_package_client_priority` (crates/marmot-app/src/key_package_records.rs:137-156, :180-249 at 946e0547): newest per `d` slot, another profile's slot passed over, `client` ranking. So real MDK 0.11 apps skip Groundhog's MDK 0.8 slot.
   - *GUI accept.* Genuine: the dialog's `mls-invites.accept`, then the "You joined" toast, then 445 both ways.
   - *"Newest wins" (nostrc-cyxb).* A real risk now. MDK 0.8 has no selection code of its own, the W23 0.8 driver is slot-blind (`max_by_key(created_at, id)`), and M1 shows the adopted event can stay the newest for up to 28 days.
4. **New Group UX.** The copy is plain and accurate ("newer/older format", "Some people use an older app version; this group will use the older format. Remove them to make a newer-format group instead."), and the mixed case offers a choice. Group Info refuses incompatible Adds before the service does (revert-tested), the service returns `PROFILE_MISMATCH`, and libmarmot's adopted Add parses ADOPTED only (groups.c:1553), so the path fails closed. Minor copy thought: "Keep Newer-Format People / Keep Older-Format People" reads awkwardly. "Keep People on Newer Apps / Keep People on Older Apps" says the same thing in the user's terms.
5. **Security.**
   - *Adopted Welcomes and joins.* They go through the unchanged slice E/H/I path. `mls_group_profile_check_entered` checks every leaf, ours included (mls_welcome.c:788), along with leaf signatures, proven members, an admin inviter, and sender == inviter (welcome.c:851-866). An MDK 0.8 KeyPackage therefore cannot enter an adopted group.
   - *The adopted classification skips the proof check.* That is correct, because adopted validation verifies the proof (kp_profile.c:477-487).
   - *Downgrade.* An existing group cannot be downgraded: the signed GroupContext fixes its profile. At creation it can (M2).
6. **Merge risk with L, M and N.** High textual overlap, and every one of them uses the hook K removes (M3).

## Findings

### M1 (Medium): the MDK 0.8 companion of an adopted publish is dropped when held or when it fails, so the adopted event stays the account's newest 30443 for up to 28 days

**Where.**
- gnome/groundhog/src/mls/gh-mls-service.c:5618-5619: `want = due[f] || (f == LEGACY && go[ADOPTED])`.
- :5597: `due` comes only from `rotate`/cursor.
- :5551: `slot->rotate = FALSE`.
- The documented promise is at gh-mls-service.h:43-47 and in commit `1ab02f1c` ("an adopted replacement brings an MDK 0.8 one along").

**Mechanism.** The companion is wanted only in the same pass in which the adopted one goes. Nothing records it (no `rotate`, no cursor 0), so:
- **Held.** Bob upgrades with an invitation pending. His first adopted KeyPackage is not held (nothing of that format to protect) and goes out. The MDK 0.8 companion has keys and is held (`HOLD_YES`). When the hold ends (decline, timer, cap), `key_package_maybe_publish` finds no format due, stops the hold and publishes nothing.
- **Failed.** The companion's signature is declined, or no relay accepts it, so the slot is `FAILED`. The retry finds the MDK 0.8 slot not due (its cursor is recent), so it is never retried.

Either way, the adopted event stays the newest kind 30443 until the MDK 0.8 KeyPackage's own lifetime ends (`GH_MLS_KEY_PACKAGE_LIFETIME`, 28 days) or an MDK 0.8 join. Two identical-looking concurrent signer prompts per round make the "declined" variant likely.

**Reproduced.** Scratch case in `test_mls_kp_lifecycle.c` (adopted build):
- `world_legacy_only = TRUE`; Alice invites Bob, and the invitation is pending.
- `world_legacy_only = FALSE; app_restart(bob)`; wait for the adopted publish.
- Log: `held=1 newest format after upgrade = ADOPTED`.
- Decline the invitation. Log: `held=0 newest format after decline = ADOPTED`.
- `g_assert_cmpint(newest_key_package_format(&w.w, BOB), ==, LEGACY)` fails (0 == 1).

**Impact.** A slot-blind reader gets the adopted KeyPackage and cannot invite the account during that window. The W23 MDK 0.8 driver is one, and 0.8-era White Noise or Amethyst may be (nostrc-cyxb).

**Fix.** When the adopted one goes, mark the MDK 0.8 slot due: set `kp[LEGACY].rotate = TRUE` and its cursor to 0, persisted. A held or failed companion then goes out when the hold ends, at the retry, or after a restart. Add the case above, and a declined-companion variant, to `formats-own-lifecycle`.

### M2 (Medium): silent adopted-to-MDK 0.8 downgrade at creation

**Where.**
- gh-mls-service.c:3201 (`op->adopted = adopted_count == n`, then silently MDK 0.8 when `legacy_count == n`).
- gh-mls-new-group-page.c:396 (`gh_mls_service_create_group_async` gets no expected format).

**Mechanism.** New Group's notice and choice come from the check-time lookups (`gh_mls_invitee_check_async`). At Create the service looks everyone up again and decides the format alone.

**Scenario.** Bob, a Groundhog account with both formats, is checked READY, so no notice appears. At Create, the lookup returns only Bob's MDK 0.8 event, because:
- his write relay withholds the adopted 30443 from the second REQ (an A1 relay operator, charter §2); or
- that relay misses the lookup deadline while another one with only the MDK 0.8 event answers; or
- that relay had refused the adopted event.

**Result.** An MDK 0.8-profile group, without the "older format" notice the page promises. The profile is fixed for the group's life:
- it can never add a White Noise 0.11 user (`PROFILE_MISMATCH`);
- it follows MDK 0.8 rules, so unproven members are admitted by default (nostrc-6ukh).

**Fix.** Pass the format New Group showed to the service (for example a `GhMlsKeyPackageFormat` or `require_adopted` argument). Refuse with `PROFILE_MISMATCH` or `MIXED_PROFILE` rather than falling back, so the page re-checks and shows the notice. Add a service or UI case where the create-time lookup lacks the adopted event.

### M3 (Medium, merge): L, M and N call the hook this branch removes; error codes collide

**Removed hook.** `gh_mls_service_test_create_adopted_group_async` is deleted in `f5b4a121`. New uses not on this branch:
- L: `tests/mls/test_mls_files.c:716`
- M: `tests/mls/test_mls_service.c:2712` and `tests/mls/test_mdk011_interop.c:998`
- N: `tests/mls/test_mdk011_interop.c:1126`
- plus each slice's copies of the calls K ported.

Whichever slice lands after K fails to build: an implicit declaration under `-Werror`, and `test_mls_service.c` is in the default test build.

**Trial merges.** `git merge-tree` of K with each sibling conflicts:
- L: 7 files, including gh-mls-service.c/.h, test_mdk011_interop.c and test_mls_ui.c.
- M: 9 files, including gh-mls-copy.c, mls-world.h and test_mls_service.c.
- N: 5 files.

**Error codes.** `GhMlsServiceError` gains `EPOCH_CHANGED` (L), `ADDRESS_TAKEN` (M) and `MIXED_PROFILE`/`PROFILE_MISMATCH` (K), all after `FORGED_IDENTITY`. Keep every code and its `gh_mls_error_copy` case.

**Behaviour change for siblings.** After K, Groundhog accounts in a test world make adopted groups with each other by default. Any sibling test that assumes an MDK 0.8 group between Groundhog accounts needs `world_legacy_only`.

**Recommendation.** Merge K first. Then rebase L, M and N, porting each hook call to `gh_mls_service_create_group_async`; Groundhog invitees publish adopted KeyPackages now. Where a case needs an MDK-made KeyPackage, keep a slice-local test hook.

### L1 (Low): the privacy cost is not in the charter, and legacy has no sunset

**Where.** docs/designs/groundhog-privacy-ux-charter-2026-09-28.md:189. "What anyone can see" still says "Your KeyPackages (kind 30443): that you support MLS". The brief asked to document the cost; it is only in gh-mls-service.h:30-36 and a comment.

**What is missing.** Besides the second event:
- the pair is a client fingerprint: two 30443 slots, `created_at` n and n+1, the same relays (today only Groundhog does this);
- every rotation needs two signer approvals;
- the account stays invitable into MDK 0.8-profile groups.

**Fix.** Amend the charter, as the 2026-10-01 nostrc-0bdg amendment did, with this row and a sunset condition: drop or make opt-in the MDK 0.8 KeyPackage once nostrc-cyxb shows 0.8-era clients are gone or slot-aware. A Preferences opt-out is reasonable.

### L2 (Low): READY (both formats) does not check the MDK 0.8 KeyPackage's proof

**Where.** gh-mls-invitee.c:12-14.

**Scenario.** An invitee has both formats, but their MDK 0.8 KeyPackage lacks the account proof; the row says "Ready to invite". Another invitee is MDK 0.8-only, so the group becomes MDK 0.8. The account requires proofs, so Create fails in `invitees_ready` with "Nobody was invited" (`MARMOT_ERR_KEY_PACKAGE_IDENTITY`) instead of the row saying NEEDS_UPDATE beforehand. Today only Groundhog publishes both formats, always with the proof.

**Fix.** Check the MDK 0.8 event's proof in `classify` too.

### N1 (Nit): join rotation is keyed on the joined group's profile, not the consumed KeyPackage's

**Where.** gh-mls-service.c:4528.

The two are the same for every inviter libmarmot or MDK can build. A crafted MDK 0.8-profile Welcome to our adopted KeyPackage would rotate the other slot instead. There is no key exposure, since both KeyPackages are last-resort. libmarmot already knows the consumed ref (`MarmotKpUse`); exposing its profile would make the rotation exact.

### N2 (Nit): the old CMake option is silently overridden and never cleared

**Where.** libmarmot/CMakeLists.txt:19-23.

The STATUS line prints on every configure, because the stale entry is never unset. An explicit `-DMARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER=OFF` (a packager wanting MDK 0.8 only) turns into ON with only that STATUS line. Suggest `message(WARNING)` when the old entry is OFF and the new one ON, then `unset(MARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER CACHE)`.

### N3 (Nit): `deactivate_key_packages` is still account-wide

**Where.** libmarmot/src/credentials.c:978.

Making the MDK 0.8 KeyPackage marks the live adopted one `active = false`, and the reverse. So `marmot_gobject_key_package_is_active()` and the seeding order misreport. Deletion is unaffected: the lifecycle record decides that. Fix with per-profile deactivation, or a doc note.

## Scratch case for M1 (reviewer-only; added to `test_mls_kp_lifecycle.c` under `#if GH_MLS_ADOPTED_KEY_PACKAGES`)

```c
world_split_lists = TRUE;
world_legacy_only = TRUE;                       /* before the upgrade */
world_up(&w, (const guint[]){ ALICE, BOB }, 2);
/* wait_published(alice), wait_published(bob), accept_contact(alice, BOB) */
create_attempt(alice, "One", (const guint[]){ BOB }, 1, &error);   /* MDK 0.8 group */
spin_until(invites_at_least, &(InvitesWait){ bob, 1 }, "the invitation");
g_autofree gchar *wrapper = invite_from(bob, ALICE);
world_legacy_only = FALSE;                      /* the upgrade */
app_restart(bob);
spin_until(format_count_reached,
           &(FormatCountWait){ &w.w, BOB, GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED, 1 }, "adopted");
/* held=1, newest = ADOPTED */
gh_mls_service_decline_invite(bob->service, wrapper, &error);
/* settle; held=0, newest still ADOPTED */
g_assert_cmpint(newest_key_package_format(&w.w, BOB), ==, GH_MLS_KEY_PACKAGE_FORMAT_LEGACY);  /* fails */
```

## Versioning

libmarmot and Groundhog changes are folded into their unreleased 0.12.0 rows (VERSION_MANIFEST.md). The rows correctly say marmot-gobject and gnostr need no bump: neither calls the adopted producer (nostrc-ruwy still blocks gnostr). The harness and driver changes are tests only. This complies with AGENTS.md.

## Addendum (re-review of fixes `0fd2ad8a` libmarmot and `de4290d5` Groundhog; tip `de4290d5`)

- **Final verdict:** **CHANGES-REQUIRED (narrow): one item, M4.**
- M1, M2, L1 (except M4), L2 and N1-N3 are fixed and revert-tested, as are the two bugs the author found.
- M4 is new and was introduced with the L1 withdrawal: its deletion request does not reach the newest MDK 0.8 KeyPackage, yet its keys are still retired. The fix is small. With M4 fixed and its test added, this is **APPROVE-WITH-NITS**, and a further re-review is not needed.
- M3 (merge order with L, M and N) stands as written.

### What I ran (worktree rebased onto `de4290d5`)

| Check | Result |
|---|---|
| Build (`BUILD_GROUNDHOG=ON`) | clean |
| `ctest` marmot, MLS, privacy and preferences subset | 42/42 pass, including `groundhog-mls-kp-lifecycle{,-adopted}`, `groundhog-mls-service`, `groundhog-mls-ui-gui`, `groundhog-preferences`, `groundhog-privacy-{static,e2e,summary}` |
| MDK 0.8 and 0.11 matrices (images rebuilt from the tip's drivers) | MDK 0.8: all 11 cases pass (`-V`). MDK 0.11: control, groundhog-invites-mdk, mdk-invites-groundhog, -gui, adopted-welcome, white-noise-welcome and adopted-commits **pass**; mdk09-probe **Skipped** (the expected XFAIL) |
| `scripts/linux-gate.sh --sanitizers` (clean detached worktree at `de4290d5`) | 52/52 sanitizer tests pass; no Docker volume created |
| My M1 scratch case, unchanged | **now passes**: `held=1 … after upgrade = ADOPTED`, then `held=0 … after decline = LEGACY` |
| Revert spot-checks | All fail as expected:<br>- M2 service guard off: `mls-service/create-in-format` (:176) and `mls-ui-gui/new-group-format-changed` ("Group Not Created" never shown)<br>- withdrawal OK branch removed: `legacy-switched-off`, keys never retired<br>- mid-round recheck removed: `legacy-switched-off`, withdrawal never sent<br>- N3 re-activation removed: `test_kp_lifecycle:691`<br>- N1 `kp_used` record removed: `test_kp_lifecycle:611`<br>All restored |
| N2 | `-DMARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER=OFF` gives one WARNING naming the new option. The next configure says nothing, and the old entry is gone from `CMakeCache.txt` |

### Item by item

- **M1: fixed.** When the adopted KeyPackage goes out, `key_package_maybe_publish` marks the MDK 0.8 slot due: `rotate`, plus its cursor set to 0, which persists (gh-mls-service.c, the `review M1` block). A companion that is held, refused or interrupted therefore goes out at the hold's end, the retry or the next start. My original case passes, and the author's `upgrade-companion-held` covers it.
- **M2: fixed at both layers.**
  - **Service.** `gh_mls_service_create_group_in_format_async` refuses with `FORMAT_CHANGED` when the create-time lookups no longer give every invitee a KeyPackage of the expected format. There is no fallback.
  - **Page.** New Group passes the format it showed (`self->format`, computed with the notice). On `FORMAT_CHANGED` it runs the invitee check again, so the page re-renders what Create would now make.
  - **Remaining callers.** New Group is the only production caller of create; the format-less `gh_mls_service_create_group_async` is left to tests.
  - Both tests are revert-checked.
- **L1: switch and charter are mostly right; M4 below.**
  - **Setting.** `mls-legacy-key-packages` (default true, `check_privacy.py` updated) appears in Preferences › Privacy › Conversations, inside the encrypted-groups feature gate.
  - **Withdrawal.** Turning it off sends one kind-5 request:
    - tags: one `a` = `30443:<pubkey>:<d of the MDK 0.8 slot>` and `k` = `30443`; empty content;
    - signed by the account and checked after signing (kind 5, author);
    - sent only to the KeyPackage write relays, with the own-list-publish AUTH purpose;
    - held, like a replacement, while invitations are pending.
  - **Keys are retired only on the request's first relay OK.** A refused request keeps them (tested). `marmot_key_package_retire_profile` touches only the MDK 0.8 profile.
  - **Privacy of the request.** Acceptable: it names only the slot address the KeyPackage already published. But the request is itself a lasting public event saying that this account offered, and then dropped, the older format. The charter's "copies may persist" covers the KeyPackage, not the request. Nit: say so.
  - **Charter amendment (§2.2 table row and 2026-10-01 note, §settings table).** Accurate on cost, default, behaviour, ACK-tied deletion, the hold, re-enabling, and sunset criteria tied to nostrc-cyxb. One exception: "only the current format is published" does not hold today (M4).
- **L2: fixed.** For an invitee with both formats, the MDK 0.8 event's proof is now judged too. When it is unusable and proofs are required, the invitee becomes READY_ADOPTED_ONLY (gh-mls-invitee.c).
- **N1: fixed.** `marmot_kp_lifecycle_consumed` records the spent profile (`kp_used`) inside the accepting transaction, clearing it when unknown. `accept_invite` rotates that format and falls back to the group's profile.
- **N2: fixed** (see the table).
- **N3: fixed.** After the account-wide deactivation, the other profile's newest KeyPackage is re-activated, so `active` is per profile.
- **The author's two bugs: fixed and covered.**
  - **Withdrawal OK.** The request's OK reaches `key_package_update` with the slot as its data. It is now routed to `key_package_withdrawn` (first OK only) before the KeyPackage-confirmation path, which would otherwise have recorded the request's id as the KeyPackage and marked the slot published without retiring anything. A failed withdrawal is not reported as a KeyPackage `FAILED` and is retried.
  - **Switch flipped mid-round.** `on_legacy_key_packages_changed` sets `key_package_recheck`, which `key_package_done` honours once the round ends. A KeyPackage published by the round in flight is then covered by the following withdrawal, subject to M4.

### New findings

#### M4 (Medium): the withdrawal's `created_at` can be older than the MDK 0.8 KeyPackage it should delete, so a NIP-09 relay keeps that KeyPackage while Groundhog deletes its keys

**Where.** gh-mls-service.c `key_package_withdraw`: `nostr_event_set_created_at(event, now_s(self))`, with only an `a` tag.

**Mechanism.**
- libmarmot gives every KeyPackage `created_at = max(now, last + 1)` per account (`marmot_kp_lifecycle_created_at`). The MDK 0.8 one is always the adopted one + 1, so right after a round it is at least a second in the future, and bursts of rotations push it further.
- NIP-09 deletes the versions of an `a` address only up to the request's `created_at`.
- Groundhog retires the keys on the request's first OK.

**Reproduced.** Scratch case `review-withdrawal-covers`, reviewer-only: three quick `rotate()`s, then the switch off. Log: `newest MDK 0.8 KeyPackage created_at=1790889528 deletion created_at=1790889521`, and `del >= newest_kp` fails.

**Triggers.** The ones that matter:
- a withdrawal right after a round: the mid-round flip path with an auto-approving signer;
- several joins in quick succession;
- the system clock stepped back, which leaves libmarmot's `last_created_at` ahead for the size of the step.

**Impact.** A compliant relay keeps the newest MDK 0.8 KeyPackage after the user switched the format off:
- the fingerprint the user wanted gone stays published;
- older apps keep inviting through it, and every such Welcome fails, because its keys are already deleted. That breaks the "keys deleted only once the relays were asked to delete it" contract the charter states.

The fake relay does not apply NIP-09, so `legacy-switched-off` cannot see this.

**Fix.**
- Give the request `created_at` ≥ the newest MDK 0.8 KeyPackage's `created_at`. For example, have libmarmot expose the slot's last `created_at`, or take the request's from `marmot_kp_lifecycle_created_at`.
- Additionally add `e` tags for the slot's known event id(s): NIP-09 `e` deletions are not bounded by `created_at`.
- Assert in the test that the request covers the newest MDK 0.8 event, and ideally teach the fake relay NIP-09 `a`/`e` deletion.

#### L3 (Low): any `marmot_key_package_slot` error is treated as "never published"

**Where.** `key_package_withdraw`: the `!= MARMOT_OK` branch calls `key_package_withdrawn()` straight away.

**Scenario.** A transient storage error while reading `kp_slot` retires the MDK 0.8 keys with no deletion request sent, while the KeyPackage stays published. Its Welcomes then fail. This breaks the ACK-tied rule.

**Fix.** Treat only `MARMOT_ERR_STORAGE_NOT_FOUND` as "nothing to withdraw". On any other error, leave the keys and retry.

#### Nits

- N4: in a build without the adopted producer, the Preferences switch is still shown and still toggles, but has no effect (the MDK 0.8 KeyPackage is kept). Hide or disable it there.
- N5 (charter): mention that the deletion request itself is public and lasting (see L1 above).
