# W26 Slice E Review: relay-ux — CHANGES-REQUIRED

**Branch:** `groundhog/w26-relay-ux` (36e68684, 49f82ee0, 6364ae9b)  
**Base:** `master` 4df20d10  
**Beads:** nostrc-mi1z, nostrc-a4po, nostrc-q74l, nostrc-0srb  
**Reviewer:** independent peer review  
**Date:** 2026-10-03

## Verdict: CHANGES-REQUIRED

One blocker: inline 10050 (inbox) edits can never succeed because the
clobber check treats the user's own existing event as "another client's
list."

## Findings

### F1 — Blocker: 10050 inline edit always SKIPPED

**File:** `gnome/groundhog/src/app/gh-app-services.c:1147–1155`  
**Also:** `gnome/groundhog/src/app/gh-relay-list-setup.c:352–370`

`on_edit_relay()` for kind 10050 sets `base_id=NULL` and
`base_created_at=0` because GhAccountRelays stores no raw JSON for the
inbox event. In `other_list()`, this causes the user's own existing 10050
to be treated as "another client's list":

1. `g_strcmp0(event_id, NULL)` is never 0 (the event has a real id).
2. `created_at >= 0` is always TRUE for any published event.
3. So `other_list()` returns TRUE → `self->found = TRUE` → SKIPPED.

**Failure scenario:** User has a published 10050. They click the
add-relay button for an inbox relay in Preferences. The check finds their
own 10050 on a discovery relay and ends SKIPPED immediately. The toast
says "Another app published a newer relay list — not overwritten." No
signer request is ever made. The feature is completely non-functional for
any user who already has a published inbox list.

**Fix:** GhAccountRelays already stores `inbox.id` and
`inbox.created_at` in its `Revision inbox` struct. Expose them (e.g.
`gh_account_relays_get_inbox_event_id()` and
`…_get_inbox_created_at()`), then use them in `on_edit_relay()` for kind
10050 instead of passing NULL/0.

### F2 — Medium: No test covers the 10050 add/remove edit path

**File:** `gnome/groundhog/tests/app/test_inbox_setup.c`

`test_edit_never_clobbers_newer` only exercises kind 10002 edits. No test
exercises kind 10050 inline editing. This gap allowed F1 to ship.

**Failure scenario:** A regression in the 10050 edit path goes undetected.
A test calling `gh_relay_list_setup_start_edit` with kind=10050 and a
base_id/created_at matching the existing event, then delivering the same
event on the check scope, should end DONE (not SKIPPED).

### F3 — Medium: Missing VERSION_MANIFEST.md entry

**File:** `VERSION_MANIFEST.md`

AGENTS.md requires every version decision to be recorded in the manifest.
This change is folded into the unreleased 0.12.0 but the manifest has no
row for it.

**Fix:** Add a manifest row: "same | groundhog | 0.12.0 | No further bump
(unreleased 0.12.0): …" with a summary of the changes.

### F4 — Low: Clobber test doesn't cover same-timestamp edge case

**File:** `gnome/groundhog/tests/app/test_inbox_setup.c:1089`

`test_edit_never_clobbers_newer` delivers a newer event with
`base_created_at + 10`. The `>=` comparator in `other_list()` also
catches events with the exact same `created_at` (which could come from
another client that used the same second), but the test doesn't verify
this. Weakening `>=` to `>` leaves the test green.

**Failure scenario:** Another client publishes a list at the same second
as the base event. With `>` instead of `>=`, the clobber check would miss
it and overwrite.

**Fix:** Add a test case that delivers an event with
`created_at == base_created_at` and a different id, and asserts SKIPPED.

### F5 — Nit: Feature branch pushed to origin

The branch exists on origin at 49f82ee0 (second of three commits).
Confirmed no other refs were pushed; master and all other remote branches
are at their expected positions. Noted per brief.

## What passed

- **10002 relay list edit (mi1z):** The check-sign-check-publish pipeline
  is correctly implemented. Every target is checked before signing and
  again after the signer returns (F1 review). Additive edits preserve
  existing markers. The `GH_RELAY_LIST_OFFER_EDIT` mode properly skips the
  offer re-check in both `sign_list()` and `check_settled()`.

- **Onboarding discovery publishes (mi1z):** Consent-gated by the user
  clicking Publish (explicit action). Suggestion relays are added only
  when `adopt && discovery_is_empty()`. The privacy charter is respected:
  discovery relays learn relay lists (their purpose). Discovery settings
  save only user-chosen relays, not suggestions.

- **Signer wait / Cancel (a4po):** Cancel calls
  `gh_inbox_setup_cancel()` which cancels the GCancellable, truly
  revoking the signer request. In `on_signed()`, a cancelled setup's
  state is no longer SIGNING, so the signed result is discarded — no
  stale approval can apply. Signer vanish detection checks only
  `GH_SIGNER_AVAILABILITY_ABSENT`, which fires on NameOwnerChanged
  disappearance, not on slow approval. The daemon's 300s TTL expiry
  arrives through the normal async callback → FAILED.

- **Key package states (q74l):** WAITING, DECLINED, FAILED states are
  shown with honest copy. Try Again button is correctly gated on
  declined/failed (not waiting). The `retry-identity` action is wired
  through to `gh_mls_service_retry_identity()`.

- **Rename (0srb):** The menu item "Rename Group…" shows for MLS and
  NIP-29 groups only, hidden for DMs. It opens the conversation-info
  dialog which handles admin checks. DM-specific actions (pin, mute,
  delete, disappearing) are correctly disabled for groups.

- **Tests:** All 5 touched suites pass (inbox-setup, preferences,
  onboarding, conversation-menu, conversation-menu-gui). Spot-check
  confirmed: reverting the cancel button visibility makes
  `test_cancel_while_signing` fail as expected.

- **Build:** Clean build with `-DBUILD_GROUNDHOG=ON`, no warnings.
  `check-unsequenced-args.py` clean.

## Build verification

```
source /tmp/nostrc-macos27-env.sh
cmake -G Ninja -DBUILD_GROUNDHOG=ON -B build   # OK
ninja -C build                                   # OK (2554 targets)
ctest -R 'groundhog-(inbox-setup|preferences|onboarding|conversation-menu)' # 5/5 pass
python3 scripts/check-unsequenced-args.py        # clean
```

---

## Addendum — e6cd8bcd verified, APPROVE

**Fix commit:** e6cd8bcd  
**Date:** 2026-10-03

All five findings addressed. Verification below.

### F1 (Blocker → fixed): 10050 inline edit uses inbox base event

**Verified.** New `gh_account_relays_get_inbox_event_id()` and
`gh_account_relays_get_inbox_created_at()` expose the `Revision inbox`
fields. `on_edit_relay()` now branches on `kind == 10050` and calls them
instead of `base_event_info(NULL, …)`. The root cause (NULL base_id,
zero base_created_at) is eliminated.

The `test_edit_inbox_add` test delivers the user's own existing 10050
during both checks, asserts `state != SKIPPED` after each, and confirms
the full check → sign → check → publish pipeline reaches DONE with
one acceptance. `test_edit_inbox_remove` confirms the same pattern for
removal. Both tests exercise the new accessors and would regress to
SKIPPED or hang if the app-layer wiring reverted to NULL/0.

### F2 (Medium → fixed): 10050 add/remove tests added

**Verified.** Three new test cases: `edit-inbox-add`,
`edit-inbox-remove`, `edit-same-timestamp-skipped`. All pass.
`edit-inbox-add` is a thorough end-to-end: both pre-sign and post-sign
checks, signer round-trip, publish acceptance.

### F3 (Medium → fixed): VERSION_MANIFEST.md entry added

**Verified.** New row: "W26 slice E relay and signer UX … | groundhog |
0.12.0 | No further bump (unreleased 0.12.0): new Preferences UI, no
public API, storage or wire-format change."

### F4 (Low → fixed): NIP-01 same-timestamp tie-breaking

**Verified.** `other_list()` now handles equal timestamps with proper
NIP-01 ordering: the lower event id is authoritative. The logic at
line 370–374:

```c
if (created_at != self->base_created_at)
  return created_at > self->base_created_at;
return g_strcmp0(event_id, self->base_id) < 0;
```

This is correct: when the discovered id is lexicographically lower
(it wins the NIP-01 tie), `other_list()` returns TRUE → SKIPPED. When
our base id is lower (ours wins), it returns FALSE → proceed.

`test_edit_same_timestamp_skipped` arranges the discovered event to
have the lower id and asserts SKIPPED. Spot-check confirmed: changing
the return to `FALSE` (never skip on tie) causes the test to time out
waiting for a terminal state.

**Note:** The test only exercises the "discovered wins" branch. The
"base wins" branch (discovered has higher id → proceed) is not directly
tested but is covered by the `edit-inbox-add` and `edit-inbox-remove`
tests where the existing event has the same id as the base (line 368
returns FALSE before reaching the tie-break). The logic is
straightforward and visually verified.

### F5 (Nit → fixed): Remote branch deleted

**Verified.** `git branch -r | grep w26-relay` returns nothing after
`git fetch origin`.

### Build and test verification

```
source /tmp/nostrc-macos27-env.sh
cmake -G Ninja -DBUILD_GROUNDHOG=ON -B build            # OK
ninja -C build                                            # OK
ctest -R 'groundhog-(inbox-setup|preferences|onboarding  # 6/6 pass
          |conversation-menu|account-relays)'
python3 scripts/check-unsequenced-args.py                 # clean
scripts/linux-gate.sh --sanitizers /tmp/rv-w26-relay-ux   # 53/53 pass
```
