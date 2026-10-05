# W29 main-window independent review — 2026-10-04

**Branch reviewed:** `groundhog/w29-main-window` at `701425d3` (bead `nostrc-lol6`), based on `origin/master` `fc63e29e`. The WIP commit message reflects a credit limit, not the review outcome.

**Verdict: CHANGES-REQUIRED.** The wide header, sidebar spacing, search interactions, and removal of the duplicate Conversation Info menu item work, but the title still does not read fully at narrow width, an explicit acceptance criterion.

## Finding

1. **Medium — The narrow message-header title is still truncated.** `gnome/groundhog/data/ui/gh-content-page.blp:32-37` limits the title to two lines while retaining middle ellipsization; `:137-146` merely enables wrapping below 600sp. In the actual 360×294 screenshot, “Book Club - planning our autumn reading weekend together” renders as “Book Club - / planning … together”. A user on the minimum-width/folded layout cannot read the full title without a pointer tooltip (not a dependable touch or keyboard solution). `gnome/groundhog/tests/ui/test_conversation_list.c:762-775` asserts wrap and `line_count > 1`, but does not reject an ellipsized layout, so it passes despite this failure. Make the complete title discoverable in the narrow header without hover and assert the rendered layout does not elide the test title at the supported minimum width. If the product instead accepts truncation plus tooltip, obtain an explicit owner decision and change that acceptance criterion; the present result does not satisfy it.

## Verification

- A clean macOS Ninja build with `BUILD_GROUNDHOG=ON` passed. `groundhog-shell`, `groundhog-conversation-list`, `groundhog-blueprint`, `groundhog-conversation-info`, `groundhog-conversation-menu`, and `groundhog-mls-ui` passed, as did `scripts/check-unsequenced-args.py` and `git diff --check`.
- `GROUNDHOG_TEST_SCREENSHOTS` produced 900×600 and 360×294 light/dark captures. The 900px content header displays the full title; the 360px content header visibly elides its middle. The sidebar has 12px outer list margins, separators, and padded rows in the captures.
- Search tests cover live trimmed/case-insensitive title and npub substring matching; request-subject matching and metadata refiltering; body/preview exclusion; filtered Requests/no-results/invitations; and clearing/focus. As a mutation check, I changed `GTK_STRING_FILTER_MATCH_MODE_SUBSTRING` to `EXACT` only in my review worktree: `/groundhog/conversation-list/search-live-scope` failed (`0 == 1`), then passed after restoring the source. The test currently emits `stop-search` rather than synthesizing an Escape keypress, but its action signal is wired to the same clearing handler.
- The header-bar Conversation Info button is retained, the menu entry is removed, and the button opens the dialog in `groundhog-conversation-menu`; `groundhog-shell` verifies Ctrl+I remains bound. Folded Ctrl+F navigation and the sidebar search focus passed in `groundhog-conversation-list`.
- `scripts/linux-gate.sh --sanitizers /tmp/rv-w29-main-window` exited 0: 60 tests passed after `groundhog-mls-kp-lifecycle-adopted/upgrade-companion-held` failed in the parallel run and passed alone on the gate's retry. That test is in untouched code; no sanitizer report was observed.
- The reviewed commit cherry-picked without conflict onto `origin/master` in an isolated `/tmp/rv-w29-cherrypick` worktree (integration commit `73623f2d`). Neither the main checkout nor the author's worktree was changed.
- Version decision: **no additional Groundhog bump**. The UI bug fix is compatible and included in the already-declared unreleased `0.12.0`; `VERSION_MANIFEST.md` documents this correctly. No other tracked component is affected.

No push was made.
