# Groundhog W15f re-review: W14 and W15 remediation, the ordered EOSE fix, 0.8.0 (2026-09-29)

## Context / scope

This is a focused, independent re-review. It covers the fixes that answer two REQUEST CHANGES reviews, plus one shared-library fix and the version bump. HEAD is `b7789088` (branch `groundhog/w15f-rereview`), and file:line references are to that commit.

| Commit | Bead | Answers |
|---|---|---|
| `553e6cef` | `nostrc-qp24.10.6` | nostr-gobject `GNostrSubscription` delivers EVENT, EOSE and CLOSED through one ordered queue (2.0.2). Groundhog's `G_PRIORITY_LOW` workarounds are removed (W14 non-blocking #4) |
| `47f3cb68` | `nostrc-qp24.81` | `docs/reviews/groundhog-w14-review-2026-09-28.md` (B1 and non-blocking #1, #2, #3, #5, #6, #7b) |
| `283c2b11` | `nostrc-qp24.82` | `docs/reviews/groundhog-w15-review-2026-09-29.md` (B1, B2, B3 and non-blocking #1, #2, #4, #5, #7, #8, #9) |
| `b7789088` | — | Groundhog 0.8.0 and its release notes |

- **Ignored.** The beads-only commits (`ff7e9e23`, `0950156f`, `368e0e62`) and the W15 review document (`9e25b7ea`).
- **Normative reference.** `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`.
- **Changes.** I changed no code or beads.

## Prior blocking findings

### W14 B1: the Default Timer made new conversations disappear silently, with no way out. **Resolved.**

- **Per-conversation control.** It already existed from G19 (Conversation Info's timer, `gh_expiry_set_timer`), and the composer now leads to it.
- **Pre-send indicator.** `timer_slot` holds a flat pill: an alarm icon and "1 day" (`gh-composer.blp:63-97`, `gh-composer.c:218-243`).
  - Its tooltip and accessible label are "Messages you send disappear after 1 day".
  - Its accessible description says it opens Conversation Info.
  - It activates `win.conversation-info`.
  - It is hidden while the timer is off.
  - `describe_timer` never rounds down: a non-whole-day value is shown in hours, rounded up.
- **Wiring.**
  - `gh-send-ui.c:286-328` reads `gh_expiry_get_timer` for the shown room on every view change.
  - `"timer-changed"` keeps the indicator live. The handler is `g_signal_connect_object`'d to the composer, and `use_expiry` disconnects it from the previous `GhExpiry`.
  - `gh-app-services.c` hands every window the current `GhExpiry`:
    - at attach;
    - after `expiry_sync`;
    - after a default change, because a not-yet-stored room shows the default.
  - It hands them `NULL` before disposing the `GhExpiry` (`expiry_stop`, steal → share → dispose), so no window keeps a disposed `GhExpiry`.
  - A non-NIP-17 room reads as `INVALID_ARGUMENT` → 0 (hidden), which is correct for a relay group.
- **Incoming requests.** They still inherit the default. That was a decision the W14 review left open, and it is now disclosed in three places:
  - the Preferences subtitle: "Only for new conversations, including ones others start. Existing conversations keep their own timer." (`gh-preferences-dialog.blp:128`);
  - the gschema description;
  - the composer indicator the user sees before the first send.
- **Tests.** `/groundhog/composer/timer-indicator` and `timer-live` ran here and are stable (stress below).

### W15 B1: a block could never be lifted by writing from this device. **Resolved.**

- **The block is lifted where the message is first stored.**
  - T-enqueue sets `request_state = 0` in the same transaction as the outgoing message, its outbox row and its `seen` rumor id (`gh-store.c:2665-2677`).
  - A repeat of the same `op_id` returns before this point, so it lifts nothing.
  - The local echo that follows is a DUPLICATE, finds the room unblocked (`gh-store-conversations.c:505-515`) and is listed.
- **T-admit no longer lifts a block.** `admit_read_state` now only accepts `request_state = 1` (`:366-367`). That covers every relay-delivered self-copy: replayed, older than the read marker, or written on another device. It honours the W15 request to keep replay protection, and it settles the "measured against the read marker" question in the conservative direction. The rule is documented as per-device (P8) in `gh-store-conversations.h:160-178`.
- **The reachable UI path is New Message.** A blocked room is not listed, so the user reaches it through New Message.
  - `gh_conversation_store_open_room` asks the delegate `is_blocked`/`unblock` (`gh-conversation-store.c:486-506`). `unblock_room(accept=TRUE)` sets `request_state = 0` and restores the kept page.
  - After `block_and_forget` nothing is kept, so an empty accepted room is listed.
  - The confirm page says so before anything changes (`gh-new-message-dialog.c:998-1018`): "You blocked this conversation. Starting it unblocks it … Messages they sent while it was blocked aren't shown." That is accurate, since those were recorded seen-only.
  - `open_room`'s only caller is New Message (grep), so nothing else can unblock implicitly.
- **Test realism.** `/groundhog/outbox/send-lifts-block` (`test_outbox.c:2383-2445`) is the regression test the W15 review asked for. It runs:
  1. a real `GhOutbox` send (T-enqueue);
  2. the echo through `gh_conversation_store_add_message` → NEW;
  3. the peer's reply → NEW, unread 1;
  4. a restart that lists all 4 messages.

  It also asserts that a post-block self-copy from "another device" stays HIDDEN and the room stays blocked. `test_requests.c` covers the New Message path, both in-store and with `--gui`.
- **Copy.** Block in Requests (`gh-requests-view.blp:200`) and in Conversation Info (`gh-conversation-info-dialog.c:686-695`) now says "this conversation" and how to unblock. W15 non-blocking #3 (a per-sender block) stays open as `nostrc-qp24.79`, and the copy no longer overclaims it.

### W15 B2: the safety code could be matched by a MITM with a collision. **Resolved.**

- **Construction.** Each person gets their own code (`gh-privacy-summary.c:326-379`):
  - SHA-512("groundhog-fingerprint-v1" ‖ key), then 5199 more rounds of SHA-512(digest ‖ key);
  - 8 groups, each 40 bits (5 digest bytes, big-endian) mod 10000.
- **Checking the claim.**
  - 32 decimal digits are log2(10^32) ≈ **106.3 bits** per person, so "106 bits" is right.
  - The mod-10000 bias from 2^40 is below 10^-8 per group and negligible.
  - The per-party layout means a MITM showing Alice K1 and Bob K2 must find F(K1) = F(B) and F(K2) = F(A). Each is a second preimage of a *fixed* 106-bit code, about 2^106 × 5200 hashes, not a birthday collision.
  - This is the Signal safety-number shape (per-party, iterated SHA-512, 5200 rounds), with slightly more digits per party (32 against 30).
- **The property the W15 review asked for.** It follows from the construction: the code shown as "their code" depends only on the key held for them. `test_privacy_summary.c` pins the vectors and the format.
- **The copy is honest.** "Safety Codes" (`gh-conversation-info-dialog.blp:215-217`):
  - says to compare both codes in person or on a call you trust;
  - says the other side sees the same two codes the other way round;
  - says what a match proves ("the key you have for them is the one they use, and they have yours");
  - says what to do on a mismatch ("don't mark the key verified");
  - says that other apps don't show these codes and that the full keys are the check there.
- **Accessibility.** The accessible labels read the codes digit by digit, with pauses between groups.
- **The verified record.** It stores (key, time), never a code, so the format can change without a migration.

### W15 B3: CI did not build `test-groundhog-nip29-service`. **Resolved.**

- **Main job.** The target was added to the main job's `--target` list, and `groundhog-nip29-service` to `required` (`groundhog-ci.yml:63-64,91`).
- **Sanitizer job.** It now also lists new-message, requests, privacy-summary, conversation-info and nip29-service, which answers W15 non-blocking #5.
- **Registration check.** I configured the sanitizer job's exact CMake shape locally (`BUILD_NOSTR_GTK=OFF BUILD_APPS=OFF SIGNET_ENABLE=OFF`). All five register (`ctest -N`), so the job's registration guard will not trip.

## Prior non-blocking items addressed

| Item | Status | Evidence |
|---|---|---|
| W14 #1 NO-11 at defaults | Done | `notice_allowed` (`gh-notifier.c:572-578`) shows the notice unless the user explicitly set `notifications-enabled=false`. `g_settings_get_user_value == NULL` counts as allowed. The notice names nothing. Charter §5.3 B4 and NO-11 are amended. `/groundhog/notifier/no11-default-settings` resets the key and asserts the notice |
| W14 #2 Check Privacy | Done | The visible `check_note`, "Checking connects to the ticked relays. They see your IP address, not your account.", is wired as `described-by` on the button (`gh-onboarding-view.blp:500-510`). The contradicting footer is gone |
| W14 #3 seal jitter | Done (charter) | §0.1 #8, A1, PD-7, §3.7, PT-7 and EX-1 now say the seal is jittered, with the rationale and the interop cost (≤ ~25 h longer at a seal-honouring client). The code is unchanged (`gh-expiry.c:67-81`) |
| W14 #4 EOSE race | Done | `553e6cef`, see the next section |
| W14 #5 composer IME | Done | `preedit-changed` tracks the preedit, and Enter during a preedit goes only to the IM and stops (`gh-composer.c:278-283`). Shift+Enter and Enter with `enter-sends` off propagate unfiltered to the text view's own controller, so there is no double filtering. `/groundhog/composer/preedit-enter` drives a real `GtkIMContextSimple` hex sequence. §7.7 is amended to say why capture phase is used |
| W14 #6 metainfo | Done | The description says what ships and what doesn't (no NIP-29/MLS, attachments, Tor) and keeps the §1.4 non-goals. `appstreamcli validate --no-net` passes, with only `developer-info-missing` |
| W14 #7b directory comment | Done | `gh-contact-directory.[ch]` now disclose that onboarding's discovery adoption can put the own inbox relays in the lookup set |
| gschema | Done | "Reserved; not used yet" is gone from live keys, and the descriptions match behaviour |
| W15 #1 NIP-29 cursor | Done (cap paging open, `nostrc-x055`) | Backfill raises `sync_cursor` only, committed at EOSE unless an admission failed (`gh-nip29-service.c:947-966,1075-1081`). A live event moves the cursor at once. A failed admission freezes it for the rest of the REQ. `relay_resubscribe` resets both. On a reconnect, `GhRelayScope` clears `endpoint->eose`, so a re-backfill is again "backfill", and the REQ is rebuilt from the committed cursor. A carried-over `sync_cursor` therefore only commits after a complete re-backfill |
| W15 #2 info dialog routing | Done | `shown_conversation` returns only NIP-17 rooms (`gh-conversation-info-dialog.c:862-872`), so `win.conversation-info` is disabled for groups. The composer's timer button is hidden there too (timer 0) |
| W15 #4 NIP-11 `pubkey` | Done | Only `self` is read (`gh-nip11.c:61-76`). Without it the key is unavailable, and the relay's 39000s stay held |
| W15 #7 lsan anchor | Done | `leak:^nostr_subscription_close$`. sanitizer_common's template matcher honours `^`/`$` |
| W15 #8 check rounds | Done | `Pending.round` is compared with `check_round`, and a stale or cancelled answer touches neither the count nor the row (`gh-new-message-dialog.c:920-930,967-970`) |
| W15 #9 Blueprint / subtitle | Done | Requests alerts live in `gh-requests-view.blp`. The NIP-29 subtitle comes from `gh-privacy-summary` |
| W15 #6 PT-1 `--gui` | Deferred | `nostrc-lf9d` |

## `553e6cef`: nostr-gobject ordered delivery (shared with gnostr)

### Correctness

- **The ordering argument holds.**
  - libnostr's reader thread dispatches frames one at a time.
  - `nostr_subscription_dispatch_event` does a non-blocking `go_channel_try_send` on `events` (`libnostr/src/subscription.c:440`) before `dispatch_eose` does its own `try_send` on `end_of_stored_events` (`:561`).
  - So when the monitor receives an EOSE, every earlier event is already in `ch_events`. `queue_eose_for_main` drains that channel first.
  - An event that arrives just after the EOSE may be put ahead of it, which is harmless as documented.
  - `queue_closed_for_main` drains pending EOSEs, then events, then the CLOSED.
- **One drainer.** All three kinds share `event_queue`, and a single `DEFAULT_IDLE` source pops one item per lock. `event_idle_scheduled` stays TRUE until that source sees the queue empty, and GLib does not recurse a dispatching source, so a handler that iterates the main context cannot reorder.
- **Lifetimes.**
  - The drain source holds a ref, dropped by its destroy notify.
  - Finalize frees remaining `SubItem`s.
  - The monitor keeps its own ref until it exits.
- **Threading.** `self->state` is read and written only on the main thread: `emit_item`, `fire` and `close`.
- **The CLOSED reason.** The monitor now `free()`s the `strdup`'d reason from `dispatch_closed`. That was a leak before, not a new double free: the reason's only other consumer is `relay.c`'s synchronous query path, which never shares a subscription with the GObject monitor.
- **Queue cap.**
  - `EVENT_QUEUE_CAPACITY` (200) now counts only events (`queued_events`) and drops the oldest *event*, never a marker.
  - Markers are bounded by libnostr's EOSE coalescing (one queued at a time) and the single CLOSED.
  - The scan for the oldest event is O(markers ahead), which is effectively O(1).
- **No double `closed`.**
  - `gnostr_subscription_close()` emits `closed(NULL)` synchronously and sets CLOSED (`nostr_subscription.c:834-835`).
  - A relay CLOSED or EOSE drained afterwards is dropped by the state guard in `emit_item` (`:165-177`).
  - The reverse order returns early in `close()`.
- **Consumers.**
  - signet does not use `GNostrSubscription` (grep).
  - nostr-gobject's pool and gnostr's DM service connect `eose`/`closed` with no competing EOSE timeout.
  - Groundhog's `reset_subscription` disconnects before `close`.
  - `GhDmInbox`'s settle counts events on receipt and unwraps asynchronously, as it did before, so moving its idle from LOW to DEFAULT changes nothing it relied on.
- **Versioning.** 2.0.2 (PATCH) matches a behaviour fix with no API change.

### Behaviour changes worth a line in the library's notes (non-blocking, see #1 and #2 below)

- **EOSE priority and latency.** `eose`/`closed` are now emitted at `G_PRIORITY_DEFAULT_IDLE`, behind at most 200 queued events (≤ 4 drain ticks of 50), where before they had their own `G_PRIORITY_DEFAULT` idle.
- **Queued events still emit after `close()`.** Events queued before a consumer's `close()` are still emitted, as before, but a queued relay CLOSED is not.

## Findings

### Blocking

None.

### Non-blocking (follow-up suggested)

1. **Low: a relay's CLOSED reason is lost when the consumer closes first** (`nostr-gobject/src/nostr_subscription.c:173-177` with `:834-835`).
   - **Scenario.**
     1. A relay sends `CLOSED "auth-required: …"` right after a burst of events.
     2. A gnostr consumer calls `gnostr_subscription_close()` from an `event` handler or a deadline, while that CLOSED item is still queued.
     3. The consumer gets `closed(NULL)`, and the relay's reason is discarded by the new state guard.
   - **Before.** Both `closed` emissions fired: the "double closed" this commit removes.
   - **Impact.** None today, since Groundhog disconnects before closing and no gnostr consumer reads the reason after its own close.
   - **Suggestion.** Document in `nostr_subscription.h` that `closed` fires exactly once and that a locally initiated close carries `NULL`.

2. **Low: `eose` is no longer prioritised over event ingestion. Document it** (`nostr_subscription.c:180-190`).
   - **Scenario.**
     1. A gnostr timeline subscription receives a 200-event burst followed by EOSE.
     2. The "loading" state that clears on `eose` now clears after the four drain ticks, instead of on the next `G_PRIORITY_DEFAULT` dispatch.
   - **Correctness.** This is correct by design, and it is what makes EOSE honest.
   - **Header.** Nothing in the header states the new guarantee ("every EVENT received before EOSE is emitted before `eose`"), and gnostr and signet maintainers would want it written down.

3. **Low: the capacity cap can still make a one-shot "fetch until EOSE" miss stored events** (`nostr_subscription.c:233-252`; pre-existing, not a regression).
   - **Scenario.**
     1. More than 200 stored events queue before the main loop drains (for example, a blocked main loop at startup).
     2. The oldest queued events are dropped.
     3. EOSE is still delivered, so the consumer concludes it saw everything.
   - **Groundhog.** Its lookups (a single author, `limit` 1–2) cannot reach the cap.
   - **Suggestion.** If gnostr ever relies on completeness at EOSE, count dropped events per REQ and expose it, for example as an `eose` detail or a property.

4. **Nit: the Requests Block copy says "start a new message to them", but a group request unblocks only with the same people** (`gnome/groundhog/data/ui/gh-requests-view.blp:200`).
   - **Scenario.**
     1. A NIP-17 request from {Bob, Carol} is blocked.
     2. The user later starts a New Message to Bob alone.
     3. That opens the 1:1 room, and {Bob, Carol} stays blocked.
   - **Suggestion.** Conversation Info's copy already distinguishes "to the same people" (`gh-conversation-info-dialog.c:686-689`); reuse that wording.

5. **Nit: the new ordering regression test silently disappears without libsoup** (`nostr-gobject/CMakeLists.txt:428-437`, `pkg_check_modules(... QUIET ...)`).
   - **Scenario.** A nostr-gobject-only CI or packaging build without `libsoup-3.0-dev` registers no `test_nostr_gobject_subscription_eose_order`, so a regression of `nostrc-qp24.10.6` would go unnoticed there.
   - **Mitigation.** Groundhog's `/groundhog/dm-send/wire-lookup` covers the same path wherever Groundhog builds.
   - **Suggestion.** Add a `message(STATUS …)` when the test is skipped, or require it wherever nostr-gobject tests are required.

## Versioning

- **The bumps.** Groundhog went 0.7.0 → 0.7.1 (the EOSE fix) → **0.8.0** (`b7789088`). `VERSION_MANIFEST.md` and `project(groundhog VERSION 0.8.0)` agree.
- **MINOR is right.** New Message, Message Requests, Conversation Info with safety codes and the composer timer are new user-facing capabilities, and 0.8.0 is still unreleased.
- **Release notes.** The 0.8.0 entry lists them. The 0.7.0 entry now keeps only what 0.7.0 shipped.
- **nostr-gobject.** 2.0.2 in CMake, meson and the manifest.

## Verification

- **Checks.** `git diff --check 553e6cef~1..b7789088` (excluding `.beads`) is clean.
- **macOS 15 (Darwin 24.6, arm64; SQLCipher via pkg-config)**
  - **Setup.** Submodules initialised first (`third_party/nostrdb`, `third_party/nsync`).
  - **Configure and build.** `cmake -S . -B /tmp/w15f -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w15f`: 2233/2233 targets. There are no Groundhog or nostr-gobject warnings; the existing ones are in nostrdb, libmarmot and gnostr, plus the `ld` duplicate-library notice.
  - **Restored.** `apps/gnostr/data/ui/dialogs/gnostr-profile-edit.ui` was rewritten by the build and restored.
  - **Groundhog tests.** `ctest --test-dir /tmp/w15f -R 'groundhog-|nostrc-test-bus|eose' -j6`: **56/56 passed**. Four platform skips are included: `groundhog-launch`, `-store-key-keyring`, `-background-gui` and `-notifier-gui`.
  - **Library tests.** `ctest -R 'nostr_gobject|gnostr-|subscription|signet' -j6`: **73/73 passed**. That covers:
    - all `test_nostr_gobject_*`, including `subscription_eose_order`;
    - `libnostr_subscription_dispatch`;
    - `test_subscription_{lifecycle,backpressure,backpressure_long,blocking_depth}` and `concurrency_subscription_shutdown`;
    - every `gnostr-test-*` (including `startup-live-eose`, `thread-sub`, `timeline-source-live`, `sync-bridge-*`);
    - `signet_smoke` and `signetctl_credentials`.
  - **Stress.** `--repeat until-fail:5` over 21 suites passed **105/105**: composer, requests(-gui), outbox, nip29-service, dm-send, conversation-info(-gui), privacy-summary, new-message, notifier, store-conversations, contact-directory, inbox-lookup, dm-inbox, preferences, onboarding, privacy-e2e, `test_nostr_gobject_subscription_eose_order`, `gnostr-test-startup-live-eose`, `test_subscription_lifecycle` and `concurrency_subscription_shutdown`.
  - **CI shape.** Configured with the sanitizer job's flags (no ASAN): the five newly listed suites register.
  - **Metainfo.** `appstreamcli validate --no-net data/org.nostr.Groundhog.metainfo.xml` passes.
- **Code audits.**
  - **Traced:**
    - T-enqueue → echo → `delegate_admit`, for a blocked room;
    - `open_room` → `delegate_unblock` → `change_block` → `restore_room`;
    - the fingerprint arithmetic;
    - the `GhExpiry` hand-over in `gh-app-services.c`;
    - libnostr `dispatch_event`/`dispatch_eose`/`dispatch_closed` and their channel ownership;
    - `GhRelayScope`'s EOSE reset on disconnect, for the NIP-29 cursor.
  - **Grepped:** the callers of `gh_conversation_store_open_room` and `gh_store_enqueue`, the consumers of `GNostrSubscription` in gnostr, nostr-gobject and signet, and the `request_state` writers.
- **Not run.** Linux, the ASAN job itself, and real public relays.

**APPROVED**, scoped to `553e6cef`, `47f3cb68`, `283c2b11` and `b7789088`.

- **W14 B1 is resolved:** a composer timer indicator, a live and honest Default Timer, and a way to change the timer.
- **W15 B1, B2 and B3 are resolved:**
  - sending or starting a New Message lifts a block, while relay self-copies never do;
  - per-person 106-bit iterated safety codes with honest copy;
  - the NIP-29 service is built and required in CI, and five suites were added to the sanitizer job.
- **The nostr-gobject change is correct** for gnostr and Groundhog, and signet does not use it. EVENT/EOSE/CLOSED keep wire order, markers are never dropped, and `closed` fires once.
- **Nothing blocks.** The five non-blocking items are documentation and copy nits plus one pre-existing cap limitation.
