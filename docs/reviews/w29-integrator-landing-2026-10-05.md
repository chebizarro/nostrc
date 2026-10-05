# W29 alpha polish — integrator landing note (2026-10-05)

**Decision:** the owner asked for W29 to land as integrator-verified ("Land W29",
2026-10-05) because no independent reviewer was available: both agent providers
were at their usage limits when the slices were finished. This note records what
was and was not independently reviewed, so a later reviewer can pick it up.

| Slice | Bead | Independent review | State at landing |
|---|---|---|---|
| A — consent-gated web content (link previews, linked images, profile pictures); relay rows in Preferences | nostrc-8pfk | none | Integrator-verified |
| B — sidebar margins, full conversation titles, live metadata search, one Conversation Info control | nostrc-lol6 | `w29-main-window-review-2026-10-04.md`: CHANGES-REQUIRED (title still ellipsized at 360px; test could not tell) | Finding answered: full-width wrapping title below 600sp and a test that fails when the label cannot wrap. Not re-reviewed |
| C — libnostr JSON escaping (emoji/reactions), Create Poll visibility | nostrc-30gt, nostrc-p9y4 | `w29-poll-reactions-review-2026-10-04.md`: fix approved in substance (RFC 8259, canonical id independently confirmed, fails with fix reverted); CHANGES-REQUIRED for an unfinished test harness and a missing manifest row | Harness dropped, manifest row added. Addendum not written |
| D — About: nostr: links open a DM, NIP-34 issue dialog, themed Details | nostrc-lz9e | none | Integrator-verified |
| Signer tests on nostrc-test-bus | nostrc-gcu4 | none | Test-only; needed for the macOS gate after a reboot |

**Verification by the integrator (each slice on its own branch before landing):**
macOS Groundhog suite 90/90 (one known voice flake, nostrc-mmko, passed alone on
D); Linux smoke gate 462 tests plus the serial perf stage; sanitizer gate 60/60;
all four slices together on an integration branch 90/90. New tests were checked
against their fixes reverted where the review asked (B's truncation test fails
with `wrap: false`; C's locale test fails with `iscntrl()` restored).

**Open for a follow-up review:** A's HTML preview parsing and consent memory;
D's NIP-34 event shape and the `nostr:` URI handling; B's search contract.
