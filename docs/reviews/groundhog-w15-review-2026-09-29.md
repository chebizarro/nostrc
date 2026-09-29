# Groundhog W15 peer review: G24, G18, G19, the G18/G19 block reconciliation, G20a (2026-09-29)

## Context / scope

This is an independent review of `43dfa508..67aefa0f` (the HEAD of `groundhog/w15-review`).

- **Ignored.** The beads-only commits (`7fbaa035`, `d6e3404c`) and the W14 review document (`9bb2b12f`).
- **Not re-reported.** W14's findings are being remediated separately in `nostrc-qp24.81`, so they are not repeated here. That includes blocking #1 (the Default Timer on incoming requests).
- **Normative references.** `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`, `docs/nips/{11,17,29}.md`, and the earlier reviews in `docs/reviews/`.
- **Line numbers.** Every file:line refers to `67aefa0f`.

| Commit | Item | What it does |
|---|---|---|
| `96a351ba` | **G24** + `qp24.15.2` | The privacy acceptance harness (`tests/privacy/*`), the H7 canary scanner, `wire-relay.h` store-and-serve, the ASAN/UBSAN CI job and `tests/lsan.supp` |
| `e7a343ff` | **G18** | New Message, NIP-05 lookup consent over `GhNetHttp`, and Message Requests (Accept/Delete/Block) |
| `a1cee86f` | **G19** | Conversation Info, the per-backend privacy summary, Verify (safety code), mute, the timer, Block and Forget |
| `034fb462` | reconciliation | One block model: `set_blocked` keeps history and `block_and_forget` does not. Messages are seen-only while blocked, and only a "new" own message lifts a block |
| `67aefa0f` | **G20a** | `GhNip29Service`, `GhNip29Outbox`, `gh-store-nip29.c`, and NIP-11 over `GhNetHttp` |

I changed no code or beads. A throwaway probe test (described in B1) was added locally, run, and then reverted. `git status` is clean.

## Findings

### Blocking

1. **A block can never be lifted by writing to the person from this device, so a blocked contact's replies are silently and permanently discarded.** This comes from `034fb462`, in `gh-store-conversations.c:366-369` and `:507-508`.
   - **The rule as written.** The reconciliation lifts BLOCKED only when `fresh && after`, where `fresh = result != GH_STORE_ADMIT_DUPLICATE`.
   - **Every real send is a DUPLICATE.** Each message sent from this device goes through T-enqueue first:
     - `gh_outbox_send` calls `gh_store_ensure_conversation` (`gh-outbox.c:1603`), which leaves an existing BLOCKED row as it is.
     - `gh_store_enqueue` inserts the message row **and** its rumor id into `seen` (`gh-store.c:2659`: "the self-copy that comes back from our inbox is then a duplicate").
     - The send UI's local echo (`gh-send-ui.c:112-131`) then reaches `delegate_admit` as a DUPLICATE. Its self-copy returning from the inbox is also a DUPLICATE.
   - **So `fresh` is never TRUE for a message the user writes in Groundhog.** Only a self-copy sent from *another* client can lift a block. That contradicts the header (`gh-store-conversations.h:162-163`: "A new own message to the room (writing to them again) lifts the block") and the Requests copy (`gh-requests-view.c:274-275`: "Writing to them again unblocks them").
   - **Reproduced.** I added a probe to `test_requests.c`, then reverted it:
     1. The probe calls `set_blocked(TRUE)`.
     2. It does what `GhOutbox` does (`gh_store_ensure_conversation` plus `gh_store_enqueue`), then the echo through `gh_conversation_store_add_message`.
     3. Result: `GH_CONVERSATION_ADD_HIDDEN`, and the room is still blocked.
     4. The peer's next message also returns `ADD_HIDDEN`: it is recorded seen-only and never stored.
   - **Why tests pass.** `test_accept_delete_block_persist` (`test_requests.c:533`) and `test_replayed_self_copy_keeps_block` (`:570`) "write again" with a bare `gh_conversation_store_admit(model, own_msg, NULL)`. No T-enqueue precedes it, which never happens in the app.
   - **Failure scenario.**
     1. The user blocks Bob from a request (`block_and_forget`), or from Conversation Info and lets the Undo toast expire.
     2. Later they reconcile. They open New Message, pick Bob and send "sorry, let's talk".
     3. `open_room` lists an empty in-memory room, since the blocked room is not in `self->rooms`.
     4. The message is published to Bob, but its echo is hidden (`gh-send-ui.c:127` logs "could not show a queued message"). The user's own message never appears.
     5. Bob's reply arrives, is recorded as seen and is dropped, and it can never be recovered.
     6. After a restart the room is skipped again.
   - **No other way out.** No UI unblocks a room once the toast is gone: `set_blocked(FALSE)` has no other caller. The block is effectively permanent, and the copy promises the opposite.
   - **Required.**
     - Lift the block on the T-enqueue path. For example, have `gh_outbox_send`, or `gh_store_ensure_conversation` with the ACCEPTED intent, set `request_state = 0` when the user sends, in the same transaction.
     - Alternatively, carry an explicit "user-authored now" flag into `admit_read_state` instead of inferring it from `result`.
     - Keep the replay protection for relay-delivered self-copies (non-NULL wrap, DUPLICATE, or before the block).
   - **Test.** A regression test that goes through `GhOutbox` (or `gh_store_enqueue`) followed by the echo, then asserts the block is lifted and the peer's reply is listed.
   - **Worth deciding too.** Should "after" be measured against the time of the block rather than the read marker? A self-copy written on another device before the block, and delivered late by a relay, currently lifts it.

2. **The safety code can be matched by a MITM with about 2^30 work per side, yet the UI says matching codes prove both sides have the real keys.** See G19, `gh-privacy-summary.c:392-416`, `gh-privacy-summary.h:85-93` and `gh-conversation-info-dialog.blp:213`.
   - **The construction.** The code is `SHA-256(domain || min(A,B) || max(A,B))` truncated to 60 bits. Each device computes it over the pair *it* holds.
   - **What an attacker needs.** Suppose Mallory has shown Alice `K1` as "Bob" and shown Bob `K2` as "Alice" (a spoofed NIP-05 or contact card, which is exactly the case Verify exists for).
     - Alice's device shows `code(A, K1)`; Bob's shows `code(K2, B)`.
     - Mallory controls both `K1` and `K2`, so matching them is a **collision** between two sets she generates, not a second preimage.
     - By the birthday bound that takes about 2^30 keys per side, a few billion secp256k1 point additions plus SHA-256: minutes on a GPU, hours on a laptop.
   - **Why the design missed it.** The header comment's reasoning ("60 bits keep grinding a look-alike key … out of reach") only considers a second preimage against one fixed pair.
   - **The copy overclaims.** The dialog tells users "Matching codes mean you both have each other's real keys". Most users will compare only the emoji code.
   - **Required.** Do one of the following, and fix the copy to match:
     - **Per-party fingerprints**, as Signal does. Show `F(own key)` and `F(their key)` as two halves, each at least 60 bits (ideally with iterated hashing). A MITM then needs second preimages against *both* real keys, at 2^60 each.
     - **At least 120 bits combined**, so the collision bound is 2^60 or more.
   - **Test.** Assert the construction's property: for fixed `A` and `B`, the displayed code must change when `K2 ≠ A` even if `K1` is chosen freely. With per-party halves, `F(K1)` is compared against `F(B)`.

3. **G20a breaks the main Groundhog CI job.**
   - **The cause.** `groundhog-nip29-service` is registered whenever the NIP-29 service and the wire-relay pkg-config are available (`CMakeLists.txt:1490-1512`), which they are on ubuntu-24.04 with `libsoup-3.0-dev`. But `test-groundhog-nip29-service` is not in the job's explicit `--target` list (`.github/workflows/groundhog-ci.yml:47-63`).
   - **What happens.** `ctest -R '^(groundhog-|…)'` reports it as `***Not Run`. The pipeline fails under `set -o pipefail`, and the job's own `grep 'Not Run'` guard fails it too.
   - **Reproduced here.** With the executable moved aside, `ctest -R '^groundhog-nip29-service$'` gives "406 - groundhog-nip29-service (Not Run)".
   - **Also missing.** `groundhog-nip29-service` is not in the required-registration set, so the 13 G20a acceptance cases could silently drop out of CI if the CMake gate ever fails.
   - **Required.** Add `test-groundhog-nip29-service` to the build targets and `groundhog-nip29-service` to `required`.

### Reviewed areas — no blocking finding

- **G24 harness (PT-1/2/4/8/10, NO-1, AT-5).** It is not vacuous:
  - **Scanner self-test.** `test_h7_scanner` proves every surface the scanner covers can detect a planted canary: raw, hex, base64 at all alignments, UTF-16LE, `-wal`/`-shm`, GSettings, relay frames, GLib logs and the fd-level stderr tee.
  - **`world_scan` cannot pass on nothing.** It asserts that files were scanned (`files > 0`), that log and fd probes are present, that both stores exist, are at least 4 KiB and are not plain SQLite (`:983-991`), and that the D/F/M tripwires saw zero attempts.
  - **PT-1.** Checked per connection. Pre-script connections may not send EVENT, and AUTH on them is limited to the account's own inbox. Barrier connections may send only the barrier's wrap ids, and their REQs are shape-checked as directory lookups of the peer.
  - **PT-2.** It has a built-in tripwire: if the inbox ever admits kind 15, the test aborts (`:1569-1571`). The AT-5 attachment skip therefore cannot outlive `qp24.11`.
  - **Gaps are honestly documented.** Pin and mark-unread are not implemented (`:1360-1364`), and the same-second read-marker issue (`qp24.75`) is worked around with `wait_next_second`. See non-blocking #6 for what PT-1 does not drive.
- **`lsan.supp`.** The entries are narrow and each names its bead (`nostrc-jwj0`).
  - `subscription_filters` is a Groundhog function kept `G_GNUC_NO_INLINE` precisely so that a leaked `GNostrSubscription` still reports (`gh-relay-gnostr.c:108-113`).
  - `reset_subscription` disconnects Groundhog's handlers before `gnostr_subscription_close`, so no Groundhog callback runs under a suppressed frame.
  - CLOSED dispatch hands the reason to a channel rather than calling back synchronously.
  - See non-blocking #7 for a substring nit.
- **G18 network consent (PT-9).**
  - **One HTTP client.** `GhNetHttp` is the only libsoup user in `src/` (grep). It has no cookie jar, cache, HSTS or User-Agent, drops the content sniffer, sets `SOUP_MESSAGE_NO_REDIRECT` and refuses 3xx.
  - **Other limits.** It enforces a size cap (header and read-to-cap+1), allows plain http only to loopback, refuses userinfo, fails closed in Tor mode or any unknown mode, and keeps cancellation.
  - **NIP-05.**
    - **What is fetched.** One GET happens only after the consent row names the domain. The domain must be a host name, not an IP literal, and `.onion` is refused.
    - **Parsing.** Names are matched case-insensitively and the key must be hex64.
    - **The inbox check.** It is a separate explicit row whose copy says the discovery relays "learn whom you asked about".
  - **Requests.**
    - **PT-8.** No kind-0 or directory lookup happens until Accept, and notifications are forced hidden.
    - **Delete.** It forgets the request and keeps a tombstone.
    - **Nothing is published.**
- **The rest of the reconciliation.** Apart from B1 it is sound:
  - While blocked, another sender's message becomes seen-only (wrap and rumor) inside the T-admit transaction, before any storage.
  - A replayed duplicate or an older self-copy no longer unblocks the room.
  - `delegate_accept` never touches a BLOCKED room.
  - The Block copy no longer promises "kept hidden".
- **G19.**
  - **Privacy summary.** The copy is honest per backend and the snapshots match `gh-features.h`:
    - a relay group is never called encrypted;
    - disappearing messages are a request;
    - the IP address and Nostr use are listed as unprotected;
    - the storage line follows the store mode.
  - **Local-only state.** Mute lives in the database (NO-7), and "Mark as Verified" is recorded only locally (`contacts.verified_at`, schema v3).
  - **Forget** uses the ST-9 tombstone.
- **G20a.**
  - **Relay-signed state.** 39000–39003 are held until a key is pinned, then admitted only through `gh_nip29_group_admit_at`, which requires the pinned author. A foreign signer triggers one forced re-fetch per session, and a new key resets the group state.
  - **Moderation events.** 9000/9001/9005 count only from the relay key or an admin listed in 39001.
  - **The `previous` tag.** It comes from a newest-first ring of 50 entries that excludes the author's own events, as NIP-29 specifies. Deleted events leave the ring.
  - **NIP-11.**
    - **Transport.** It uses `GhNetHttp` with `Accept: application/nostr+json` and never follows redirects.
    - **Plaintext.** `ws://` off loopback returns `GH_NIP11_ERROR_PLAINTEXT` with no retry and no plaintext fallback.
    - **When it runs.** It is fetched only from `relay_resubscribe`/`relay_ensure_key`, for rooms in a joined state. Restored rooms keep their pinned key.
  - **AUTH.** It goes only through `gh_auth_policy_apply_scope` and `gh_auth_policy_apply_publish` with `GH_AUTH_PURPOSE_GROUP`, scoped to the group relay URL.
  - **Store.** `gh-store-nip29.c` admits each event with the `seen` row in one transaction.
  - **Honest UI.**
    - The group header says "Relay group · not end-to-end encrypted".
    - The composer refuses the NIP-17 path for a group.
    - Group notification previews name the author as an npub, and only at the `preview` level.

### Non-blocking (follow-up suggested)

1. **The NIP-29 since-cursor advances before EOSE and before the message is stored (G20a). History gaps are permanent and not flagged.**
   - **Where.** `room_admit_message` raises `room->cursor` from every event as it arrives (`gh-nip29-service.c:949-953`), before `gh_conversation_store_admit` (`:963`), and even when that admit fails (`:965-967`).
   - **The REQ.** `relay_filters` then sends `since = cursor - 600` with no `limit` (`:734-737`). EOSE only sets `backfilled` (`:1046-1049`).
   - **How the gap forms.** Relays usually return stored events newest-first and cap the answer (typically 100–500).
     - The connection can drop mid-backfill.
     - Or, after a long offline period, more messages may have accrued than the relay's cap.
     - Either way, the cursor jumps to the newest event and everything older in between is never requested again.
   - **No recovery.** `gh_nip29_service_load_older` only reads the local store (`:1746-1751`).
   - **The claim.** The commit message says "EOSE-aware backfill".
   - **Suggestion.**
     - Keep a pending cursor that is committed at EOSE, and only for events that were admitted.
     - Page older history with `until` when the relay's answer reaches its cap.
     - Otherwise, mark the room "may be incomplete".
   - **Status.** It is latent until G20b gives groups a UI, but it should be fixed before then.

2. **Conversation Info is enabled for NIP-29 rooms but offers only NIP-17 controls and copy (G19 × G20a, latent until G20b).**
   - **Where.** `sync_action` enables `win.conversation-info` for any shown conversation (`gh-conversation-info-dialog.c:862-864`), and G20a lists group rooms.
   - **What fails.**
     - Block, Forget and Mute go through `GhStoreConversations`, whose `check_room` rejects non-NIP-17 ids ("Not a NIP-17 room of this account", `gh-store-conversations.c:123-137`).
     - The Block and Forget copy says "encrypted copies may remain on relays" and "deliver their messages to your message relays", which is false for a relay group (`:676-688`).
   - **Why it is latent.** No UI calls `gh_nip29_service_join` yet, so no group room can exist in the shipped app.
   - **Suggestion.** Route by backend, or disable the action for NIP-29 until `gh-group-info-dialog` (G20b) lands.

3. **Block is per room, while the charter and copy describe a per-sender block list (G18).**
   - **The charter.** §7.9 says "Block (local block list)", and the schema has `contacts.blocked` (`gh-store-schema.c:119`), which nothing writes.
   - **The copy.** "You won't see their messages or get notifications from them" (`gh-requests-view.c:273-275`).
   - **The gap.** The blocked sender reappears in Message Requests by writing from any other room id, for example a NIP-17 conversation that adds one more participant.
   - **Suggestion.** Either implement the contact-level block, or say "this conversation" in the copy.

4. **NIP-11 `pubkey` fallback (G20a).**
   - **What the code does.** `gh_nip11_parse_relay_key` falls back to `pubkey` when `self` is absent (`gh-nip11.c:65-76`).
   - **What the NIPs say.** NIP-29 says 39000–39003 are signed by the NIP-11 **`self`** key. NIP-11 defines `pubkey` as the administrator's contact key.
   - **Impact.** It is compatible with older relay29 deployments. On a relay whose `pubkey` is a person, that person's key is pinned, so it can speak for the group (`room_trusts`), and the relay's real 39000s are held as foreign.
   - **Suggestion.** Record which field was used, and show the state as "unverified (relay has no `self` key)" rather than pinned.

5. **The sanitizer job does not cover the new display-free suites.**
   - **Missing.** `groundhog-new-message`, `groundhog-requests`, `groundhog-privacy-summary`, `groundhog-conversation-info` and `groundhog-nip29-service` all run without a display but are absent from `GROUNDHOG_SANITIZER_TESTS` (`groundhog-ci.yml:162-170`).
   - **Why it matters.** The new async code (NIP-05 and NIP-11 `GTask`s, the key-fetch `KeyCall`, and `Pending` in the dialogs) is therefore never run under ASAN/LSan.

6. **PT-1 drives store APIs, not the UI glue (G24).**
   - **What it does.** `run_pt1` calls `gh_store_conversations_set_draft`, `_set_muted_until` and `gh_conversation_mark_read` directly (`test_privacy_e2e.c:1403-1430`).
   - **What it misses.** A regression in the composer or the conversation view, for example a future "typing" hook in `GhSendUi`, would not be caught.
   - **Suggestion.** Acceptable for a display-free harness. Add a `--gui` PT-1 variant once the window can be composed on the private bus.

7. **An `lsan.supp` pattern is a substring.** `leak:nostr_subscription_close` also matches `gnostr_subscription_close`.
   - **Risk today.** None: handlers are disconnected first.
   - **Suggestion.** Anchor the pattern (`^nostr_subscription_close$`) so that a future Groundhog "closed" handler reached through the wrapper cannot hide a leak.

8. **`checks_pending` can be corrupted by callbacks from a cancelled round (G18).**
   - **Where.** `reset_cancellable` zeroes `checks_pending` (`gh-new-message-dialog.c:956-962`), but `on_checked` decrements it for cancelled results too (`:918-919`).
   - **Scenario.**
     1. The user taps Check, goes back, then presses Next and Check again quickly.
     2. The late cancelled callbacks decrement the new round's count.
     3. The spinner stops early, and "Check finished" is announced while checks are still running.
   - **Suggestion.** Ignore cancelled results before touching the counter, or tag each round.

9. **Minor consistency.**
   - **(a) An alert built in C.** The Requests view builds its AdwAlertDialog in C (`gh-requests-view.c:278`), while G19 keeps its alerts in Blueprint. Move it into `gh-requests-view.blp` for the Blueprint-only rule.
   - **(b) A duplicated subtitle.** The NIP-29 header subtitle is hard-coded in `gh-conversation-list.c:162-168` instead of taken from `gh-privacy-summary`, which G19 made the single strings table for header subtitles. The strings are identical today, but they can drift.

## Verification

- **Checks.** `git diff --check 43dfa508..67aefa0f` (excluding `.beads`) is clean.
- **macOS 15 (Darwin 24.6, arm64; SQLCipher via pkg-config)**
  - **Setup.** Submodules were initialized first: the fresh worktree lacked `third_party/nostrdb`.
  - **Configure and build.** `cmake -S . -B /tmp/w15rev -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w15rev`: 2231/2231 targets. There are no Groundhog compiler warnings; the only ones are in nostrdb, libmarmot and gnostr, plus the `ld` duplicate-library notice.
  - **Restored.** `apps/gnostr/data/ui/dialogs/gnostr-profile-edit.ui` was rewritten by the build and restored.
  - **Tests.** `ctest --test-dir /tmp/w15rev -R 'groundhog-' -j6`: **53/53 passed**, including `groundhog-privacy-e2e`, `-nip29-service`, `-requests(-gui)`, `-new-message`, `-conversation-info(-gui)` and `-privacy-summary`. Four platform skips are included: `groundhog-launch`, `-store-key-keyring`, `-background-gui` and `-notifier-gui`.
  - **Stress.** `--repeat until-fail:5` over the 11 suites this range touches most (privacy-e2e, nip29-service, nip29, requests, requests-gui, new-message, conversation-info, conversation-info-gui, privacy-summary, composer, store-conversations): all passed.
- **Probes.**
  - **B1.** A local test drove `gh_store_ensure_conversation` plus `gh_store_enqueue` plus the echo against a room blocked with `set_blocked`. It observed `ADD_HIDDEN` for the echo, the room still blocked, and `ADD_HIDDEN` for the peer's reply. I then reverted it and rebuilt the test binary.
  - **B3.** Moving `test-groundhog-nip29-service` aside makes ctest report "Not Run".
- **Not run.** Linux, the ASAN job and real public relays.

**REQUEST CHANGES**: scoped to `67aefa0f` versus `43dfa508`.

- **Blocking:**
  1. A block is never lifted by the user writing from Groundhog: T-enqueue makes every own send a DUPLICATE. Replies from a contact the user has re-engaged are then silently discarded, the user's own message is hidden, and no other UI unblocks. Lift the block on the send path, and test it through the outbox.
  2. The 60-bit combined safety code is collision-matchable by a MITM (about 2^30 per side), while the UI says matching codes prove the real keys. Use per-party fingerprints, or 120 bits or more, and fix the copy.
  3. CI: `test-groundhog-nip29-service` is registered but not built in the main job ("Not Run"). Add it to the targets and to `required`.
- **Everything else is sound:**
  - G24's harness is non-vacuous and its suppressions are narrow.
  - G18 contacts nothing before consent, and `GhNetHttp` is the only HTTP client.
  - G19's privacy copy is honest per backend.
  - G20a's relay-key pinning, the `previous` tag, AUTH purpose, NIP-11 policy and one-transaction dedup meet the brief.
- **Before G20b.** Fix the NIP-29 cursor (non-blocking #1) and the info-dialog routing (#2).
