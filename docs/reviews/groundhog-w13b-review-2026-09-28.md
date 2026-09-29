# Groundhog W13b peer review: W13 remediation, G04, G12, G15, G17 (2026-09-28)

## Context / scope

This is an independent review of exactly `5ec0ade31ec7f64bb3ff6c97517edca38fde9b15..31615784` (the HEAD of `groundhog/w13b-review`).

- **Ignored.** The beads-only commits (`10d6df3a`, `077a6739`, `58210566`, `f8756476`, `45628d69`, `dc1d9103`, `86d55a4f`) and the W13 review document itself (`6e722964`).
- **Normative reference.** `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`.

| Commit | Item | What it does |
|---|---|---|
| `10b8947c` | W13 remediation | B1, plus W13 non-blocking #1, #3, #4, #5 and #6 |
| `e6233b3a` | **G04** | `GhAccountStore` (key lookup, SQLCipher open, legacy import, delegate, outbox and inbox grant, forget and start fresh), `GhAppServices`, the `GhDmInbox` storage mode, store banners |
| `9aee3e59`, `2129a1b2` | **G12** | `GhConversationView`, `GhMessageRow`, day separators, `GhDeliveryIndicator`, `gh-link-policy`, the preview consent flow (D13) |
| `075f4d51` | **G15** | `GhBackground` (application hold, autostart file or portal, SetStatus, first-close explanation), SIGTERM/SIGINT, `register-session` |
| `7e7a14fd`, `e221469e` | **G17** | `GhPreferencesDialog`; "Delete All Messages on This Device" runs `GhAccountStore`'s forget |
| `31615784` | test fix | the request-title test uses G12's view accessor |

**What ships.** With SQLCipher, the executable now opens the account's encrypted store before the inbox subscribes. Conversations and messages persist, and the new conversation view, background mode and Preferences are all live. The composer (G13), notifications (G16), expiry and retention purge (G07), the contact directory (G10) and attachments (G21/G22) are **not** in this range.

I changed no code or beads.

## B1 of the W13 review: re-check

**Verdict: resolved in every configuration.** Nothing that makes a message count as seen outlives the message, and nothing writes a plaintext file named after the account.

| Configuration | Where the seen keys and checkpoint live | Evidence |
|---|---|---|
| **Store present** (`OPEN`) | `w`/`r` keys in the store, written with the message in one T-admit. The checkpoint is in the store's `cursors` table (`gh-account-store.c:170-196`). | The inbox gets its grant only after the import, the attach and the outbox (`gh-account-store.c:288-318`), so nothing is admitted before it can be stored. The grant is withdrawn, saving the latest checkpoint, before the store closes: `unbind_store` (`:208-227`), `withdraw_storage` (`gh-dm-inbox.c:936-948`). The inbox saves it through the grant even if its own account-switch handler runs first (`teardown_account`, `:950-966`). `restart-relists` runs on the executable's wiring over the same directories: no REQ while the key lookup is held, the message listed again from the store, the relay copy skipped with no signer call, and `since` taken from the stored cursor. |
| **Store-less build** (`gh_dm_inbox_new`, `gh-app-services.c:182-188`) | `w`/`r` keys in a per-binding `MemorySeen` hash (`gh-dm-inbox.c:296-356`). The checkpoint is never written (`checkpoint_write`, `:218`) and reset per binding (`:1056`). | Only `x` ids persist, in the pseudonymous `<acct>.seen` (`open_rejected`, `:985-1019`). Switching A→B→A inside one process builds a new `MemorySeen` and starts from checkpoint 0, so the 30-day window (`gh-dm-inbox.h:66`) is fetched again. The UI says so: the in-memory banner ("Messages aren't saved on this device — they're gone when Groundhog closes", `gh-status.c:147`, set at `gh-app-services.c:500-504`). |
| **Ephemeral** ("Continue Without Saving Messages") | In the in-memory SQLCipher store, cursor included. No legacy import runs (`bind_store`, `:295-296`). | `ephemeral-restart-refetches`: zero files under the whole XDG root, and the message is unwrapped and listed again after a restart. |
| **Legacy import** (ST-12) | Only `x` lines are imported, from both 0.6.0's `<pubkey>.seen` and `<acct>.seen`. `<pubkey>.checkpoint` is deleted unread, and all the files are deleted (`gh-account-store.c:240-284`; `gh_store_conversations_import_seen_file`, `gh-store-conversations.c:980`). | `legacy-import`: `w`/`r` are not seen, the cursor is 0, the initial window is requested, and the relay copy is stored and survives a restart. `gh_nip17_seen_import_rejected` (`gh-nip17-inbox.c:652`) does the same on the memory-only side. The ST-12 expectations were updated. |

**Also checked:**
- The x-only files are only ever rewritten with `x` keys.
- A refused (planted or foreign) legacy file is left in place and never read as a seen-set.
- Both import paths delete the old checkpoint.

**Note.** The pseudonymous `<acct>.seen` still carries the pubkey in its header line (`groundhog-nip17-seen 1 <account>`). This is acceptable: the file is 0600, and `current-npub` in dconf names the account anyway. §3.2 could say that only the *name* is pseudonymous.

## Findings

### Blocking

1. **After a restart, stored messages older than the newest 50 of a conversation cannot be reached. Opening the conversation then marks them read unseen (charter P4, §7.6 "load-earlier paging", PT-2's spirit).**
   - **Restore.** `bind_store` attaches with `page_size` 0 (`gh-account-store.c:297`), which lists each room's newest `GH_STORE_CONVERSATIONS_PAGE_SIZE` = 50 messages. `has_older` is set, and the unread count also covers the unloaded messages (`gh-store-conversations.h:39-44,58-60,70-77`; the behaviour is pinned by `tests/store/test_store_conversations.c:644-668`).
   - **No loader.** Older pages load only through the view's history loader (`gh-conversation-view.c:544-557`). No production code installs one: the executable sets only the conversation and the settings (`gh-conversation-list.c:187,278-279`). `gh_store_conversations_load_older` has no production caller. The G12 bead records it only as a "seam for G13/G04", and G13's acceptance does not cover it.
   - **Nothing says so.** `maybe_load_older` returns silently when no loader is set, and no spinner, row or banner shows that older messages exist.
   - **Unread messages are lost from view.** The view opens at the first *loaded* unread message (`:923-928`, clamped). Showing the conversation calls `gh_conversation_mark_read`, which sets `unread_older = 0` and persists the read marker (`gh-conversation.c:469-482`). So a room with 80 unread messages shows 50, and all 80 become read.
   - **Regression against the store-less build.** The memory-only build shows everything from the last 30 days after a restart; the encrypted-store build shows at most 50 per room.
   - **Required.**
     - Wire the history loader in the executable. For example, in `gh_conversation_list_attach`/`gh_app_services_attach_window`, call `gh_store_conversations_load_older(gh_account_store_get_conversations(…), conversation, PAGE_SIZE, …)` and then `gh_conversation_view_finish_loading_older()`. Clear the loader, or make it fail, while no store is bound.
     - Until older messages have been listed, do not zero `unread_older` on open.
   - **Test.** Use the executable wiring (as `restart-relists` does) with more than 50 messages in one room, including more than 50 unread. After a restart:
     - scrolling to the top lists the older page;
     - the unloaded unread messages stay unread until they are listed.

2. **Preferences states effects that nothing in this build performs (charter P4; §7.11 "no fake support"; §7.1 "Every disabled control says why").**
   - **Rows with no consumer.** Every §7.11 key is bound both ways, but nothing outside the dialog reads these keys (grep over `src/`): `retention-days`, `default-disappearing-seconds`, `notifications-enabled`, `notification-privacy`, `sound-enabled`, `load-remote-images`, `load-profile-pictures`, `filter-unknown-senders` and `enter-sends`. `gh_store_purge` has no production caller either (G07 has not landed).
   - **False claims about data retention.** These are privacy claims in the shipped UI:
     - "Keep Messages … Older messages are deleted from this device only" (`gh-preferences-dialog.blp:140-147`). With 30 Days chosen, every message is still kept indefinitely.
     - "Messages are deleted from this device when they expire, and relays are asked to delete them", with a "Default Timer" (`:118-130`). Expired incoming messages are only hidden by the view and stay in the store, and nothing can send.
   - **Notifications.** "Notifications — Tell me when new messages arrive", the content levels and the lock-screen footer (`:18-45`) describe G16 behaviour that does not exist. With "Receive Messages When Closed" on, a user will wait for notifications that never come.
   - **Contrast.** The same dialog handles missing features honestly elsewhere: Tor is hidden (`GROUNDHOG_HAVE_TOR`), and it says that proxies, attachment servers and NIP-46 are not supported yet.
   - **Required.**
     - Gate the rows whose feature is not in the build, as Tor is gated. Hide them, or make them insensitive with a reason ("Not available in this version yet").
     - At minimum, no row may claim a deletion or a notification that does not happen: Keep Messages, the Disappearing Messages group, and the Notifications group.
     - Apply the same treatment to Load Images, Load Profile Pictures, Filter Unknown Senders (requests are always separated today) and Send with Enter, which are harmless but equally inert.
   - **Test.** `test_preferences` should assert the gated state in this build and flip when each feature lands. Alternatively, add a `check_privacy.py`-style rule that every key bound in the dialog has a consumer outside it, unless it is listed as gated.

### Reviewed areas — no blocking finding

- **The W13 non-blocking items this range addressed**
  - **#1 (test-bus EBADF).** `nostrc_test_bus_tolerate_ebadf()` re-arms the fatal handler and resets the count per case (`tests/common/nostrc-test-bus.c:111-124`). `nostrc_test_bus_add_func()` wraps the cases, and `nostrc-test-bus-selftest` covers two single-EBADF cases, a 65-EBADF abort and other messages staying fatal. One binary in this range still uses the old pattern (non-blocking #6).
  - **#3 (SQLCipher memory).** `PRAGMA cipher_memory_security = ON` runs right after keying. It is read back, and any value other than 1 fails the open with `NO_CIPHER` (`gh-store.c:1040-1060`). Charter §3.4/§3.5 record the effect and the measured cost.
  - **#4 (request titles).**
    - The title is the sender's npub (`gh_conversation_get_title`).
    - The subject becomes secondary text in the row, "Subject: …" in the accessible label and part of the header subtitle; it stays searchable.
    - `title` notifies on accept and reply.
    - Avatar initials no longer come from sender-chosen text.
  - **#5 (ids in logs).** No wrap or rumor id appears in any `gh-dm-inbox` log. The `check_privacy.py` `log-ids` rule has mutation self-tests.
  - **#6 (state file names).** State files have pseudonymous names. Legacy files are deleted after the x-only import and on forget.
  - **W13 non-blocking #8 (selectable text).** Message text is now selectable (G12).
- **G04: lifecycle, PT-6 and key custody**
  - **Serialized operations.**
    - One operation at a time; a stale open closes its store (emitting "store-closed") before anything else may open (`gh-account-store.c:375-397`).
    - A generation change cancels an open, unbinds, and opens the next store only once idle (`:563-594`).
    - `pt6-switch-order` and `pt6-switch-mid-open` check the event order and that no fd remains on the old store.
  - **Key custody.**
    - Only a lookup when `store.db` exists (a lost item becomes `KEY_MISSING`); lookup-or-create otherwise.
    - The key is dropped in the worker right after keying.
    - `unlock` is the only interactive call.
    - "Continue Without Saving" is offered only in `UNAVAILABLE`.
    - `CORRUPT` is read-only, with no grant and no outbox.
  - **Forget (§3.8, ST-8).**
    - For the active account: unbind (grant, outbox, delegate, store), then destroy the key item interactively, then unlink the directory and the legacy files. `current-npub` is cleared only if it names that account (`:489-540`).
    - Queued forgets, and one arriving during an open, are ordered; the open is abandoned and its store closed first.
    - Preferences passes no cancellable, so a forget cannot stop between the key and the unlink.
    - The copy is honest: "Also signs out of this account here", and "_Delete and Sign Out" is destructive (`gh-preferences-dialog.blp:405-438`). Clearing `current-npub` revokes the generation through the controller's shared `GSettings` (`gh-account-controller.c:247-253`).
    - Charter §3.8 step 1 says "revoke the generation" first. The code instead closes every scope first and revokes at the end. Its intent (nothing receives, sends or holds the store during the shred) is met.
  - **Container.** `GhAppServices` has an ordered init table, tears down in exact reverse, and the application is the owner.
- **G12: rendering, links and honesty**
  - **Link safety (PD-3, PT-3).**
    - Bodies go through `gh_link_policy_to_markup`: everything is escaped, and only scanned links become `<a>` elements showing themselves.
    - Format, bidi, private-use and unassigned characters end a link (`gh-link-policy.c:45-65`).
    - `javascript:`, `data:`, `file:`, `nsec` and malformed hosts stay text.
    - http, IDN/punycode and user-info need a confirmation showing the rebuilt ASCII address (`:355-396`; `gh-conversation-view.c:1039-1090`).
    - `nostr:` addresses are copied, never resolved.
    - `activate-link` always returns TRUE through the view's policy (`gh-message-row.c:325-332`); GTK's "Open Link" context item goes through the same signal.
    - https opens through `GtkUriLauncher`.
  - **No remote fetch.** No preview fetcher, image loader or profile fetch exists in the executable. See non-blocking #1 for the consent prompt this leaves behind.
  - **Delivery copy (UX-5).**
    - No delivered or read value exists.
    - "Sent. At least one of each recipient's message relays accepted it."
    - The popover reminds that "A relay accepting a message means it stored it. Groundhog can't tell when anyone receives or reads it."
  - **Accessibility and HIG.**
    - The row label is composed as "You, 10:43: Hi. Sent."; polite and assertive announcements happen only while the window is active.
    - The jump button's label is set from C with a plural.
    - Icons carry the `presentation` role; retry and unlock have tooltips.
    - Bubbles are compact below 480 sp, with high-contrast outlines. The outgoing accent is darkened for WCAG AA body text (`data/style.css:82-88`).
  - **Lifetime and blueprint.**
    - Every idle and timeout is cleared on dispose and when the conversation changes (`gh-conversation-view.c:891-895,1465-1483`).
    - When the account switches, the model's removal clears the shown conversation.
    - Blueprint parity holds with 0.12 and 0.20.
- **G15: background lifecycle**
  - **Hold.** The one application hold follows `run-in-background` (`gh-background.c:87-99`); the reconciliation holds and releases in pairs.
  - **Quit.** `app.quit` and SIGTERM/SIGINT reach `g_application_quit`, then teardown runs in reverse order (`main.c:104-119`, `gh-window.c:257-262`). `register-session` covers logout.
  - **Autostart needs a confirmed choice.** Only a *user* value of the key touches autostart (`:77-84,710-712`); the unconfirmed default writes nothing, and NO-9 on the executable asserts that. Only an entry marked `X-Groundhog-Autostart` is replaced or removed, and one the user switched off in the desktop wins.
  - **Portal.** Used only when sandboxed. A refusal turns the key off, and SetStatus is sent only for portal version 2 or later.
  - **First close.** The dialog's copy is honest about relay exposure.
  - **Test realism.** The tests are real: the shipped binary in `--gapplication-service`, `org.gtk.Actions.Activate("quit")`, SIGTERM, exit status 0, a fake portal, and the NO-12 timer probe.
- **G17: Preferences dialog.**
  - **Bindings.** Rows bind both ways, and a stored value no choice has is shown as an extra item and never rewritten.
  - **URL editors.** They apply the PD-5 rules.
  - **One dialog, locked while deleting.** Only one dialog opens at a time, and it cannot close while a forget runs. Disposal during a pending forget is tested.
  - **Tor.** It stays hidden without G09.
- **Versioning**
  - **Current state.** The source still reads **Groundhog 0.6.0** (`CMakeLists.txt:2`, `VERSION_MANIFEST.md:21`, `groundhog --version`); each commit deferred the bump on instruction.
  - **Recommendation.** This range ships new user-facing capabilities: persistent encrypted history, the conversation view, background delivery with autostart, and Preferences. It also adds GSettings consumers and an autostart file. With Groundhog at 0.x, that is **MINOR: 0.7.0**, applied once by the integrator after the blocking findings are fixed, with `VERSION_MANIFEST.md` updated to match.
  - **No other bump.** `tests/common` and `cmake/NostrcTestBus.cmake` are test infrastructure. No library, NIP or other app source changed.

### Non-blocking (follow-up suggested)

1. **The link-preview affordance asks consent for a fetch that cannot happen (G12, D13).**
   - **What the user sees.** Every message with an https link shows "Show Preview" (`gh-message-row.c:218-263`). The consent dialog says "This connects to example.com from your IP address…" (`gh-conversation-view.c:1179-1226`). The result is then "Link previews aren't available in this version… Nothing was loaded."
   - **What persists.** "Don't ask again" still stores `link-previews=true` (`:1238-1240`), consent given for a feature that did nothing.
   - **Suggestion.** Hide the preview box while no fetcher is installed.
   - **Later, with a fetcher.** The Tor wording keys off `network-mode` even though G09 is absent. Gate it on the transport that will actually be used.
2. **Other G12 seams are unwired and untracked.** The delivery-report function, `retry-requested`, `unlock-requested`, `set_locked_messages` and `set_recipient_without_inbox` have no production caller. They cannot be reached today: nothing sends, and no locked count is fed. The delivery popover honestly says "Relay details aren't available for this message." Make their wiring an acceptance criterion of G13 (with G06/G04), so they do not ship as dead buttons.
3. **No recovery action in `KEY_MISSING` and `CORRUPT` (charter §3.4, §7.15 #16).**
   - **What the banners offer.** `KEY_MISSING` offers only "Try Again", and `CORRUPT` offers nothing (`gh-status.c:122-125`). `gh_account_store_start_fresh_async` has no UI caller.
   - **The only way out.** Preferences → Delete All Messages, which also signs out.
   - **Suggestion.** Add "Start Fresh on This Device…" and "Reset Storage…" behind an `AdwAlertDialog`, with the §3.4 copy about what can be downloaded again.
4. **Forget result reporting.**
   - **(a) Misleading failure toast.** If deleting the key item fails, the files are still unlinked and `current-npub` is still cleared, which is reasonable and documented. But the toast reads "Couldn't delete all messages: …" (`gh-preferences-dialog.c:569-570`) although the messages are gone.
     - **In `EPHEMERAL`.** There is nothing to shred, yet `destroy` fails with `UNAVAILABLE` and reports the same failure.
     - **Suggestion.** Say "Messages deleted; the storage key couldn't be removed from the keyring", and skip or excuse the key step when no Secret Service exists.
   - **(b) Order is untested.** ST-8 asserts the end state, not that the key goes before the unlink. The fake Secret Service could record whether `store.db` still existed when the item was deleted.
   - **(c) Quit during a forget.** Ctrl+Q stays available while the dialog is locked. Quitting abandons the operation, at best after the key step, and the next start shows `KEY_MISSING`. That is safe, because the data is crypto-shredded, but it is unexplained.
5. **G15 copy and translation.**
   - **Autostart is not stated where it is chosen.** The Preferences subtitle ("keeps running after its window is closed; Quit stops it", `gh-preferences-dialog.blp:398-399`) does not say that switching the row on also starts Groundhog at login. The entry is written as soon as the key has a user value (P9).
   - **Untranslated strings.** The first-close dialog, the portal reason and the SetStatus strings are not marked for translation (`gh-background.c:23,531-539`; `gh-background.h:78-81`).
   - **No gettext domain.** It is still never bound (W13 non-blocking #8).
6. **`tests/app/test_background.c` still uses the pattern W13 non-blocking #1 described.**
   - **What happens.** The bus comes up before `g_test_run()` (`:1514`), but the cases are registered with plain `g_test_add_func` (`:1517-1524`). On macOS the EBADF tolerance is therefore armed only for the first case.
   - **Fix.** Use `nostrc_test_bus_add_func`.
   - **Current impact.** 70/70 stress runs passed, so the problem is latent.
7. **Autostart while the keyring is locked (NO-11) has no way back without the window.**
   - **What is missing.** No retry when the login keyring unlocks, no G16 "Unlock to receive messages" notification, and no status on a host install (SetStatus is portal-only).
   - **Suggestion.** Track this with G16, or add a watch on the default collection's `locked` state.
8. **Other W13 items unchanged:**
   - #2 (T-mls wrapping in `qp24.7`/`qp24.13`).
   - #7 (outbox self-copy AUTH, D8 jitter, wrap retention): G04 now instantiates the outbox per store, but nothing enqueues until G13, so these stay latent.
   - #8: row times across midnight (`qp24.48`) and the untranslated account-limits strings.
   - #9: cosmetic.

## Verification

- **Checks and submodules.** `git diff --check 5ec0ade3..31615784` (excluding `.beads`) is clean. `git submodule update --init third_party/nsync third_party/nostrdb` was run.
- **macOS 15 (Darwin 24.6, arm64; GLib 2.90; SQLCipher 4.17.0)**
  - **Build.** `cmake -S . -B /tmp/gh-w13b-review -G Ninja -DBUILD_GROUNDHOG=ON -DBUILD_APPS=OFF -DBUILD_NOSTR_GTK=OFF && cmake --build /tmp/gh-w13b-review -j4`: 1228/1228 targets. There are no Groundhog compiler warnings; the only ones are in libmarmot and nostrdb, plus the repo-wide macOS `ld` duplicate-library notice.
  - **Tests.** `ctest --test-dir /tmp/gh-w13b-review -R 'groundhog-|nostrc-test-bus' -j4 --output-on-failure`: **36/36 passed**. Three platform skips are included: `groundhog-launch`, `groundhog-store-key-keyring` and `groundhog-background-gui` (no WindowServer session or keyring). This matches the integrator's count.
  - **Stress.** `--repeat until-fail:10` over `groundhog-{background,account-store,dm-inbox,preferences,conversation-view,account,dm-send}`: 70/70 passed, with 0 "Bad file descriptor" lines logged.
- **Linux (Docker `local/groundhog-ci:24.04`, plus `libsqlcipher-dev` 4.5.6, `gnome-keyring`, `blueprint-compiler` 0.12, `adwaita-icon-theme`)**
  - **Build.** I used CI's configure flags (Debug, `-DBUILD_RELAYD=OFF -DSIGNET_ENABLE=OFF`) and CI's full target list: 414/414, with no Groundhog warnings.
  - **Tests.** `dbus-run-session -- xvfb-run -a -s '-screen 0 1280x800x24' ctest -R '^(groundhog-|nostrc-test-bus-selftest$)' -j4`: **38/38 passed with no skips**. This includes:
    - `groundhog-launch`;
    - `groundhog-background` (NO-9/NO-10 on the real executable), `groundhog-background-gui` and `groundhog-background-desktop`;
    - `groundhog-store-key-keyring` and `groundhog-store-sqlite`;
    - `nostrc-test-bus-selftest`.
  - **Reproduction note.** A tar made on macOS carries AppleDouble `._*` files, which break the CMake test globs. Delete them before configuring. This is not a repository issue.
- **Code audits.** B1 and forget were traced through the code paths cited above. There were greps for key consumers, preview, loader and seam callers, and every file write outside the store.

**REQUEST CHANGES**: scoped to `31615784` versus `5ec0ade3`.

- **W13 B1 is resolved in every configuration.** The W13 non-blocking #1, #3, #4, #5 and #6 are also done.
- **Two new blocking findings.** Both are small integration fixes with a test each:
  1. Stored history beyond the newest 50 messages per room is unreachable after a restart, and opening the room marks it read.
  2. Preferences claims deletion and notifications that this build does not perform.
- **Everything else.** G04's lifecycle, PT-6 and forget ordering, G12's link policy and honest delivery copy, and G15's lifecycle are sound. Recommend Groundhog **0.7.0** when this range is integrated.
