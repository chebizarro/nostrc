# W26 Slice A Review: White Noise DMs (`groundhog/w26-wn-dms`)

**Branch:** `groundhog/w26-wn-dms` (7 commits: 78643775 … ee8844fb on master 4df20d10)
**Reviewer:** Independent peer review
**Date:** 2026-10-03
**Verdict:** APPROVE-WITH-NITS

## Summary

This slice presents White Noise's 2-member Marmot (MLS) groups as ordinary
DMs in Groundhog, adds protocol badges (NIP-17 / Marmot), a configurable
default-DM-protocol preference, an Add Contact dialog, NIP-17 fallback,
DM request rules, and co-member invitee expansion.

The implementation is solid. The DM shape detection (`!group->name &&
members == 2`) matches WN's logic in marmot-app `groups.rs`
(`set_direct_member_ids_from_roster`: `self.profile.name.trim().is_empty()`
and roster size 2). The name normalisation at line 1841 (`g_strdup(g->name
&& *g->name ? g->name : NULL)`) ensures empty strings become NULL, which is
functionally equivalent to WN's trim-is-empty for all practical input.
Groundhog-created DMs pass `""`, stored as NULL, matching the shape WN
expects.

All 90 CTest cases pass (5 expected skips). The `test_dm_shape` test fails
when `group_is_dm()` is reverted to `return FALSE` (spot-checked: assertion
at test_mls_ui.c:609). Privacy, preferences, and privacy-summary tests all
pass. The `check-unsequenced-args.py` script reports clean.

## Findings

### M1 — `test_white_noise_dm` not registered in CTest (Medium)

**File:** `gnome/groundhog/CMakeLists.txt:1879`
**Failure scenario:** The `test_white_noise_dm` interop case (test_mdk011_interop.c:1601)
exists in the test binary and passes when run manually, but is missing from
the `foreach(groundhog_mdk011_case ...)` loop at CMakeLists.txt:1879.
CI never runs this case, so a regression in Marmot DM interop with MDK 0.11
would go undetected.

**Fix:** Add `white-noise-dm` to the foreach list, after `white-noise-media`:

```cmake
foreach(groundhog_mdk011_case control groundhog-invites-mdk mdk-invites-groundhog
        mdk-invites-groundhog-gui adopted-welcome white-noise-welcome white-noise-media
        white-noise-dm adopted-commits routing-rotation concurrent-commits mdk09-probe)
```

### L1 — Silent NIP-17 fallback (Low)

**File:** `gnome/groundhog/src/ui/gh-new-message-dialog.c:830`
**Failure scenario:** The user's default-dm-protocol is "marmot" and the
peer's KeyPackage lookup fails. `marmot_dm_done()` falls back to NIP-17 with
only a `g_debug` log. The brief says "with honest copy." The protocol badge
on the resulting conversation row does show "Private conversation" (NIP-17)
vs "Marmot private message," which gives some indication post-creation, but
there is no toast or in-dialog notice at the moment of fallback. A user who
chose Marmot as default would not learn the downgrade happened until they
inspect the badge.

This is marked Low because the badge system does ultimately expose the
protocol. A future enhancement could add a toast ("Could not create a Marmot
DM; opened a NIP-17 conversation instead") in `marmot_dm_done()`.

### N1 — Whitespace-only group name edge case (Nit)

**File:** `gnome/groundhog/src/mls/gh-mls-service.c:1206`
**Failure scenario:** A hypothetical whitespace-only name (e.g. `" "`) would
not be normalised to NULL by the name assignment at line 1841 (`*g->name`
is truthy for `' '`), so `group_is_dm()` would return FALSE, while WN's
`name.trim().is_empty()` returns true. In practice, no client creates
whitespace-only names, and the New Group dialog's `entry_text()` strips
whitespace to NULL. This is purely theoretical.

### N2 — `add_contact_attachment_free` does not disconnect `contact-added` signal (Nit)

**File:** `gnome/groundhog/src/ui/gh-add-contact-dialog.c:289`
**Failure scenario:** The `g_signal_connect(dialog, "contact-added", ...)` at
line 289 connects to the attachment data, but the dialog is transient and dies
before the attachment. In practice, the dialog's destruction disconnects it
via GObject signal cleanup. No leak or UAF, just a stylistic note that the
connection could use `g_signal_connect_object` for symmetry.

## Focus Area Assessment

### 1. WN DM Shape — ✅ Correct
Groundhog's detection (`!group->name && members == 2`) and creation (`""`
name → NULL via normalisation) match WN's `set_direct_member_ids_from_roster`
logic. The MDK 0.11 interop test (`test_white_noise_dm`) verifies both
directions: MDK→Groundhog and Groundhog→MDK, with message round-trips.

### 2. Request Rules and Privacy — ✅ Correct
- DM Welcomes from strangers go to Requests (`gh_conversation_get_is_request`
  returns TRUE for MLS DMs without prior acceptance).
- Auto-accept works: a second DM from an already-accepted contact is
  auto-accepted in `group_list_room()`.
- PT-8 maintained: the Add Contact dialog doesn't fetch profiles until the
  conversation is accepted; NIP-05 lookup requires explicit consent.
- No network before user acts: `open_room` is purely local.

### 3. Default Protocol Preference and Fallback — ✅ Correct (with L1 nit)
- `default-dm-protocol` GSettings key with schema choices, Preferences row,
  and `default_dm_is_marmot()` consumer.
- Fallback path works: `marmot_dm_done()` creates NIP-17 on failure.
- User cannot be silently downgraded without the badge revealing it (L1).

### 4. Service Wiring — ✅ Correct
- `app_create_marmot_dm` goes through `gh-app-services.c` → `mls_ui_service()`
  → `gh_mls_service_create_group_async` with `""` name. The real MLS service
  and the account's write relays are used.
- The `create_group` name validation change (`!*name` removed) only lets
  empty-string through. NULL is still rejected. The New Group UI prevents
  empty via `entry_text()` returning NULL for empty/whitespace. No weakening
  of group-name validation for regular groups.

### 5. Conversation Model — ✅ Correct
- One person with both NIP-17 and Marmot DMs shows as two separate
  conversations, each with its own protocol badge.
- Search, notifications, and mute work via the existing `GhConversation`
  infrastructure (separate room IDs).
- Badges are accessible: tooltip text, `GTK_ACCESSIBLE_PROPERTY_LABEL`.

### 6. Tests — ✅ Adequate (with M1 fix needed)
- `test_dm_shape` covers: DM detection, request routing, auto-accept, named
  groups NOT being DMs, co-member contacts, protocol badge strings, default
  protocol setting, recipient input parsing.
- `test_white_noise_dm` covers both directions with message round-trips but
  is not in CTest (M1).
- Spot-check: reverting `group_is_dm()` to FALSE causes test_dm_shape to fail
  at line 609.

### 7. Merge Risk — Low
- **B (reactions):** Touches `gh-mls-service.c` in `process_event` (inner-kind
  routing) and struct fields, plus `gh-app-services.c` (reaction store wiring).
  A's changes are in `group_list_room`, `create_group`, `invite_of`, and
  `app_create_marmot_dm` — different code regions. No conflicts expected.
- **C (polls):** Touches `gh-mls-service.c` in `process_event` and struct
  fields. Same separation. No conflicts expected.
- **E (preferences, header):** Would overlap with `gh-preferences-dialog.blp/c`
  if it adds rows near the DM protocol row. Low risk — positions are additive.
- Shared .ui regeneration files (gh-conversation-info-dialog.ui etc.) may have
  trivial whitespace conflicts; they should be regenerated from .blp anyway.
