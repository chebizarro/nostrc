# W26 slice C: polls — review

**Branch:** `groundhog/w26-polls` (576ae6f2, 58ceeccc, 7fe40e0a) on master 4df20d10  
**Bead:** nostrc-a36s  
**Reviewer:** independent peer (W26 review protocol)  
**Date:** 2026-10-02

## Verdict: CHANGES-REQUIRED

Two blockers must be fixed before merge. The wire-format unit vectors are
byte-for-byte correct, but the brief required a live Docker driver interop
case and none exists. A heap-buffer-overflow in `valid_display_text` is
triggered by invalid UTF-8 in a fuzzed inner event and is exploitable by
any MLS group member.

---

## Build & test

| Gate | Result |
|------|--------|
| macOS cmake + ninja (BUILD_GROUNDHOG=ON) | ✅ configures and builds |
| `test-groundhog-mls-poll` (25 tests) | ✅ all pass |
| `check-unsequenced-args.py` | ✅ clean |
| ASan + UBSan (`test-groundhog-mls-poll`) | ✅ all 25 pass |
| Quick fuzz (100k iterations, ASan) | ❌ **heap-buffer-overflow** (F1) |
| Spot-check revert (5 of 25 tests) | ✅ all fail when fix reverted |

---

## Findings

### F1 — Blocker: heap-buffer-overflow in `valid_display_text` on invalid UTF-8

**File:** `gnome/groundhog/src/mls/gh-mls-poll.c:81`  
**Severity:** Blocker  
**Failure scenario:** A group member (or a corrupted relay delivery) sends
a poll inner event whose option label or question contains an invalid
UTF-8 sequence — e.g. a byte like `0xC0` near the end of a short string.
`g_utf8_next_char()` uses the `g_utf8_skip` table to advance by the
expected multi-byte count, reading past the NUL terminator and into
unallocated heap. ASan confirms a 1-byte OOB read. On a release build
this is undefined behaviour; it can crash or leak heap contents.

**Root cause:** `valid_display_text` calls `strlen` (line 76) and then
iterates with `g_utf8_next_char` (line 81) without first validating
that the string is well-formed UTF-8. `g_utf8_next_char` is documented
as requiring valid UTF-8.

**Fix:** Add `if (!g_utf8_validate(s, len, NULL)) return FALSE;` between
the `strlen` call and the `g_utf8_next_char` loop (after line 77). MDK
is immune because Rust `&str` is always valid UTF-8 by construction.

---

### F2 — Blocker: no live MDK 0.11 Docker driver interop case

**File:** `gnome/groundhog/tests/mls/test_mls_poll.c:442–701`  
**Severity:** Blocker (brief requirement)  
**Failure scenario:** The brief (W26 slice C) explicitly requires "Add an
MDK 0.11 matrix case: MDK poll → Groundhog votes → MDK tallies;
Groundhog poll → MDK votes." The existing MDK 0.11 interop driver
(`tests/interop/mdk/driver-0.11/`, `test_mdk011_interop.c`) runs live
round-trips through Docker; no poll case was added to it. The 6
"MDK v0.11 wire-format matrix" tests (tests 20–25) are unit-level
vectors with hand-constructed JSON — they exercise the parser, not the
MLS+network round-trip. They are good but do not fulfil the brief.

**What the vectors prove:** tag names, field order, option-id encoding,
kind numbers, single/multi choice, ends_at, vote `["e",…]` and
`["response",…]` tags all match MDK v0.11.0's `crates/traits/src/polls.rs`
byte-for-byte. I re-derived a vector manually and it checks out.

**What is missing:** A case in `test_mdk011_interop.c` that starts a
Docker MDK peer, creates a poll on one side, votes on the other, and
verifies tallies round-trip through MLS encryption + relay delivery.

---

### F3 — Medium: incoming votes accepted after poll deadline

**File:** `gnome/groundhog/src/mls/gh-mls-service.c:2543`  
**Severity:** Medium  
**Failure scenario:** MDK's `validate_poll_response` rejects votes whose
`created_at` exceeds the poll's `ends_at`. Groundhog's `process_event`
dispatches incoming votes to `gh_mls_poll_apply_vote` without checking
the poll deadline or the vote's `created_at` against the poll's
`created_at`. A late or malicious vote (created_at > ends_at) from
another group member is counted. This causes inconsistent tallies between
MDK and Groundhog nodes viewing the same poll.

**Fix:** Before calling `gh_mls_poll_apply_vote` in `process_event`,
check `gh_mls_poll_is_open(poll, gh_message_get_created_at(message))`
and also that `gh_message_get_created_at(message) >= gh_mls_poll_get_created_at(poll)`.

---

### F4 — Medium: memory leak in Create Poll dialog validation

**File:** `gnome/groundhog/src/ui/gh-create-poll-dialog.c:39`  
**Severity:** Medium  
**Failure scenario:** `is_valid()` calls `g_strstrip(g_strdup(q))` inside
a condition expression. The `g_strdup` allocation is never freed. This
function runs on every keystroke (`on_text_changed → update_sensitivity
→ is_valid`), leaking ~40 bytes per keystroke.

**Fix:** Use a `g_autofree gchar *stripped = g_strdup(q);
g_strstrip(stripped);` before the check.

---

### F5 — Low: poll state not cleared on message withdrawal within a session

**File:** `gnome/groundhog/src/mls/gh-mls-service.c` (polls hash table)  
**Severity:** Low  
**Failure scenario:** If a message carrying a poll or vote is withdrawn
(W25 convergence: a competing Commit's branch drops it), the in-memory
`polls` hash table retains the withdrawn poll or vote tally. On
restart, `rebuild_polls` scans the timeline and the withdrawn message
is absent, so the state is correct. The window of inconsistency is
until the service restarts.

---

### F6 — Low: merge conflict with slice B (reactions) expected

**File:** `gnome/groundhog/src/mls/gh-mls-service.c:2515`  
**Severity:** Low (merge risk)  
**Failure scenario:** Both slice B (reactions) and this slice add
inner-kind dispatch branches at the same `if (message)` block in
`process_event`. Both also touch `gh-message-row.c` and `gh-message.c`.
Textual merge conflicts are certain. The conflicts are straightforward
(sequential `else if` branches for different kinds) but should be
coordinated.

---

### F7 — Nit: `apply_vote` return value contradicts its doc-comment

**File:** `gnome/groundhog/src/mls/gh-mls-poll.c:440`  
**Severity:** Nit  
**Failure scenario:** The header comment says "Returns TRUE if tallies
changed" but the function always returns TRUE (even for an identical
re-vote). No caller depends on this for correctness today, but the
mismatch is confusing.

---

## Focus-area notes

### 1. Wire compatibility with MDK v0.11.0

Tag names, kind numbers (1068/1018), option-id encoding (sequential
integers from "0"), polltype values ("singlechoice"/"multiplechoice"),
endsAt format (string decimal timestamp), vote tags (["e", poll_id],
["response", option_id]), and empty content for votes all match
`crates/traits/src/polls.rs` byte-for-byte. The "h" routing tag is
harmless (both parsers ignore unknown tags). Re-derived vector confirms.

### 2. MDK interop matrix case

The 6 unit vectors (tests 20–25) are valid and thorough parser-level
coverage. They are NOT a live Docker driver round-trip through the
`groundhog-mdk011-interop-*` harness. The brief required one (F2).

### 3. State and privacy

**Restart:** polls survive restart via `rebuild_polls` — two-pass
scan of the timeline (polls first, then votes). Correct.

**Reorg/withdrawal:** within a session, a withdrawn message leaves stale
in-memory state (F5). On restart, state is rebuilt from surviving
messages. Acceptable for now.

**Purge:** no special handling. Polls for a purged conversation persist
in the in-memory hash table until service restart. Acceptable.

**Who-voted:** the protocol reveals all votes to all group members (each
vote is an MLS app message). The UI shows only aggregate tallies (count
and percentage per option). Individual voters are tracked internally
(`GhMlsPollVoterRecord`) but never exposed to the UI. This matches
what the protocol reveals — the model has the information but the view
does not call attention to it. The header exposes no API to enumerate
individual voters.

### 4. Memory safety

The author's use-after-free fix (question pointer read after
`nostr_event_free`) is correct and consistently applied. The fuzz
campaign found a new heap-buffer-overflow (F1) in `valid_display_text`
from invalid UTF-8. No other memory safety issues found under ASan+UBSan
(25 tests clean) or code inspection.

### 5. UI/HIG/accessibility

- **Blueprint:** well-structured Create Poll dialog with Adw.ToolbarView,
  Adw.Clamp, Adw.PreferencesGroup. Uses standard HIG patterns.
- **Accessibility:** poll button has `accessible label` and `description`
  in Blueprint. Poll card uses `GTK_ACCESSIBLE_ROLE_GROUP`. Option
  buttons are standard `GtkToggleButton` with text labels.
- **Dynamic options:** 2 fixed + up to 8 dynamic AdwEntryRow, hiding the
  "Add Option" button at 10. Correct bounds.
- **Closed poll:** options disabled, status shows "Closed". Good.
- **Percentage display:** `votes * 100 / total_voters` — integer division,
  displayed as `N%`. Will show 0% for options with votes < 1% of total.
  Standard behaviour.

### 6. Tests fail with fixes reverted

Spot-checked 5 tests by reverting their guarded code:
1. `test_bounds_too_few_options` — reverted min-options from 2 to 1: ❌ fails
2. `test_change_vote` — prevented vote replacement: ❌ fails
3. `test_parse_valid` — changed poll kind to 999: ❌ fails
4. `test_mdk_vote_to_groundhog` — changed "e" tag to "x": ❌ fails
5. `test_closed_poll` — made `is_open` always return TRUE: ❌ fails

### 7. Merge risk

Expected textual conflicts in `gh-mls-service.c` (inner-kind dispatch),
`gh-message-row.c` (template children), and `gh-message.c` (kind
constants). All are additive (new `else if` branches, new struct fields)
and mechanically resolvable. Coordinate with slice B (reactions) author.
