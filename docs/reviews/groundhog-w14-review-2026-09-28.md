# Groundhog W14 peer review: W13c remediation, G07, G14, G16, G08+G10, G13, 0.7.0 (2026-09-28)

## Context / scope

This is an independent review of exactly `31615784..43dfa508` (the HEAD of `groundhog/w14-review`).

- **Ignored.** The beads-only commits (`d76510a8`, `9119e3e5`, `948ef53c`, `055772ea`, `00dff609`, `718b1a18`, `73d0abaf`, `735c8de4`, `2d2995c4`, `c2f38bb6`) and the W13b review document itself (`15d7444a`).
- **Normative reference.** `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`.
- **Order.** The W13c remediation (`86925cde`) was rebased on top of the feature commits. HEAD is therefore the state reviewed throughout; file:line references are to `43dfa508`.

| Commit | Item | What it does |
|---|---|---|
| `d14b9b41` | **G07** | `GhExpiry`: per-conversation timers, NIP-40 expirations on rumor, seal and wrap, T-purge with exact read state, retention, the "purged" signal |
| `935a3ab8` | **G14** | `GhOnboardingView`, `GhInboxSetup`/`GhInboxProbe` (kind-10050 publish and the private-reads check), `data/relay-suggestions.json` |
| `eb1cd75c` | **G16** | `GhNotifier`: levels, forced-hidden requests, coalescing, withdrawal, opaque activation targets, the NO-11 locked-store notice |
| `574b5bb0` | **G08 + G10** | `GhAuthPolicy` (SELF_WRAP becomes ACCOUNT on challenge) and the cached `GhContactDirectory` behind the outbox |
| `c76386c7` | **G13** | `GhComposer`, drafts, `gh-send-ui.c` (send wiring and the G12 seams), the two-account e2e test |
| `86925cde` | W13c remediation | W13b B1 and B2, plus non-blocking #1, #3, #4a, #5 and #6 |
| `43dfa508` | version | Groundhog 0.7.0 |

**What ships.** With SQLCipher, the executable can now send NIP-17 messages through the durable outbox. It keeps and pages stored history, purges expired and old messages, and shows private notifications (off by default). It also runs a first-run onboarding that can publish a kind-10050 list. Attachments (G21/G22), Tor (G09), the New Message dialog (G18) and Conversation Info (G19) are **not** in this range.

I changed no code or beads.

## W13b review: re-check

### B1: history beyond the newest page. **Resolved.**

- **The loader is wired.** `gh_app_services_attach_window` calls `gh_store_status_attach_history` (`gh-app-services.c:691`). That installs `load_older_page` (`gh-store-status.c:62-83`), which lists one `GH_STORE_CONVERSATIONS_PAGE_SIZE` page through `gh_account_store_load_older`. It holds the store weakly and fails with `GH_STORE_ERROR_STATE` when none is open.
- **Loading.** The list loads from an idle, drops a load for a room the view has left, and then finishes or fails the view's load (`gh-conversation-list.c:221-271`).
- **Nothing unlisted is marked read.** While `unread_older > 0`, `gh_conversation_mark_read` only records the listed range as seen for the session, and neither moves nor persists the marker (`gh-conversation.c:565-592`).
- **The count stays right.** A page restore recomputes `unread_older` from the stored count minus what is loaded (`:439-440`), so the marker is written once the older unread messages are listed.
- **Test realism.** `groundhog-account-store-gui` drives the executable's wiring across restarts: 120 messages, 80 unread, 30 staying unread until listed. It ran here (not skipped) and survived the stress run below.

### B2: honest Preferences. **Resolved** for what B2 listed, but see blocking finding 1 for a row this range made live.

- **One list of features.** `src/app/gh-features.h` is the single feature list.
- **Gated rows say why.** Gated rows are unbound, insensitive and say "Not available in this version yet" (`gh-preferences-dialog.c:98-126,826-843`). "Filter Unknown Senders" is shown as always on, with a truthful subtitle (PT-8 holds; see G10 below).
- **Every live key has a real consumer:**
  - `notifications-enabled`, `notification-privacy`, `sound-enabled`: `gh-notifier.c:11-13`;
  - `enter-sends`: `gh-send-ui.c:584-586`;
  - `retention-days`, `default-disappearing-seconds`: `gh-app-services.c:343,385,401-404`;
  - `show-message-previews`: `gh-conversation-list.h:51`.
- **Enforced.** `check_privacy.py`'s new `preference-consumers` rule enforces this.
- **The copy matches the code:**
  - "Keep Messages … deleted from this device only" (retention purges on `received_at`, `gh-store.c:3400-3407`);
  - the disappearing description "ask relays to delete them within about a day after that" (the wrap expiration is ≤ E + min(D, 24 h) + 3599 s).

### W13b non-blocking items addressed

| # | Status | Evidence |
|---|---|---|
| **#1** (link previews) | Done | The preview box only appears with a fetcher (`gh-message-row.c`, `gh_conversation_view_get_previews_available`), so `link-previews` is never written from a no-op. |
| **#3** (recovery actions) | Done | "Start Fresh on This Device…" and "Reset Storage…" run behind an `AdwAlertDialog` (`gh-store-status.c:86-…`). |
| **#4a** (forget toast) | Done | `GH_PREFERENCES_FORGET_KEY_KEPT` gives "Messages deleted; the storage key couldn't be removed from the keyring" (`gh-preferences-dialog.c:687-691`). |
| **#5** (translation) | Done | The background strings are marked for translation (`gh-background.c:23,110-118`). |
| **#6** (test bus) | Done | `test_background.c` registers the bus cases with `nostrc_test_bus_add_func` (`:1518-1530`). The `--gui` cases use no bus. |
| **#7** (NO-11 notice) | Partly | The notice exists, but see non-blocking #1. |

## Findings

### Blocking

1. **The Default Timer silently makes every new conversation, including incoming requests, disappear for good. The user sees no indicator before sending and has no way to turn it off (charter §3.7 "UI", P4, §7.11).**
   - **Live since W13c.** `86925cde` made `default-disappearing-seconds` live: `GH_FEATURE_EXPIRY` is `GROUNDHOG_HAVE_EXPIRY` (`gh-features.h:48`), bound at `gh-preferences-dialog.c:856`, and shown as "Default Timer — For conversations started from now on" (`gh-preferences-dialog.blp:126-128`).
   - **It sticks to every new room.** `conversation_insert` stamps `store->default_disappearing` into every new row (`gh-store.c:2008`). That covers rooms created by T-admit for an incoming message from a stranger (`:2434`), not only rooms the user starts.
   - **Every later send uses it.** T-enqueue reads that timer for each send (`gh-outbox.c:1605`).
   - **Nothing can change it.** In the shipped executable no code changes a room's timer afterwards:
     - `gh_expiry_set_timer`/`gh_expiry_get_timer` (`gh-expiry.c:331,352`) have no production caller;
     - the composer's `timer_slot` is an empty, hidden box (`gh-composer.blp:63-64`);
     - nothing in `data/ui` offers a per-conversation timer;
     - the §3.7 timeline row ("You set messages to disappear after 1 day") does not exist.
   - **The default no longer helps.** Changing the default back to Off does not touch existing rooms.
   - **Failure scenario:**
     1. A user tries "Default Timer: 1 Day" and later sets it back to Off.
     2. Meanwhile Alice (a stranger) wrote, or the user started a chat with Bob.
     3. From then on, every message the user sends in those rooms is purged from this device after a day, with `secure_delete`. The outer layers ask Alice's and Bob's relays to drop the wraps.
     4. Nothing in the composer says so before sending. Only the timer icon on the sent bubble shows it afterwards.
     5. The only way out is "Delete All Messages on This Device". That is irreversible data loss the user cannot see or stop.
   - **Required**, either of:
     - Gate the row until a per-conversation control ships in the same build. For example, add a `GH_FEATURE_CONVERSATION_TIMER 0` that `default-disappearing-seconds` also needs in `key_features`, and keep the store default at 0 while it is gated. That is a one-line gate, like the others W13c added.
     - Land G19's timer control (it exists on `groundhog/w15-g19-conversation-info`) before 0.7.0 is tagged, together with a visible pre-send indicator (the composer's `timer_slot`).
   - **Worth deciding too.** Should an *incoming* request inherit the user's default at all? The copy says "conversations started from now on", which reads as conversations the user starts.
   - **Test.** `test_preferences` asserts the gated state for this build's features. Alternatively, a GUI test shows that a room created while the default was on can be switched off from the UI and that the composer shows the timer before sending.

### Reviewed areas — no blocking finding

- **G07: expiry cryptography and the purge**
  - **PT-7 bounds.** `gh_expiry_outer_expiration` gives ceil_hour(E + clamp(U, 0, min(D, 24 h))). That is never before E, a whole hour, and ≤ E + min(D, 24 h) + 3599 (`gh-expiry.c:59-65`).
  - **Independent layers.** Each of the four outer layers draws its own jitter from the `GhClock` CSPRNG (`:67-81`), again at every re-seal (`gh-outbox.c:908-…`). Before T-seal nothing was published, so re-drawing reveals nothing.
  - **The wrap is signed correctly.**
    - The wrap uses a fresh ephemeral key.
    - The tag is appended after `nostr_nip59_wrap` and the wrap is re-signed; `nostr_event_sign` recomputes the id over the new tags (`libnostr/src/event.c:753-781`).
    - `created_at` is still CSPRNG-randomized.
    - The ephemeral key is wiped (`gh-nip17-envelope.c:243-258`).
  - **The seal is checked.** A signed seal whose expiration tag differs from the one requested is rejected as an invalid signer result.
  - **Receive.** `expires_at` = rumor, else seal, else wrap (`gh-nip17-inbox.c:381-383`). G07a's single-tag seal rule is in place, so Groundhog-to-Groundhog disappearing messages round-trip. A message already expired on arrival is recorded as seen only (`gh-store.c:2423`).
  - **T-purge** (`gh-store.c:3367-3455`):
    - the outbox row (with `rumor_json` and the signed wraps, by cascade) goes with the message;
    - read markers on doomed rows move back to the newest survivor, and unread counts are recomputed;
    - `min(expires_at)` is read inside the transaction;
    - the WAL is truncated at most once a minute, with deferral handled;
    - `seen` keeps the ids, so backfill cannot resurrect them (EX-6).
  - **No timer spin.** The purge deletes at `<= now` and the next wake-up is strictly later. A read-only (CORRUPT) store only hides expired messages and schedules on `expires_at > now` (`gh-store-conversations.c:986-1035`).
  - **Lifetime.** The timer holds no reference and is removed in dispose. "store-closed" disposes the `GhExpiry` before the next store opens (`gh-app-services.c:411-413`). The wait is capped at 10 min for clock jumps and suspend. Every timer runs on `GhClock`.
  - **Withdrawal on purge (EX-3/NO-5)** works through the model. Purged messages leave their conversation; the notifier's `items-changed` → `room_prune` drops any room whose counted message is gone (`gh-notifier.c:257-276`). Expiry alone is also covered by `room_expired`.
  - **Deviation.** See non-blocking #3.
- **G14: onboarding, PT-9 and the suggestions**
  - **No relay is contacted before the user acts.**
    - Welcome, account, signer (including "Test Signer", which is local D-Bus only), the inbox step and the confirm page open no connection. `test_onboarding.c:517-519` asserts zero scopes and publishes through the confirm page, and each skip path asserts zero too.
    - Suggestions start unticked (`gh-onboarding-view.c:862-869`).
    - `GhAccountRelays`, `GhDmInbox` and the directory have nothing to contact while `discovery-relays=[]`.
  - **Where "Checked just now: asks for sign-in" comes from.** The screenshot's "Checked just now: asks for sign-in…" (`gh-onboarding-view.c:740`) appears only after one of two explicit actions:
    - **"Check Privacy"** (`onboarding.check-relays`, `:942-970`): enabled only with ticked relays, and it probes only the ticked ones;
    - **Publish**, after the confirm page listed every target (`:1004-1027,1162-1185`).
  - **The probe is harmless.** It is an unauthenticated REQ `{"kinds":[1059],"#p":[<random>],"limit":1}` on its own connection. AUTH challenges are never answered (`gh-inbox-setup.c:448-525`). The random key comes from `g_random_int`, which is fine: it is not a secret. The disclosure copy is non-blocking #2.
  - **`data/relay-suggestions.json` is honest.**
    - It has three `wss://` relays (`relay.nostr.net`, `auth.nostr1.com`, `nip17.com`), each with dated evidence of an `auth-required:` refusal for kind 1059 on 2026-09-28.
    - The descriptions are plain: nip17.com "can't hold your published settings", and nostr1 is described as "alpha".
    - No banned relay: `git grep -i damus -- gnome/groundhog` is empty.
    - `check_privacy.py`'s `relay-suggestions` rule reads the banned list from `AGENTS.md`, so no banned name enters the tree.
  - **Publish.**
    - The list is signed before any connection; a denial contacts nothing (`test_signer_denied`).
    - It goes to the chosen inbox relays, the own 10002 write relays and discovery relays, each on its own connection, with account AUTH only there (own list publish, §4.3).
    - Outcomes are reported per relay with the relay's text escaped.
    - Adopting the inbox relays as discovery relays needs the confirm page's switch, whose subtitle says those relays "will see whom you look up".
- **G16: notifications**
  - **Levels.**
    - hidden: "New message" / "N new messages", id `messages`.
    - sender and preview: the conversation title and id `conv-<n>`. The preview is ≤ 120 graphemes, line breaks collapsed, with an npub prefix in groups (`gh-notifier.c:414-457`).
    - An unknown level counts as hidden.
  - **Requests.** Requests always share the hidden id (`:131-138`), so no name, npub, subject or text reaches the payload at any level (NO-2).
  - **Coalescing.** A send goes out on the next turn, and after that an id updates at most once per 2 s. Sound fires at most once per 10 s, only with `sound-enabled` (`:497-559`).
  - **Withdrawal.** The notifier withdraws:
    - when the room is visible in the active window;
    - on read;
    - on account switch (`sync_account`);
    - when notifications are turned off;
    - on a level change;
    - on purge or expiry;
    - on dispose.
  - **Suppression.** The notifier never alerts for its own messages or self-copies. It also skips expired messages, muted or blocked rooms (from the encrypted store, fail-closed, `:243-252`) and the visible room.
  - **Priority and category.** Priority is normal and the category is `im.received`. No icon, no buttons.
  - **Activation targets leak nothing.**
    - `app.open-conversation((tx))` carries a random 63-bit per-notifier generation (+1 per switch) and a per-generation room number from an in-memory map (`:118-128,489-491,671-700`). It names neither account nor room.
    - A stale or foreign generation never opens a thread. It shows the list and the "That notification was for another account" toast; the Switch button appears only if this process knew that account.
    - The hidden notification carries a target only while it counts exactly one room.
  - **NO-11.** The notice has no account and no count, and its activation brings up the window's Unlock state. See non-blocking #1 for when it appears.
- **G08: `GhAuthPolicy`**
  - **The purpose table matches charter §4.3 exactly** (`gh-auth-policy.c:18-31`). ACCOUNT is used only for own inbox read, own list publish, SELF_WRAP and GROUP; everything else is EPHEMERAL, including unknown purposes.
  - **Self-copy.** SELF_WRAP is now ACCOUNT on challenge. That matches §4.3's "DM self wrap | own 10050 | account (on challenge)" row, and the rationale is recorded in the header.
  - **The legs are kept apart.**
    - The outbox classifies each leg per URL: a recipient's wrap is always RECIPIENT_WRAP.
    - A stored self target that has left the own 10050 list counts as a recipient's (`gh-outbox.c:1005-1020`).
    - Each leg has its own `GhRelayPublish`, so S3 holds.
  - **Stale generations.**
    - A refused ACCOUNT identity for a stale generation leaves the URL NONE, which fails closed as AUTH_REQUIRED.
    - The generation's `GhAccountAuth` is revoked on a switch.
    - The policy and controller hold each other weakly, via qdata and a weak pointer.
- **G10: the contact directory (PT-8, S1, S2)**
  - **PT-8.** Kind 0 is requested only for accepted contacts. The flag comes from `contact->accepted` on every path (`gh-contact-directory.c:552,629,1306`), and a received kind 0 for a non-accepted author is dropped at admission (`:889-893`). A request's sender gets no REQ at all until a send, which is an explicit user action. At that point only 10050 is asked for, unless the room is already accepted.
  - **S2.**
    - A cached 10050 is the answer with no network.
    - A stale one is still used, and a refresh follows U(5, 60) s later off the send path.
    - A changed list re-targets the same wrap through "changed".
  - **S1 and connections.**
    - The first run starts U(2, 30) min after the store binds.
    - Batches are ≤ 10 authors in random order, U(10, 120) s apart.
    - Every REQ uses a fresh scope, only on `discovery-relays`, with CONTACT_DIRECTORY (ephemeral) AUTH.
  - **Cache hygiene.** Rows are re-verified on restore, and everything is dropped on an account switch.
- **G13: composer and send**
  - **T-enqueue before the signer.** `gh_outbox_send` commits the message, the outbox row with its `rumor_json`, `seen(rumor)` and `draft = NULL` in one transaction, with the conversation's timer read inside it (`gh-outbox.c:1596-1640`; `gh-store.c:2659-2675`). Sealing starts only afterwards. A failed enqueue keeps the text and says why, including "Storage is full" (`gh-send-ui.c:427-458`).
  - **Honest status.** Each own message's status is bound to its `GhOutboxItem` (`:86-104`). There is no delivered or read state. The "Can't send" composer page offers "Check Again". Group reply is honestly disabled: "Replying in group conversations isn't possible yet."
  - **Drafts** live only in the encrypted store:
    - saved 1 s after the last edit, on switching rooms, and on dispose (`gh-composer.c:290-303`);
    - restored on return;
    - cleared by T-enqueue;
    - never written to GSettings (PD-11).
  - **A reply accepts the request.** The local echo goes through T-admit's duplicate path, which runs `admit_read_state`, so the acceptance is persisted (`gh-store-conversations.c:448-457`).
  - **The G12 seams are wired** (W13b non-blocking #2): the delivery report, `retry-requested`, `unlock-requested`, `set_locked_messages` and `set_recipient_without_inbox` all have production callers in `gh_send_ui_attach` (`gh-send-ui.c:594-605,410`).
  - **Accessibility.** The icon-only buttons have tooltips and accessible labels, the entry is labelled "Message" (`gh-composer.blp`), and focus returns to the entry.
- **Logs.** Every log call added in this range prints only a `GError` message or fixed text. No pubkey, room id, URL list or content is logged.
- **Versioning**
  - **The bump.** `43dfa508` sets `project(groundhog VERSION 0.7.0)` and updates `VERSION_MANIFEST.md` (declared 0.7.0, Latest release unchanged). `groundhog-version` passes.
  - **MINOR is right.** For a 0.x component that is correct: sending, notifications, onboarding, disappearing messages and retention are new user-facing capabilities.
  - **Storage is unchanged.** There is no schema change in this range (`GH_STORE_SCHEMA_VERSION` untouched; only existing columns are used).
  - **Other components.** No other component's source changed. The stale metainfo text is non-blocking #6.

### Non-blocking (follow-up suggested)

1. **The NO-11 unlock notice never appears with default settings.**
   - **The gate.** `update_notice` also requires `notifications-enabled` (`gh-notifier.c:566`). That key defaults to false, and onboarding never asks about it: charter §7.8's privacy step is missing, which is tracked in `nostrc-qp24.63` #2.
   - **Scenario.**
     1. A user enabled "Receive Messages When Closed" in Preferences, so autostart is written.
     2. They log in with a fingerprint, so the login keyring stays locked.
     3. Groundhog starts windowless in `LOCKED`, opens no REQ and shows nothing.
     4. Messages stay on the relays until they happen to open the window.
   - **Why tests pass.** The NO-11 case runs with the notifier fixture's `notifications-enabled=TRUE` (`test_notifier.c:194`).
   - **Suggestion.** Show the notice unless the user *explicitly* turned notifications off (`g_settings_get_user_value`). It carries no message information. Alternatively, land the onboarding privacy step, which sets `notifications-enabled` when background is chosen (PD-9).
2. **The privacy check's disclosure is only in a tooltip (P9).**
   - **The copy.** "C_heck Privacy" (`gh-onboarding-view.blp:494-497`) says that it "Connects to the ticked relays" only in its tooltip. Meanwhile the visible footer says "none is contacted until you confirm" (`:467`).
   - **Why it matters.** The click *is* the confirmation, but a user who doesn't hover will not know it reveals their IP to those relays.
   - **Suggestion.** Put one visible sentence next to the button, for example "Checking connects to the ticked relays. They see your IP address, not your account."
3. **The seal's expiration is jittered; charter §0.1 #8, §3.7 and EX-1 say it is exact.**
   - **Where.** `gh-expiry.c:67-81`. The deviation is deliberate and is recorded in the commit message and the bead, but not in the charter.
   - **Privacy.** It is harmless: the seal is inside the encryption, and Groundhog receivers take the rumor's value.
   - **Interop.** A client that honours the seal's tag keeps the message up to about 25 h longer.
   - **Suggestion.** Amend §3.7 and EX-1 to match, or make the seal exact.
4. **Sending now exposes the known EOSE-ordering race (`nostrc-qp24.10.6`, P1, open).**
   - **The race.** A first-contact lookup can miss the recipient's stored 10050 behind EOSE. The message then ends as "hasn't set up private messaging" until "Check Again".
   - **Mitigation.** `GhContactDirectory` defers completion to a low-priority idle, which narrows but does not close the race (the bead's words).
   - **The test routes around it.** The e2e test swaps the production directory for a test resolver for this reason (`test_e2e_dm.c:6-11`), so the real first-contact path has no real-socket test.
   - **Suggestion.** Fix it before tagging 0.7.0, then switch the e2e test to the production directory.
5. **Composer Enter handling deviates from §7.7 and its IME guard is untested.**
   - **The deviation.** The controller runs in the **capture** phase and calls `gtk_text_view_im_context_filter_keypress` itself (`gh-composer.c:199-220,392-396`). §7.7 specifies a bubble-phase controller after the IM context.
   - **Double filtering.** For a propagated key (Shift+Enter, or Enter with `enter-sends` off), the text view's own key controller filters the same event through the IM context a second time.
   - **Untested.** `stack_press` emits `key-pressed` with no current event, so the preedit branch never runs in any test (`tests/ui/send-stack.h:428-442`).
   - **Suggestion.** Add a test that drives a real `GdkEvent` through a `GtkIMContextSimple` compose sequence, or move to the prescribed bubble-phase arrangement.
6. **The metainfo still describes 0.6.0.** "This preview … cannot send messages yet, and messages are not kept after it closes" (`data/org.nostr.Groundhog.metainfo.xml:9`) is false for 0.7.0. Update it with the bump, keeping the §1.4 non-goals.
7. **Directory notes.**
   - **(a) Names nobody shows.** Kind-0 names are fetched and cached for accepted contacts (in the same REQ as their 10050, so there is no extra exposure). Nothing shows them yet: `gh_contact_directory_dup_conversation_title` and `_get_display_name` have no production caller.
   - **(b) A comment contradicted by adoption.** The directory's header says lookups never go to "the account's own relays". Onboarding's discovery adoption can make the own inbox relays the discovery relays. The confirm-page copy discloses this; the comment should match.
   - **(c) R6 is still split.** `GhInboxSetup` keeps its own `GhAccountAuth` rather than the policy's (`gh-inbox-setup.c:805-813`), so a relay used for both inbox reads and list publication can prompt twice. This is tracked in `nostrc-qp24.63` #3.
8. **Backfill is never notified.** Messages whose `created_at` precedes the account's binding are never notified (`gh-notifier.c:623-625`).
   - **Consequence.** With autostart at login, everything that arrived overnight produces no notification. Hidden-level coalescing would already fold such a burst into one alert.
   - **Charter.** §5.2 N1 does not list this case.
   - **Suggestion.** Notify backfill once at hidden level, or add the rule to the charter.

## Verification

- **Checks.** `git diff --check 31615784..43dfa508` (excluding `.beads`) is clean.
- **macOS 15 (Darwin 24.6, arm64; SQLCipher via pkg-config, blueprint-compiler 0.20.4)**
  - **Configure.** `cmake -S . -B /tmp/w14rev -G Ninja -DBUILD_GROUNDHOG=ON`. Every Groundhog feature was reported enabled: store, outbox, account store, expiry, onboarding, notifier.
  - **Build.** 2149/2149 targets. There are no Groundhog compiler warnings; the only ones are in nostrdb, libmarmot and gnostr, plus the repo-wide `ld` duplicate-library notice.
  - **Tests.** `ctest --test-dir /tmp/w14rev -R 'groundhog-|nostrc-test-bus' -j4`: **46/46 passed**. Four platform skips are included (no WindowServer session or keyring): `groundhog-launch`, `groundhog-store-key-keyring`, `groundhog-background-gui` and `groundhog-notifier-gui`.
  - **Static checks.** `groundhog-privacy-static` and `groundhog-blueprint` passed.
  - **Stress.** `--repeat until-fail:5` over 14 suites: `expiry`, `notifier`, `composer`, `e2e-dm`, `onboarding`, `inbox-setup`, `contact-directory`, `auth-policy`, `account-store-gui`, `account-store`, `preferences`, `conversation-view`, `conversations` and `outbox`. **70/70 passed**, with 0 "Bad file descriptor" lines.
- **Not run.** I did not run Linux/Docker, and I did not contact the suggested relays; their evidence strings were read, not re-probed.
- **Code audits.**
  - **Code paths traced:** the default-timer path, the purge, notifier withdrawal and activation, and the onboarding connection points.
  - **Callers grepped:** every preference key's consumer, every `GH_AUTH_PURPOSE_*` call site, and the callers of `gh_expiry_set_timer`, `timer_slot` and the directory's name accessors.
  - **Logs:** every added log call was checked.
  - **Beads read (no change):** `nostrc-qp24.10.6`, `.25`, `.37` and `.63`.

**REQUEST CHANGES**: scoped to `43dfa508` versus `31615784`.

- **W13b is resolved.** B1 (history paging) and B2 (honest Preferences) are resolved, and non-blocking #1, #3, #4a, #5 and #6 are done.
- **One new blocking finding.** It is small to fix:
  1. The now-live Default Timer permanently turns on disappearing messages for every new conversation, including incoming requests. There is no pre-send indicator and no way to turn it off. Gate the row until a per-conversation control ships, or land G19's control first, with a test.
- **Everything else is sound:**
  - G07's expiry cryptography (PT-7 bounds, per-layer jitter, re-signed wraps) and the purge;
  - G14's PT-9 behaviour and an honest, banned-relay-free suggestions file;
  - G16's levels, coalescing, withdrawal and opaque activation targets;
  - G08's AUTH table, with SELF_WRAP as ACCOUNT on challenge;
  - G10's PT-8 and S2;
  - G13's T-enqueue before the signer, drafts and seam wiring.
- **Version.** 0.7.0 is the right bump. Update the metainfo with it.
