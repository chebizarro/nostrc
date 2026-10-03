# W26 slice B: NIP-25 reactions — independent review

**Branch:** `groundhog/w26-reactions` (tip 1f45077d, 4 commits)
**Base:** master 4df20d10
**Bead:** nostrc-191r (release blocker)
**Reviewer:** independent peer (W26 slice B)
**Date:** 2026-10-03

## Verdict: CHANGES-REQUIRED

Four findings need resolution before merge: a VERSION_MANIFEST factual error
about the schema bump, NIP-29 reaction removal using the wrong kind (admin
9005 vs author 5), missing sender-ownership checks on deletions, and a
double-add path from the picker. All are fixable without restructuring.

The overall design is clean and well-separated: 3 400 lines across 30+ files,
three backends (MLS, NIP-17, NIP-29), persistence, aggregation, and UI with
accessibility. Tests are present and pass. The MDK 0.11 interop case
(`white-noise-reactions`) covers both directions.

---

## Findings

### F1 — Blocker: VERSION_MANIFEST says "no schema change" but store schema is bumped v6→v7

**File:** `VERSION_MANIFEST.md` (new row at end of decisions table)
**Severity:** Blocker

The manifest row states:
> "no schema change, behind the existing reaction store (W25)"

But the code adds store schema v7 (`gh-store-schema.c:369`), a new
`reactions` table, bumps `GH_STORE_SCHEMA_VERSION` from 6 to 7
(`gh-store.h`), and updates the migration test (`test_store_marmot.c`)
accordingly. A forward migration means a store opened by this build cannot
be opened by an older build without the v7 migration.

**Failure scenario:** A reviewer or release engineer reads the manifest and
concludes no migration testing is needed, missing the v6→v7 forward migration.

**Fix:** Update the manifest row to say: "store schema 6 → 7 (forward
migration: reactions table; a store opened by this build cannot be opened by
an older one)" — matching the pattern of the v5→v6 entry above it.

---

### F2 — High: NIP-29 reaction removal uses kind 9005 (admin delete-event), not kind 5

**File:** `gnome/groundhog/src/app/gh-app-services.c:711-713`
**Severity:** High

When removing an own reaction in a NIP-29 group, `on_react()` calls
`gh_nip29_service_delete_event()`, which sends kind 9005 (admin
`delete-event`) via `admin_enqueue` with `NOSTR_PERMISSION_DELETE_EVENT`.
This operation is gated on the account having admin/mod permissions.

A regular member who reacted with 👍 and then taps the chip to un-react
gets `G_IO_ERROR_PERMISSION_DENIED` because they don't have the
`delete-event` permission. The reaction chip stays highlighted, but the
reaction is already removed from the local store (line 694-696 calls
`remove_own` before dispatching to the backend).

**Failure scenario:** Regular NIP-29 group member reacts, taps to un-react
→ local store removes it (chip disappears) but the deletion never reaches
the relay → other members still see the reaction → the user's view and
everyone else's diverge; on restart the reaction reappears from the relay.

**Fix:** Add a `gh_nip29_service_send_kind5()` (or reuse the reaction
template builder for kind 5 with an e-tag), gated on author-self-deletion
rather than admin permission. The NIP-29 relay should accept kind 5 from the
event author. Move the local `remove_own()` call after the send succeeds, or
roll it back on failure.

---

### F3 — Medium: Kind-5 deletion removes any reaction without checking sender ownership

**Files:**
- `gnome/groundhog/src/mls/gh-mls-service.c:2558-2568` (MLS)
- `gnome/groundhog/src/app/gh-app-outbox.c:277-283` (NIP-17)
- `gnome/groundhog/src/nip29/gh-nip29-service.c:1124-1132` (NIP-29)

**Severity:** Medium

In all three receive paths, a kind-5 deletion removes any reaction whose
rumor_id matches an e-tag, without verifying that the deletion sender
matches the reaction's original sender. Per NIP-09, only the author of an
event may delete it.

**Failure scenario:** In an MLS group, Alice reacts with 👍
(reaction_rumor_id = "abc"). Malicious Bob sends a kind-5 inner event with
e-tag "abc". Groundhog removes Alice's reaction from every group member's
view. Bob cannot legitimately delete Alice's reaction.

**Mitigation:** MLS groups already trust all members (invited/authenticated).
NIP-29 relays enforce authorization. NIP-17 seals authenticate the sender.
So exploitation requires a trusted member acting maliciously. Still, the
spec says author-only deletion.

**Fix:** In each kind-5 handler, look up the reaction in the store by its
rumor_id, compare the deletion sender against the reaction's sender_pubkey,
and skip the removal if they don't match. The reaction store would need a
new `gh_reaction_store_lookup_reaction()` or the summary's `own_reaction_id`
could be extended to look up by rumor_id.

---

### F4 — Medium: Picker can double-add the same emoji reaction

**File:** `gnome/groundhog/src/ui/gh-message-row.c:470-481` (on_emoji_picked)
**Severity:** Medium

The emoji picker always emits `add=TRUE`. If a user reacts with 👍 via the
picker, then opens the picker again and picks 👍 a second time, two
distinct reactions (different rumor_ids, same emoji) are created and sent.
The reaction bar then shows "👍 2" with both counted as the user's.

**Failure scenario:** User picks 👍 twice from the picker (the picker
closes and can be reopened) → the chip shows "👍 2" when only one was
intended. Removing it removes only the first; the second persists.

**Fix:** In `on_react()` (add path, `gh-app-services.c:732`), check
`gh_reaction_summary_own_reaction_id(summary, emoji)` before sending; if
non-NULL, the user already reacted with that emoji, so skip or toggle
(remove) instead. Alternatively, make the picker check the summary and
emit toggle semantics.

---

### F5 — Low: Restore loop re-inserts reactions into the database

**File:** `gnome/groundhog/src/store/gh-store-reactions.c:222-224`
**Severity:** Low

The restore loop comments say "Bypass delegate on restore (already
persisted)." but calls `gh_reaction_store_admit(model, reaction, ...)`,
which invokes the delegate's `admit()` callback — `delegate_admit()` does
`INSERT OR IGNORE INTO reactions`. The `OR IGNORE` silences the duplicate,
so correctness is preserved, but every stored reaction causes a wasted
SQLite statement on startup, and no transaction is held (each INSERT is
auto-committed individually).

**Failure scenario:** An account with 10 000 reactions pays ~10 000 wasted
INSERT + IGNORE statements at every startup. No data corruption.

**Fix:** Either temporarily NULL the delegate before the restore loop and
re-set it after, or add a `gh_reaction_summary_add_internal()` that
bypasses the delegate (the summary's `add` function already does dedup).

---

### F6 — Nit: VERSION_MANIFEST refers to "W25" reaction store but schema is new

**File:** `VERSION_MANIFEST.md` (same row as F1)
**Severity:** Nit

The manifest says "behind the existing reaction store (W25)." There is no
reaction store in W25 — it is introduced in this branch. The sentence is
misleading.

**Fix:** Remove the "behind the existing reaction store (W25)" clause.

---

## Areas verified clean

### MDK / White Noise compatibility
- The inner kind-7 rumor matches MDK v0.11 marmot-app: e-tag (target event
  id), p-tag (target author), k-tag (target kind as string), content =
  emoji. The `MARMOT_APP_EVENT_KIND_REACTION` and
  `MARMOT_APP_EVENT_KIND_DELETE` constants are imported from cgka_traits in
  the driver.
- The `white-noise-reactions` matrix case covers: Carol (MDK) reacts →
  Groundhog sees it; Alice (Groundhog) reacts → MDK sees it via sync.
- Removal is tested via Groundhog → MDK sync (reaction shows in sync
  results with kind 7). The driver's `send_deletion` command is wired.
- The emoji content handling (default "+", max 64 bytes) matches the MDK
  convention.

### Privacy
- No remote fetch for custom emoji: the emoji field is stored as-is; the
  test `test_custom_emoji_shortcode` verifies `:fire:` stays `:fire:`.
- Max emoji length (64 bytes) rejects overlong content; test coverage at
  `test_emoji_max_length`.
- Who-reacted exposure: the `reactors` GPtrArray in `GhReactionChip`
  contains sender pubkeys visible only to group members (already authorized
  by MLS/NIP-17 seal/NIP-29 relay). No information leaks beyond the group.
- NIP-17 reactions are gift-wrapped to every participant and a self-copy,
  using the existing `gh_outbox_send_reaction_room` → outbox seal pipeline.
- NIP-29 reactions carry the h-tag via `template_begin` → `template_add`
  (the h-tag is always the first tag in NIP-29 templates). The relay
  enforces authority.

### Correctness — verified clean areas
- **Aggregation after restart:** The `gh_store_reactions_attach` restore
  loop loads all reactions from the `reactions` table ordered by
  `created_at` ASC. The in-memory model is rebuilt correctly (with the F5
  caveat of wasted INSERTs).
- **Deduplication:** `gh_reaction_summary_add` deduplicates by
  reaction_rumor_id (linear scan, acceptable for per-message counts).
  `gh_reaction_store_admit` checks `reaction_to_target` hash table first.
  Test `test_store_dedup` covers this.
- **Own-reaction toggle:** The chip click handler reads `is_own` from the
  CSS class and emits toggle semantics. The local echo updates synchronously
  on the main loop, so the CSS class is correct for the next click. (The
  picker double-add is F4, a separate path.)

### Fire-and-forget MLS publish
- The reaction uses the same `marmot_create_message` +
  `marmot_save_created_message` path as messages. If the relay refuses, the
  reaction is stored in Marmot's internal storage and admitted locally but
  never delivered. There is no retry or error feedback.
- **Epoch change mid-send:** `marmot_create_message` encrypts for the
  current epoch. An epoch change after encryption doesn't prevent relay
  acceptance (the outer event is a regular nostr event). Recipients who have
  the epoch key can decrypt. This is the same behavior as regular messages.
- This is an acceptable trade-off for reactions (low-value messages); a
  retry mechanism would add complexity for little benefit. No finding.

### UI / HIG / Accessibility
- **Chips:** `GhReactionBar` uses `GtkFlowBox` with `GtkButton` children.
  Each chip has `GTK_ACCESSIBLE_PROPERTY_LABEL` set to
  `"emoji, count, including you"` (own) or `"emoji, count"` (other). Tab
  navigation works through the flow box.
- **Picker:** `GhReactionPicker` is a `GtkPopover` with a `GtkBox` of
  quick-set buttons (6 emoji) + a `GtkMenuButton` → `GtkEmojiChooser`.
  Each button has an accessible label. The popover auto-hides.
- **Keyboard:** Buttons are keyboard-focusable. Enter activates. The picker
  opens on right-click (secondary button gesture) and long-press (touch).
- **Screen reader:** The accessible labels are set correctly. The "Add
  reaction" button has a translatable label via `_("Add reaction")`.
- **Long-press on touch:** `GtkGestureLongPress` with `touch_only = TRUE`.

### Tests — spot-check
- `test_reactions.c` (11 tests): model creation, default emoji, store
  admit/dedup/own/remove/remove-own/aggregation, privacy shortcode and max
  length. All pass with `-Wall -Wextra -Werror`.
- `test_reaction_send.c` (3 tests): NIP-17 rumor builder for reactions and
  deletions, invalid input rejection. All pass.
- `test_nip29_group.c`: template_reaction exact JSON output verified.
- `test_mdk011_interop.c`: `white-noise-reactions` matrix case.
- `test_store_marmot.c`: schema migration v1→v7 and v6→v7.

### Merge risk with slices A and C
- **Slice C (polls):** The MLS inner-kind dispatch is a separate `if` block
  after the message processing (`if (!message && self->reactions ...)`). It
  checks `inner_kind == 7` and `inner_kind == 5` only. Poll kinds will use
  different values and won't conflict. Message rows are extended with a new
  `reaction_bar` child (separate from the message body). Low risk.
- **Slice A (conversation view):** The conversation view gains a
  `reactions` field and a `react_func` callback. These are new additions,
  not modifications of existing fields. Low risk.

---

## Build and test results

```
cmake -G Ninja -DBUILD_GROUNDHOG=ON -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug  ✓
ninja test-groundhog-reactions test-groundhog-reaction-send                       ✓
ctest -R "groundhog-reactions|groundhog-reaction-send"             2/2 passed     ✓
ctest -R "groundhog-nip29-service"                                 1/1 passed     ✓
ctest -R "groundhog-store-marmot"                                  1/1 passed     ✓
python3 scripts/check-unsequenced-args.py                          clean          ✓
```
