# Groundhog social lists, drafts, reporting and badges: Plan

## Goal

Add NIP-02 follow-gated avatars, NIP-37 DM draft sync, the NIP-51 list family (kinds 10000, 10006, 10008, 10009, 10013, 10030, 10050, 10063, 10992) with their set and definition kinds, NIP-56 reporting with an offer to hide the reported account, and NIP-58 badges to Groundhog (`gnome/groundhog`). Generalize protocol and list logic into `nips/` and `nostr-gobject` wherever possible, and amend the privacy charter where owner decisions change it.

## Owner decisions

These were made on 2026-11-08 and override older documents, per AGENTS.md "Scope and delivery".

- **D1, publishing private state: opt-in sync, off by default.** The encrypted store stays the source of truth. A per-account "Sync Drafts and Lists" switch publishes NIP-44 encrypted events, and nothing new syncs until it is on. Charter P8 is amended to allow this opt-in, with a P9 explanation.
- **D2, avatars: following replaces per-contact consent, under the master switch.** With `load-profile-pictures` on, followed accounts show avatars without a per-contact prompt. A default install fetches nothing. Manual per-contact consent remains for accounts not followed.
- **D3, followed first-time senders stay in Message Requests, but are identifiable.** A followed sender remains quarantined and shows an avatar and "You follow this account". PD-8 is amended for the profile fetch only; there is no auto-accept.
- **D4, kind 10000 means hide; key lists get UI.** Kind 10000 pubkeys act like Block, dropping messages at admission, and existing Mute stays local notification silencing. Mute, blocked relays, DM relays, private relays, Blossom, groups, emojis and badges get real behaviour and UI. Slash commands and all 3xxxx sets get library support, sync and a generic manager only.
- **Layering.** Extend `nips/nip51`; add `nips/nip37` and `nips/nip56`; add one generic list service in `nostr-gobject`; move `GNostrMuteList` and `GhNip29GroupList` onto it.

## Scope boundaries

Not part of this work; file a bead for any that are wanted later:

- Publishing kind 3 from Groundhog (no Follow or Unfollow action).
- Drafts for NIP-29 and MLS conversations (`nostrc-2lpm` covers NIP-29).
- Executing slash commands, or intercepting them in the composer.
- Automatic filtering from kind 30007 sets.
- Replacing Gnostr's own badge, draft and report code.
- BUD-03 fallback lookups through other people's kind 10063 lists.

## Execution index

Create one bead per stage before starting it. Stage details and named tests are in §6.

| # | Goal | Done when | Key files | Depends on | Size |
|---|---|---|---|---|---|
| 1 | Lossless, signer-independent codecs and kind constants | `test_nip51`, `test_nip58`, `test_nip30`, new `test_nip37` and `test_nip56` pass; a failed encryption never produces an event | `nips/nip51`, `nips/nip37`, `nips/nip56`, `nips/nip58`, `nips/nip30`, `libnostr/include/nostr-kinds.h`, `NipOptions.cmake` | None | L |
| 2 | Shared list engine; `GNostrMuteList` becomes a facade | New list-service and facade tests pass; Gnostr `test_mute_filter` and `test_sync_bridge_mute_reload` still pass | `nostr-gobject/src/gnostr-list-*.c`, `nostr-gobject/src/gnostr-mute-list.c` | 1 | XL |
| 3 | Local list storage, editing and store-backed preferences, including the storage half of the Groundhog adapter | v11 migrates; lists edit with no signer and no network | `src/store/gh-store-social.c`, `src/app/gh-social-lists.c`, `src/app/gh-list-adapter.c`, `src/ui/gh-lists-page.c` | 2 | L |
| 4 | Private list sync; `GhNip29GroupList` on the shared service | Group restore is unchanged; nothing else publishes until the switch is on | `src/app/gh-list-adapter.c`, `src/nip29/gh-nip29-group-list.c`, `src/app/gh-auth-policy.h`, charter | 3 | L |
| 5 | Follow-gated avatars and followed Requests | Master-off startup fetches nothing; followed Requests stay quarantined with avatar and label | `src/app/gh-contact-directory.c`, `src/ui/gh-picture-cache.c`, `src/ui/gh-requests-view.c` | 4 | L |
| 6 | Hide policy across all message paths | `p` drops at admission, `word`/`t`/`e` hide reversibly, MLS state still advances | `src/app/gh-message-policy.c`, `src/app/gh-conversation-store.c`, the three store delegates, `src/mls/gh-mls-service.c` | 3 | XL |
| 7 | Relay denial and account Blossom servers | Denied relays close and stay closed at both boundaries; uploads use account servers | `src/relay/gh-relay-deny-policy.c`, `src/net/gh-relay-net.c`, `src/relay/gh-relay-guard.c`, `src/media/gh-blossom-client.c` | 3 | L |
| 8 | NIP-37 sync for NIP-17 drafts | Two service instances converge; clearing at send is atomic; no identifying outer data | `src/app/gh-draft-sync.c`, `src/store/gh-store-drafts.c`, `src/store/gh-store.c` | 4, 7 | L |
| 9 | NIP-56 reports with Hide offer | Serialized private-origin reports contain no message data | `src/app/gh-report-service.c`, `src/ui/gh-report-dialog.c` | 1, 3, 6 | M |
| 10 | Emoji and badges | No default image fetch; forged award chains rejected | `src/ui/gh-reaction-picker.c`, `src/app/gh-badge-service.c`, `src/ui/gh-badges-page.c` | 4, 9 (schema v14 only) | L |
| 11 | Release integration | `scripts/test-gate.sh` and the §6 Stage 11 targets pass; protocol-smell and HIG reviews done | `VERSION_MANIFEST.md`, packaging, `po/POTFILES` | 1–10 | M |

Paths in the table without a top-level directory are under `gnome/groundhog/`.

## 1. Summary

Implement this as a **shared protocol/list refactor followed by independently usable Groundhog features**, not as separate implementations for each list kind. Extend the portable NIP libraries with lossless, signer-independent codecs; introduce an account-scoped list document/service in `nostr-gobject`; adapt Groundhog’s existing signer, relay, and SQLCipher infrastructure to it. Preserve local storage as authoritative, make private synchronization explicitly opt-in, retain notification muting separately from author hiding, and keep followed Message Requests quarantined. Public actions—reporting, publishing badges, and publishing Blossom destinations—require their own disclosure and confirmation rather than inheriting permission from the private-sync switch.

This specification preserves D1–D4 and resolves the open questions from the planning scaffold. In §4, **candidate** marks a file whose contents were not inspected during planning.

## 2. Current-state analysis

### 2.1 Ownership and account lifecycle

- `GhAppServices` owns application services and injects backends into windows and Preferences.
- `GhAccountController` owns the active signer generation. Its asynchronous signing and NIP-44 operations already reject revoked generations, cancelled operations, and stale remote-signer gates.
- `GhAccountStore` owns the open account store and its attached conversation persistence delegate. Consumers borrow `GhStore` only while that account’s store remains open.
- `GhConversationStore` owns the loaded `GhConversation` objects and message indexes. It is main-context-only and implements `GListModel`.
- Account changes remove the old account’s conversations before notifying `account`. Backend-specific delegates are also cleared.
- SQLCipher migrations are append-only; the current schema is v10.

**Reuse:** signer-generation checks, account-store close ordering, injected UI backends, weak model references, `GhClock`, and the existing relay transports.

**Constraint:** no asynchronous callback may retain a usable borrowed `GhStore` beyond account teardown.

### 2.2 Message admission and restoration

The receive paths converge as follows:

| Backend | Authentication/transformation | Persistence boundary |
|---|---|---|
| NIP-17 | Gift wrap → authenticated seal → unsigned rumor → `GhMessage` | `GhStoreConversations::delegate_admit()` |
| NIP-04 | Signed kind 4 → signer decryption → local legacy rumor → `GhMessage` | Same NIP-17 delegate |
| NIP-29 | Signed relay event → `GhMessage` | `GhStoreNip29::delegate_admit()` |
| MLS | Signed envelope → Marmot state transition → authenticated inner event → `GhMessage` | `GhStoreMls::delegate_admit()`, inside the Marmot transaction |

`gh_conversation_store_admit()` calls the selected delegate before mutating the model. `commit.hidden` prevents insertion and `message-added`. Notification creation follows `message-added`.

Important limitations:

- A filter placed only before model insertion would still store unwanted plaintext.
- A filter placed before MLS processing would break ratchets, membership, commits, or conflict resolution.
- Restoration calls `gh_conversation_store_restore()` directly; it does not repeat admission.
- Reactions, deletions, polls, invitations, and group control messages have additional paths outside ordinary message admission.
- Existing room blocks and mutes are independent:
  - Block removes a NIP-17 room and suppresses incoming storage.
  - Mute only silences notifications using `muted_until`.
  - Request Block additionally forgets history.

### 2.3 Lists, relays, and shared-library limitations

`GhAccountRelays` already:

1. Opens an author-scoped live REQ on configured discovery relays.
2. Verifies ownership/signature.
3. Selects kind 10002/10050 revisions by greatest `created_at`, then lowest ID.
4. Emits `changed`, causing inbox and other consumers to reconcile.

`GhNip29GroupList` is the strongest synchronization precedent: bounded sources, encrypted content, removal reconciliation, stale-signature rejection, and explicit partial/unreadable states.

The existing portable NIP-51 implementation cannot safely underpin the requested features unchanged:

- `NostrListEntry` retains only one extra field.
- The parser drops `relay`, `group`, `emoji`, and `server`.
- Metadata and unsupported tags cannot round-trip losslessly.
- Private JSON serialization does not escape strings correctly; parsing is permissive and incomplete.
- Creation requires a raw secret key.
- Encryption failure currently falls back to empty content and still signs, potentially deleting private entries.
- Allocation failures can silently omit entries.

`GNostrMuteList` is an opaque process singleton, not a GObject despite its documented `changed` signal. It contains its own parser, transport selection, encryption, signing, merge logic, and indexes. Its asynchronous contexts do not provide the lifetime and account-generation boundaries required by Groundhog.

### 2.4 Avatars and Requests

Current flow:

`GhContactDirectory` → cached kind-0 picture URL → `GhPictureCache` consent → `GhWebContent` fetch → surface refresh.

Today:

- The directory’s `accepted` flag controls profile lookup and display.
- Requests are excluded.
- Picture consent persists per contact in SQLCipher.
- `GhPictureCache::is_allowed()` does not independently enforce the master switch.
- Setting-off revocation depends on conversation-view wiring.
- Fetch completion is keyed only by URI; an old completion could encounter a newly created entry for the same URI after rebinding.
- Several surfaces automatically call `allow()` for own accounts, conflating policy-derived eligibility with explicit consent.

These policies must be centralized without replacing the existing fetch/cache machinery.

### 2.5 Drafts and reporting

Drafts currently follow:

`GhComposer::draft-changed` → `GhSendUi` → `gh_store_conversations_set_draft()`.

Programmatic composer changes suppress draft emission. NIP-17 enqueue clears the draft transactionally. Delegated NIP-29/MLS composers do not currently receive equivalent draft persistence.

There is no NIP-37 transport or codec. The existing NIP-34 issue reporter is unrelated to NIP-56 and must remain separate.

### 2.6 Charter rules in play (`docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`)

- P7 account teardown (`:179`); P8 local-only signals, "Read state, pins, mutes, drafts and blocks are never published" (`:180`); P9 explain every exception (`:181`).
- PD-2 no remote fetch by default, including profile pictures and custom images (`:207`); PD-4 relay minimization, no 10002 fallback for DMs (`:209`); PD-6 per-account, per-purpose sockets (`:211`); PD-8 Message Requests, no profile fetch until accepted (`:213`); PD-11 per-contact and per-account state lives in the encrypted store, not GSettings (`:216`); PD-12 contact directory only on discovery connections, off the send path (`:217`). §2.1 sets the remote-media fetch limits.
- Kind 10009 amendment (charter `:667`): private NIP-44 list, bounded account-list relays, explicit "Restore Groups?" step.

### 2.7 Groundhog evidence by subsystem (paths relative to `gnome/groundhog/`)

**Avatars and Message Requests**
- `GhContactDirectory` tracks `accepted` per pubkey and admits kind 0 only for accepted contacts (`src/app/gh-contact-directory.c:38-53, 882-899`); `gh_contact_directory_dup_picture_uri()` returns a URL only for accepted contacts (`:1295-1337`). Rescan accepts peers of non-request NIP-17 DMs and direct MLS DMs (`:1102-1209`). Discovery runs 2-30 minutes after store bind, refreshes stale entries after 24 h in randomized batches of at most 10, spaced 10-120 s; a newly accepted request refreshes after 5-60 s (`:14-27, 516-560, 582-655`). An unaccepted sender's lookup asks kind 10050 only (`:1410-1416`).
- `GhPictureCache` (`src/ui/gh-picture-cache.c`) owns consent and image bytes. `gh_picture_cache_is_allowed()` only checks the `allowed` table (`:64-71`); it does not check the master setting, because `revoke_all` empties that table when the setting turns off (`:85-113`; trigger at `src/ui/gh-conversation-view.c:2749-2762`). `gh_picture_cache_get()` fetches only for allowed pubkeys (`:147-175`). Consent persists via `GhPictureConsentBackend` to the store contacts table, schema v9 (`src/app/gh-app-services.c:993-1025`). Each `GhConversationList` makes one cache shared by that window's surfaces (`src/ui/gh-conversation-list.c:675-690`).
- `load-profile-pictures` defaults false (`data/org.nostr.Groundhog.gschema.xml:65-68`); `GH_WEB_PICTURE` maps to it (`src/ui/gh-web-content.c:17-22`). Its description and the Preferences subtitle (`data/ui/gh-preferences-dialog.blp:74-77`) still claim an allow lasts only until leaving the conversation, which no longer matches the persistent per-contact consent.
- Avatar surfaces: conversation rows (`src/ui/gh-conversation-row.c:284-330`, picture branch checks directness and consent, not `is_request`); message rows and manual "Load Profile Picture" (`src/ui/gh-message-row.c:330-359, 1748-1787`; `src/ui/gh-conversation-view.c:2383-2477, 2641-2659`); Conversation Info person rows (`src/ui/gh-conversation-info-dialog.c:98-122, 658-678`; `src/app/gh-app-services.c:1176-1194`); New Message picker (`src/ui/gh-new-message-page.c:579-597, 1651-1683`); account switcher (`src/app/gh-account-ui.c:138-153`). Text-only today: Requests view (`src/ui/gh-requests-view.c:99-126`, `data/ui/gh-requests-view.blp:53-62`), group and MLS member rows (`src/ui/gh-group-info-dialog.c:80-83`, `src/ui/gh-mls-group-info-dialog.c:84-122`), Add Contact. `gh_conversation_view_dup_picture_uri()` refuses request conversations (`src/ui/gh-conversation-view.c:2577-2583`); preview and auto-load also return early for requests (`:2388-2395, 2701-2710`).
- Groundhog has no kind 3 read path. Tests: `tests/app/test_contact_directory.c` (`pt8-requests`, `nt12-startup`, `own-profile`), `tests/ui/test_conversation_view.c` (`web-allow-covers-sender`; the generic `web-consent` loop omits `GH_WEB_PICTURE`, `:6130-6133`), `tests/app/test_account_ui.c`, `tests/ui/test_conversation_list.c`, `tests/ui/test_requests.c`, `tests/ui/test_conversation_info.c`. There is no dedicated picture-cache test target and no end-to-end test of fetch + persistent consent + setting-off revocation.

**Drafts and signer**
- `GhComposer` emits `draft-changed` after a 1 s debounce; `gh_composer_set_text()` suppresses it (`src/ui/gh-composer.c:204-215`). `GhSendUi` writes through `gh_store_conversations_set_draft` and flushes on switch (`src/app/gh-send-ui.c:388-422, 465-478`). `delegated()` means a `GhSendUiDelegate` (NIP-29, MLS) handles the conversation, so those have no drafts (`:49-59`; `src/ui/gh-group-ui.c:259-264`; `src/ui/gh-mls-ui.c:346-350`).
- Store draft API accepts only canonical NIP-17 room keys (sorted lowercase pubkeys including the account); empty clears (`src/store/gh-store-conversations.c:120-139, 950-994`). `gh_store_enqueue()` clears the draft in the enqueue transaction for chat messages, not for kind 5 or 7 (`src/store/gh-store.c:3459-3472`). Room IDs: NIP-17 sorted comma-joined pubkeys; NIP-29 `relay_url\x1fgroup_id`; MLS `mls:<hex>` (`src/app/gh-message.c:160-178, 559-584, 764-781`). Open bead `nostrc-2lpm`: drafts for NIP-29 rooms.
- `GhAccountController` exposes async sign and NIP-44 with generation and cancellation guards; signer-unavailable returns `GH_SIGNER_ERROR_UNAVAILABLE` (`src/app/gh-account-controller.c:1232-1423`; gate at `:1308-1347, 1472-1548`). `GhSigner` dispatches NIP-55L or NIP-46 (`src/identity/gh-signer.c:349-418`). `GhSignerContext` carries reason, purpose and flow, and reaches NIP-55L but not NIP-46 (`src/identity/gh-signer.h:48-58`). NIP-46: 16 in flight, `sign_event` interactive, `nip44_encrypt` bulk, 330 s request timer, `auth_url` up to 600 s (`src/identity/gh-nip46-session.c:15-35, 591-678, 987-1014, 1352-1436`).
- Gnostr has two unrelated draft helpers; neither encrypts or publishes: `apps/gnostr/src/util/gnostr-drafts.c` (plaintext local JSON) and `apps/gnostr/src/util/nip37_drafts.c` (tag parser).

**Mute, block, admission**
- Three conversation backends (`src/app/gh-conversation.h:10-15`) converge on `gh_conversation_store_admit()`; each delegate owns persistence. Only `commit.hidden` skips the model and `message-added` (`src/app/gh-conversation-store.c:151-178, 287-337`, check at `:321`). Restore paths call `gh_conversation_store_restore()` directly and do not re-run admission.
- NIP-17: sender known only after unwrap (`src/app/gh-dm-inbox.c:420-450, 563-609`); blocked rooms record wrap and rumor IDs as seen and store nothing (`src/store/gh-store-conversations.c:335-343, 386-393`). NIP-04 legacy inbox joins the same path (`src/app/gh-nip04-inbox.c:86-203`). NIP-29: `src/nip29/gh-nip29-service.c:1150-1184`, delegate `src/store/gh-store-nip29.c:488-594`, reactions and deletions handled separately (`:884-900, 1211-1217` in the service). MLS: envelope processed through Marmot, then sender match, then admit (`src/mls/gh-mls-service.c:2700-2819`; delegate `src/store/gh-store-mls.c:223-335`).
- Block and Mute are per room. Mute: `src/app/gh-conversation-actions.c:10-21`, `muted_until`. Block surfaces: `info.block` in Conversation Info (`src/ui/gh-conversation-info-dialog.c:318-340, 442-465, 840-849`), `requests.block` block-and-forget (`src/ui/gh-requests-view.c:229-280`), Blocked page (`data/ui/gh-blocked-page.blp`, `src/ui/gh-blocked-page.c`, `src/app/gh-blocked-conversations.c`), menu actions (`data/ui/gh-content-page.blp:195-200`, `src/ui/gh-conversation-menu.c:443-483`, `data/ui/gh-conversation-row.blp:135-184`). Notifier listens to `message-added` and checks NIP-17-only room state (`src/app/gh-notifier.c:248-257, 658-681`). Mute and block already withdraw shown notifications (`src/app/gh-conversation-actions.c:4-8, 18-19, 31-32`), so open bead `nostrc-98se` may be stale and should be re-checked.
- Report action seams: `gh_message_get_sender()` (`src/app/gh-message.h:37-42`), message row menu (`src/ui/gh-message-row.c:1517-1578`), Conversation Info person rows (`src/ui/gh-conversation-info-dialog.c:116-126, 638, 650-665`), group and MLS member rows. Existing "Report an Issue" is NIP-34 (`src/app/gh-app-services.c:1924-1940`), a separate feature. Dialog with secondary choice pattern: `Adw.AlertDialog.extra-child` (`data/ui/gh-conversation-view.blp:204-226`).

**Account-owned lists and relays**
- `GhAccountRelays` holds kinds 10002 and 10050 in memory via a live REQ on `discovery-relays`, newest by `created_at` then lower ID (`src/app/gh-account-relays.c:110-159, 202-254`). `GhDmInbox` rebuilds on 10050 change (`src/app/gh-dm-inbox.c:1076-1106, 1236-1324`). Publish: `GhInboxSetup` (`src/app/gh-inbox-setup.c:244-281, 929-955`) and `GhRelayListSetup` via Preferences add-relay and remove-relay (`src/ui/gh-preferences-dialog.c:1036-1066, 1777`; `src/app/gh-app-services.c:1484-1640`; `src/app/gh-relay-list-setup.c:214-238, 361-397`).
- `GhNip29GroupList` (kind 10009, final GObject, one per store) is the closest template: reads from union of discovery and 10002 write relays (max 16), merges newest remote with local removal timestamps, fails closed on unreadable private content, publishes only when changed, discards a signature if a newer list arrives, states inactive/waiting/reading/partial/unreadable/synced/publishing/failed (`src/nip29/gh-nip29-group-list.h:10-114`; `.c:235-344, 447-464, 559-620, 787-818, 839-1004`). Built in `src/app/gh-app-outbox.c:151-191`; consumers `src/app/gh-app-services.c:2164-2172, 2538-2548`, `src/ui/gh-group-ui.c:117-213`, restore dialog `src/ui/gh-group-restore-dialog.c:61-94`; tests `tests/nip29/test_nip29_group_list.c`, `tests/ui/test_group_ui.c`. There is no user opt-in for kind 10009: it publishes automatically. The onboarding `relay_list_switch` (`data/ui/gh-onboarding-view.blp:664-680`) concerns kind 10002 for KeyPackage discovery. Open bead `nostrc-1b4j`: shared kind-10009 parser and builder.
- Auth policy (`src/app/gh-auth-policy.h:16-45, 68-71`): `OWN_LIST_DISCOVERY` ephemeral, `OWN_INBOX_READ` and `OWN_LIST_PUBLISH` account, `CONTACT_DIRECTORY` ephemeral. Rule R1: the account signs AUTH only for `OWN_INBOX_READ`, `OWN_LIST_PUBLISH`, `SELF_WRAP` and `GROUP`. Reading NIP-37 drafts from an AUTH-gated private relay needs a new account-auth purpose and an R1 amendment.
- Teardown: generation bump and cancel (`src/app/gh-account-controller.c:129-148, 465-487`); store close ordering (`src/app/gh-account-store.c:232-250, 639-660`).
- Store: SQLCipher, append-only migrations in `src/store/gh-store-schema.c:1-16, 119-139, 429-442`; `GH_STORE_SCHEMA_VERSION` is 10 (`src/store/gh-store.h:823`).
- Preferences: pages Privacy, Messages, Network, Account, AI Agents (`data/ui/gh-preferences-dialog.blp:16, 167, 326, 560, 662`). New GSettings keys must be added to the `GSETTINGS` allowlist in `tests/check_privacy.py:217` with a charter §7.11 row. There is no generic store-backed per-account preference binding; account UI is injected through callbacks (`src/ui/gh-preferences-dialog.h:10-49, 130-148`; `src/app/gh-app-services.c:1801-1880`).
- Relay admission boundaries: `gh_net_relay_url_allowed()` in `GhRelayNet::open_inner()` with Tor support (`src/net/gh-relay-net.c:80-108`, installed `src/app/gh-app-services.c:310`); `GhRelayGuard` without (`src/relay/gh-relay-guard.c:73, 288, 350`, installed `src/app/gh-app-services.c:355`). No kind 10006 check exists.
- Blossom: global ordered `blossom-servers` GSettings key (`data/org.nostr.Groundhog.gschema.xml:173`), UI at `data/ui/gh-preferences-dialog.blp:235` and `src/ui/gh-preferences-dialog.c:1642-1647`; uploads `src/media/gh-blossom-client.c:496, 547`. No kind 10063 in Groundhog. Note the tension with D1 and PD-11 for an account-scoped list.
- Emoji: no NIP-30 rendering; reactions show shortcode text, "not fetched (PD-2)" (`src/app/gh-reaction.h:20`). `GhReactionPicker` (`src/ui/gh-reaction-picker.c:57-76`), composer `GtkEmojiChooser` (`src/ui/gh-composer.c:550-569`), mention autocomplete (`:247, 279, 384-402`).
- Contact details for badges: `GhConversationInfoDialog` and `GhConversationInfoProfile` (`src/ui/gh-conversation-info-dialog.h:54-78`, `data/ui/gh-conversation-info-person.blp`).

### 2.8 Library surface evidence

- `nips/nip51`: `NostrList` and `NostrListEntry { tag_name, value, extra, is_private }` with one `extra` (`include/nostr/nip51/nip51.h:34-57`). `parse_list` keeps only `p e t word a r` and does not check kind (`src/nip51.c:478-572`), so `relay`, `group`, `emoji`, `server` are dropped. Private items use NIP-44 (`src/nip51.c:290-351`). Signs directly from `sk_hex` (`:390-457`), unusable with `GhSigner`. Object library, no install export (`nips/nip51/CMakeLists.txt`); `VERSION_MANIFEST.md:550` Unversioned. Only consumer: `apps/gnostr/src/model/gnostr-nip51-loader.c`. NIP-51 forces NIP-44 on in `NipOptions.cmake:314-322`.
- `nips/nip02`: `NostrFollowEntry`, `NostrFollowList`, parse, build, append (`include/nostr/nip02/nip02.h:13-72`). `nips/nip30`: parse and helpers, unused by Gnostr's parallel code (`include/nostr/nip30/nip30.h:16-125`).
- `nips/nip58`: definition 30009, award 8, profile badges as kind 30008 only (`include/nostr/nip58/nip58.h:17-26, 93-202`). `libnostr/include/nostr-kinds.h:147` names 30008 `NOSTR_KIND_PROFILE_BADGES`; the current spec calls that kind Badge Sets.
- `nips/nip37`, `nips/nip56` do not exist. `libnostr/include/nostr-kinds.h` lacks 10008, 10013, 10992, 31992, 39089, 39092.
- `nostr-gobject`: `GNostrMuteList` is a process-wide opaque singleton, not a GObject, with no real `changed` signal, and does not call `nostr_nip51_*` (`include/nostr-gobject-1.0/gnostr-mute-list.h:18-38, 410-418`; `src/gnostr-mute-list.c:73-136, 188-221`). All runtime callers are in `apps/gnostr` (`gn-nostr-event-model.c:594-631`, `gnostr-timeline-source.c:351`, `util/mute_filter.c:14-22`, `notifications/badge_manager.c`, `sync/gnostr-sync-bridge-mute.c:25-29`, `ui/gnostr-main-window-pool.c:446-448`, `util/gnostr-app-data-manager.c:698-699`, `ui/gnostr-main-window-settings.c:1400-1633`, `ui/gnostr-mute-list.c:142-424`, `ui/gnostr-main-window-profile-pane.c:79-80`, `ui/gnostr-main-window-utilities.c:42-71`, tests `test_mute_filter.c`, `test_sync_bridge_mute_reload.c`). Signing and publishing go through injected bridges (`src/gnostr-signer-bridge.c:33-96`; `apps/gnostr/src/ipc/gnostr-signer-bridge-install.c:163-191`). `gnostr-relays` parses 10002 and 10050 (`gnostr-relays.h:13-209`). `GNostrRelayStore` is a generic publish and query interface (`nostr_relay_store.h:9-39`). GObject wrapper pattern: `GNostrNip19`, `GNostrNip49`. `nostr_gobject` links neither nip51, nip02, nip30 nor nip58 (`nostr-gobject/CMakeLists.txt:272-279`).
- Groundhog links `nostr_gobject`, `nostr`, and NIP-17/59 cores (`gnome/groundhog/CMakeLists.txt:300-304, 320-330`); it does not use `nostr_nip51_*`. Groundhog has its own relay stack (`GhRelayScope`, `GhRelayPublish`, `GhAuthPolicy`) and signer (`GhAccountController`), so any shared list service must not assume the Gnostr bridges.

### 2.9 Protocol facts (checked against nostr-protocol/nips master, 2026-11-08)

- **Kind table (NIP-51):** 10000 `p t word e`; 10006 `relay`; 10008 `a` (30009), `e` (8), `a` (30008); 10009 `group` (id, relay, optional name), `r`; 10013 `relay`; 10030 `emoji`, `a` (30030); 10050 `relay`; 10063 `server`; 10992 `a` (31992).
- **Set and definition equivalents:** 30000 follow groups `p`; 30002 relay groups `relay`; 30003 bookmarks; 30004 article curation; 30005 video; 30006 picture; 30007 kind-mute sets `p` with `d` equal to the kind string; 30008 badge groups `a` and `e` pairs; 30015 interests `t`; 30030 emoji groups; 30063 release artifacts; 30267 app curation; 31924 calendar; 39089 and 39092 starter packs. Sets carry `d`, and optional `title`, `image`, `description`. 31992 is a slash command definition (NIP-CD), not a set. Kinds 10009 and 10063 have no set equivalent in the table. Deprecated: 30000 `d=mute`; 30001 `d=pin`, `bookmark`, `communities`.
- **Private items:** NIP-44 encrypted, JSON-stringified tag array in `.content`, encrypted to self. NIP-04 legacy detected by searching for `"iv"` in the ciphertext. Append new items at the end.
- **NIP-37:** kind 31234; content is a JSON-stringified unsigned draft event, NIP-44 encrypted to the signer; `d` identifies the draft; `k` required, draft event's kind; `expiration` recommended; blank `.content` means deleted; kind 1234 checkpoints use `a`. SHOULD publish to relays in kind 10013, whose public tags are empty and whose content is NIP-44 encrypted private `relay` tags. 10013 MUST be published to NIP-65 write relays. Private relays recommended to use NIP-42 and serve only the authenticated user.
- **NIP-56:** kind 1984; MUST `p` tag; MUST `e` tag when reporting a note; report type in third tag element: `nudity malware profanity illegal spam impersonation other`; blob `x` tag with an `e`; `content` optional. NIP-17 conceals participants and timing from the public, and neither NIP specifies proof for a private DM.
- **NIP-58:** 30009 definition (`d`, `image` with optional dimensions, `thumb`); 8 award (`a`, `p`, immutable); 10008 profile badges as ordered consecutive `a`/`e` pairs, unmatched ignored, award must contain the same `a`; 30008 badge sets. Deprecated 30008 `d=profile_badges` SHOULD be treated as equivalent to 10008 and migrated.
- **NIP-02:** kind 3, `["p", hex, relay?, petname?]`, each list replaces the previous one, content unused.
- **NIP-30:** `["emoji", shortcode, url, optional set address]`; shortcode alphanumerics, hyphens, underscores; enumerated kinds are 0, 1, 1111, 7, 30315, so kind 14 DM use is an extension to confirm.
- **NIP-CD:** kind 31992 tags `d command title description arg s`; `arg` is `[arg, name, type, required|optional, label, choice...]`; 10992 is a list of `a` tags; invocations are plain text at index 0 with no tags; "A client MUST NOT require its own support to send an invocation, and MUST send content that fails to parse as ordinary text."
- **NIP-B7 and BUD-03:** kind 10063 with ordered `server` tags, empty content.

### 2.10 Repository rules

- Use Beads for tracking. Relay code must stay event-driven with `REQ`, `EOSE`, `OK` and `AUTH` handling, no polling or sleep; run the `nostr-protocol-smells` skill before merging protocol changes. Follow the AGENTS.md banned-relay rule in code, docs and tests. Fix Groundhog leaks; do not add suppressions.
- Verification: `scripts/test-gate.sh`; direct CTest names `test_nip51`, `test_nip58`; Groundhog tests are registered through `gnome/groundhog/TestInventory.cmake`.
- Versions: Groundhog declared 0.12.1 (`VERSION_MANIFEST.md:30`), nostr-gobject 3.0.1 (`:25`), libnostr 2.3.0 (`:22`), nip51 and nip58 Unversioned (`:550, 556`). UI work follows the `gnome-hig` skill.

### 2.11 Prior art

- Plan shape follows `docs/plans/groundhog-alpha6-2026-10-09.md`.
- Gnostr commits: `0ab7d9700` (NIP-51 library), `3a359b621` (mute-list sync merge strategy), `50a7957c9` (NIP-37), `6024d0404` (reports), `e02ad3f5e` and `d5bf51bac` (NIP-58), `53aa138e5` (follow and unfollow).
- Open related beads: `nostrc-98se`, `nostrc-2lpm`, `nostrc-1b4j`, `nostrc-lyvz`. No open bead for NIP-56 or NIP-58.

## 3. Design

### 3.1 Resolved product and privacy policies

#### Synchronization scope

The per-account **Sync Drafts and Lists** switch is stored in SQLCipher and defaults to `false`.

| Data | Automatic behavior with switch off | Behavior with switch on |
|---|---|---|
| NIP-17 drafts | Local only | Read/write encrypted synchronization |
| 10000, 10006, 10013, 10030, 10992 | Local edits only | Synchronize supported private entries |
| Supported 3xxxx list sets | Local manager | Synchronize private entries; public publication remains separately confirmed |
| Kind 3 follows | Read own follows only while `load-profile-pictures` is on | Unchanged; Groundhog never publishes kind 3 |
| 10002 and 10050 | Existing discovery/setup behavior | Unchanged; existing explicit setup remains the writer |
| 10009 simple groups | Existing automatic NIP-44 publication (charter amendment 2026-10-11) | Unchanged; not gated by this switch |
| 10008 profile badges | Cached display or explicit lookup | Public publication still requires explicit confirmation |
| 10063 Blossom servers | Local account destinations | Public publication still requires explicit confirmation |
| Reports | No automatic publication | Unchanged; each report is explicitly approved |

Additional rules:

- Enabling sync does not publish existing public entries, reveal private entries, or publish 10008/10063.
- Enabling sync starts a one-time reconciliation of existing NIP-17 drafts, with no publication until import has finished (§3.7).
- The first time sync imports an existing remote kind 10000, Groundhog shows how many accounts and items it will hide and asks before applying it, because `p` entries permanently drop new messages. Declining leaves sync on for other lists and marks kind 10000 unsynced.
- Newly created Groundhog entries use private visibility where the kind permits it.
- Imported public entries retain their visibility. Changing it requires an explicit disclosure.
- Disabling sync cancels pending synchronization, including unstarted signed publications; local changes remain saved.
- Events already accepted by a relay cannot be recalled.
- The switch is never itself synchronized.
- Kind 10009 keeps its existing automatic private publication. It was authorized by the 2026-10-11 charter amendment and exposes nothing new; gating it would break group restore for existing users. See Open Question 1.

**Rationale:** an encryption opt-in cannot authorize public publication.

#### Hide versus notification mute

Use these user-facing concepts consistently:

- **Mute Notifications:** existing local per-conversation `muted_until`.
- **Hide Account:** kind-10000 `p` entry; affects incoming messages across backends.
- **Block Conversation:** existing room-local behavior.
- **Hidden Content:** kind-10000 `word`, `t`, and `e` entries.

Do not migrate room blocks into author entries or change the meaning of existing Block actions.

#### Already-stored history

Adding a hide entry:

- Stops storage of subsequent incoming application messages from a hidden `p` account. Messages matching `word`, `t` or `e` entries are stored but marked hidden (§3.5).
- Hides matching existing history without deleting it.
- Recomputes visible previews, unread counts, and conversation activity.
- Withdraws affected notifications.
- Does not move read markers.

Removing an entry reveals retained history. It does not resurrect messages discarded at admission, which applies only to `p` entries; `word`, `t` and `e` hides are fully reversible.

#### Private report policy

A report initiated from NIP-17, NIP-04, MLS, or a private/unknown-visibility NIP-29 group is an **account report**, not a private-message report.

It contains:

- The reporting account’s public key.
- A typed `p` tag naming the reported account.
- The selected report category.
- Optional user-written explanation.

It never contains private message text, rumor IDs, envelope IDs, group IDs, participants, attachments, transcripts, or automatically copied evidence.

#### Other Open Questions

- **Blossom:** account-scoped SQLCipher destinations replace runtime use of global `blossom-servers`.
- **Mute facade:** retain the existing `GNostrMuteList` ABI as a compatibility facade over the shared implementation.
- **Draft cadence:** local persistence remains immediate/debounced; network publication is coalesced and signer-aware, specified in §3.7.
- **Draft identifiers:** a random `d` per room, with the room identified only inside the ciphertext; never a public hash of participants, and no Groundhog-specific kind or tag (§3.7).
- **NIP-30 in private messages:** Groundhog sends standard `emoji` tags with kind 7 reactions and with chat messages, and renders received custom emoji only after PD-2 consent (§3.11). See Open Question 2.

---

### 3.2 Portable protocol libraries

#### Lossless NIP-51 representation

Add opaque, additive types to `nips/nip51`; do not enlarge or reinterpret the existing public structs.

| Type | Kind and owned state |
|---|---|
| `NostrListDocument` | Opaque portable document owning kind, identifier, ordered public/private tag vectors, metadata, and preserved extension tags |
| `NostrListTag` | Opaque tag owning every string field, including empty positional fields |
| `NostrListSchema` | Read-only registry descriptor: kind, identifier requirements, supported item forms, metadata, privacy rules |
| `NostrListPrivateState` | `EMPTY`, `ENCRYPTED_NIP44`, `LEGACY_NIP04`, `DECODED`, `UNREADABLE` |

The document preserves:

- Tag order.
- Full tag arity.
- Unknown tags and trailing fields.
- Public/private placement.
- Distinction between absent and empty positional values.
- Repeated entries and ordered pairs where meaningful.

Typed accessors validate known forms without deleting unknown extensions. Editing one known item must not rebuild the document from only recognized items.

Illustrative interfaces:

```text
parse_event(event) → document or error
parse_private_tags(plaintext, length) → ordered tags or error
serialize_private_tags(document) → owned JSON or error
build_unsigned(document, author, created_at, encrypted_content) → event or error
validate_item(schema, tag) → valid / unsupported-extension / invalid
```

These operations are synchronous and perform no network access, signing, or cryptography.

Existing raw-key functions remain compatibility wrappers over the new codecs. They must fail entirely on encryption, serialization, validation, or allocation failure; never publish a partial replacement.

Use the repository JSON implementation, not hand-built JSON or a new parser.

#### Exact supported schema registry

“Private” below means NIP-44-to-self content, not encryption of required outer routing metadata.

| Kind | Known item forms | Required/special metadata | Groundhog behavior |
|---|---|---|---|
| 3 | `p` with optional relay and petname | No `d`; read only | Avatar gate (§3.6) |
| 10002 | `r` with optional marker | No `d`; read only here | Existing `GhAccountRelays` editor |
| 10000 | `p`, `t`, `word`, `e` | No `d` | Hide policy |
| 10006 | `relay` | No `d` | Relay denial |
| 10008 | Consecutive definition `a` / award `e`; badge-set `a` references | No `d` | Profile-badge manager |
| 10009 | `group,id,relay[,name]`; `r,relay` | No set equivalent | Existing group restore/membership projection |
| 10013 | Private `relay` tags | Empty public item tags | Private draft relay destinations |
| 10030 | `emoji,shortcode,url[,set-address]`; `a` to 30030 | Ordered favorites | Emoji picker |
| 10050 | `relay` | Public; existing NIP-17 contract | Existing inbox setup |
| 10063 | Ordered `server` tags | Public; empty content; no set equivalent | Blossom destinations |
| 10992 | `a` to 31992 | No `d` | Slash-command subscriptions only |
| 30000 | `p` | `d` | Follow-group manager |
| 30002 | `relay` | `d` | Relay-group manager |
| 30003 | Bookmark references: `e`, `a`, `r`, `t` | `d` | Generic manager |
| 30004 | Article-curation references | `d` | Generic manager |
| 30005 | Video-curation references | `d` | Generic manager |
| 30006 | Picture-curation references | `d` | Generic manager |
| 30007 | `p` | `d` is canonical decimal target kind | Generic manager; no automatic filtering |
| 30008 | Consecutive `a` / `e` badge pairs | `d` | Badge sets |
| 30015 | `t` | `d` | Generic manager |
| 30030 | `emoji` | `d` | Emoji sets |
| 30063 | Release-artifact references | `d` | Generic manager |
| 30267 | Application references | `d` | Generic manager |
| 31924 | Calendar-event `a` references | `d` | Generic manager |
| 39089, 39092 | Starter-pack entries per the supplied NIP-51 table | `d` | Generic manager |
| 31992 | `command`, `title`, `description`, `arg`, `s` | `d`; `arg` retains all choices | Definition editor; never executable |

All set kinds support optional `title`, `image`, and `description`. Preserve additional tags even when they have no dedicated editor.

Also retain existing library support for 10001, 10003 and deprecated 30001 forms. Recognize 30000 `d=mute` and 30001 `d=pin|bookmark|communities` as deprecated documents; never silently reinterpret their address.

For curation and starter-pack schemas, transcribe the pinned protocol table into registry fixtures before implementation; the supplied plan does not enumerate every subtype field. Unknown entries remain lossless and editable as raw tag arrays, so incomplete typed accessors cannot destroy data.

#### Bounds

Apply at the service boundary:

- Signed list event: at most 128 KiB.
- Kind 3 only, because follow lists are routinely larger: at most 1 MiB, matching the transport frame limit (`src/net/gh-relay-soup.c:12`), and 16,384 tags. If a follow list still exceeds this, keep the last usable set and show **Follow List Too Large** in Preferences.
- Private plaintext: at most 60,000 bytes and within the NIP-44 limit.
- At most 4,096 tags per document and 32 fields per tag.
- Individual tag field: at most 16 KiB.
- Identifier: at most 256 UTF-8 bytes.
- Never truncate an editable document; reject an oversized edit.

Oversized remote revisions are reported as unreadable/unsupported, retained only as a bounded revision record, and never overwritten automatically.

#### NIP-37

Add `nips/nip37`:

- Parse/build unsigned kind-31234 envelopes.
- Require `d` and canonical decimal `k`.
- Treat blank outer content as deletion.
- Parse expiration.
- Parse/build decrypted unsigned draft events without signing them.
- Add kind-1234 checkpoint address parsing/building.
- Reject signed inner draft events.
- Keep encryption and signer access outside the module.

#### NIP-56

Add `nips/nip56`:

- Closed report-category enum: `NUDITY`, `MALWARE`, `PROFANITY`, `ILLEGAL`, `SPAM`, `IMPERSONATION`, `OTHER`.
- Parse/build unsigned kind 1984.
- Require a valid typed `p`.
- Require typed `e` when the caller declares a note report.
- Support `x` plus its associated `e` in the library, but not private-message evidence in Groundhog.
- Validate IDs, public keys, category spellings, and tag relationships.

#### NIP-58 and constants

Do not redefine existing exported names from 30008 to 10008.

- Keep `NOSTR_KIND_PROFILE_BADGES` and the old NIP-58 constant as deprecated 30008 compatibility names.
- Add unambiguous constants for canonical profile-badge list 10008 and badge sets 30008.
- Existing profile-badge builders keep producing the documented legacy form.
- Add canonical 10008 parse/build APIs.
- Add badge-set parse/build APIs.
- Add validation of consecutive pairs and award/definition/recipient relationships.
- Resolve definition addresses by splitting only the first two `:` separators.

Add missing draft, list, set, starter-pack, and slash-command constants to `nostr-kinds.h`. Use those constants in new code.

---

### 3.3 Shared GObject document and service

#### Components

Add to `nostr-gobject`:

| Component | Responsibility |
|---|---|
| `GNostrListDocument` | Immutable GObject snapshot wrapping the portable document and verified revision |
| `GNostrListService` | Account-scoped synchronization engine and document `GListModel` |
| `GNostrListAdapter` | Copied callback table providing storage, subscriptions, crypto, signing, and publication |
| `GNostrListOperation` | Immutable edit description, used to rebase local changes onto a newer remote document |

Document identity is `(author, kind, identifier)`, with `identifier=""` for non-addressable lists.

Snapshot fields include:

- Identity.
- Signed source JSON and event ID, when available.
- `created_at`.
- Ordered decoded items.
- Private-content state.
- Local revision number.
- Dirty flag.
- Last error classification.
- Per-source read and per-target publish outcomes.

The service owns adapter data through one destroy callback. It never imports `Gh*`, assumes Gnostr bridges, opens a global pool, or selects fallback relays.

#### Adapter contract

Use one adapter table rather than several single-purpose interface hierarchies.

| Adapter operation | Contract |
|---|---|
| Load/commit local state | Synchronous on owner context; commit is atomic and can fail |
| Open/update/close subscription | Author/kind/`#d` filters, supplied destinations and purpose; event-driven callbacks |
| Encrypt/decrypt | Standard async/finish; self recipient fixed by service account |
| Sign | Standard async/finish; unsigned JSON in, verified signed JSON out |
| Publish | Signed immutable JSON, explicit target list; individual `OK`/failure outcomes |
| Clock/randomness | Injected for deterministic tests |
| Generation validity | Checked before every external effect and completion |

Subscription notifications distinguish:

`EVENT`, `EOSE`, `CLOSED`, `AUTH_REQUIRED`, `ERROR`, `DISCONNECTED`, `RECONNECTED`.

Publication distinguishes accepted, duplicate, rejected, authentication failure, transport failure, and cancelled. A socket write alone is never acceptance.

New callable surface:

```text
new(account, generation, adapter, data, destroy)
start(read_plan)
update_read_plan(read_plan)
apply(identity, expected_local_revision, operations) → saved snapshot or error
publish_async(identity, approved_visibility, cancellable, callback)
publish_finish(result) → per-target result or error
set_private_sync_enabled(enabled)
retry(identity)
close()
```

`apply()` saves locally before emitting `document-changed`. It does not imply publication permission. Each read-plan entry carries a publish policy: `AUTO_WHEN_SYNC_ENABLED`, `AUTO_ALWAYS` (kind 10009 only), `EXPLICIT_ONLY` (10008, 10063, sets published publicly) or `READ_ONLY` (3, 10002, 10050), which the service refuses to publish.

#### State machine

The public service-state enum is:

- `INACTIVE`
- `LOCAL_ONLY`
- `WAITING_FOR_SIGNER`
- `READING`
- `PARTIAL`
- `UNREADABLE`
- `DIRTY`
- `PUBLISHING`
- `SYNCED`
- `FAILED`

State precedence: closed/inactive → unreadable → publishing → reading/partial → waiting → failed → dirty/local-only/synced.

Keep detailed source/target outcomes separately; one enum must not conceal partial acceptance.

#### Revision and merge algorithm

1. Verify signature, author, kind, address, size, and future-skew allowance.
2. Select remote revisions using NIP-01 ordering: greatest timestamp, then lowest event ID.
3. Ignore exact duplicate revisions.
4. Decode private content before replacing the usable private snapshot.
5. Persist the verified base and decoded state.
6. Rebase pending local operations in original order:
   - Add appends.
   - Remove removes matching entries, with a durable removal record.
   - Move identifies an occurrence, not merely a value.
   - Metadata edits replace the selected metadata field.
7. Preserve unknown tags.
8. Capture base ID, local revision, visibility approval, and generation before encryption/signing.
9. If any changes while signing, discard the signature and rebase; do not publish it.
10. Persist the exact signed event and approved targets before sending.
11. Clear dirty state only for the revision actually accepted.

Do not union successive remote replaceable snapshots: absence in the newer snapshot represents removal. Local removal records prevent pending reconciliation from reintroducing locally removed entries.

If encrypted content cannot be read:

- Retain the last usable local projection.
- Show `UNREADABLE`.
- Do not publish a replacement.
- Retry only on signer recovery, explicit Retry, or a newer revision.
- Detect legacy NIP-04 content using the supplied `"iv"` convention. New writes always use NIP-44.

EOSE is not proof that unavailable relays held no newer revision. Each read-plan entry sets `required_sources`. Kind 10009 keeps its existing rule of all sources. Other documents publish automatically once a majority of sources, and at least one, have answered with EOSE, so one dead relay cannot stop sync indefinitely. Below that threshold the document stays `PARTIAL`, and publishing needs a separate **Publish Local Version Anyway…** confirmation.

#### Restart and resume

At every store bind, scan `social_publications` and `draft_publications`. Before sending a stored signed event, re-validate it under the current generation: same account, sync still enabled, event still the newest for its address, and targets still allowed. Discard anything stale and re-sign from current state. Disabling sync deletes unsent events and keeps tombstones.

#### Concurrency and shutdown

- All service mutation and GObject signals run on its captured thread-default main context.
- Worker/transport callbacks marshal onto that context.
- Calls capture account, generation, operation serial, and base revision.
- `close()` cancels subscriptions, crypto, signer requests, timers, and publications before releasing adapter storage.
- Cancellation never marks dirty data synchronized.
- The list service bounds concurrent NIP-44 work to 16 operations and queues the rest, because the NIP-46 bulk queue refuses work beyond 256 (`src/identity/gh-nip46-session.c:16, 1405`).
- Retry uses bounded backoff only after an actual failure; there is no periodic relay polling.

#### Compatibility migrations

**`GhNip29GroupList`:**

- Keep its public API, state names, restore dialog behavior, and membership projection.
- Replace its parser, revision tracking, encryption/sign/publish pipeline with the shared service.
- Retain application-specific join/removal reconciliation and explicit Restore Groups. Add a pre-publish `derive_operations` adapter callback for it, because this list is derived from room state and removals compare against the list timestamp (`src/nip29/gh-nip29-group-list.h:35-42`).
- Use `update_read_plan()` for relay and settings changes (`gh-nip29-group-list.c:962-998`), and a wait-for-dependency state until own-list discovery settles.
- Construction order: `GhSocialLists` binds first; `GhAppOutbox` receives its service when it builds the group list.
- Remove its independent publication implementation.

**`GNostrMuteList`:**

- Retain exported functions, callback shapes, merge-strategy enum, and singleton entry point.
- Internally bind an account-scoped shared service using Gnostr adapters.
- Keep mutex-protected immutable lookup snapshots for its documented synchronous/thread-safe queries.
- Serialize mutation snapshots under that mutex; marshal persistence and network work to the owning context.
- Preserve the four explicit legacy merge modes only through the facade. Groundhog uses the safer default algorithm above.
- Add an additive accessor returning the backing service for real `document-changed` notification.
- Remove the nonexistent opaque-handle `changed` signal claim.
- Retain the facade until in-flight callbacks finish after shutdown; generation invalidation prevents stale mutation.
- Remove plaintext/private-entry logging and success-producing test-only stubs.
- The Gnostr adapter is in-memory, with session-only pending edits and removal records, because Gnostr has no account store. A fetch rebinds the facade to that pubkey.

---

### 3.4 Groundhog services and SQLCipher persistence

#### Account services

Add `GhSocialLists`, a final GObject owned by `GhAppServices`, bound to the current `GhAccountStore`.

It owns:

- The current `GNostrListService`.
- Groundhog adapter context.
- Follow snapshot.
- Immutable hide-policy snapshot.
- Account relay-denial snapshot.
- Account Blossom destinations.
- Sync-enabled state.
- Projection/reindex epoch.

It exposes synchronous cached queries, local edits returning `GError`, and account-scoped change signals:

- `follows-changed`
- `hide-policy-changed`
- `blocked-relays-changed`
- `blossom-servers-changed`
- `sync-state-changed`

Create `GhDraftSync`, `GhReportService`, and `GhBadgeService` beside it, sharing its account lifecycle and adapters where applicable—not its mutable state.

#### Schema v11: account preferences and list state

Add these tables; all are inside the account’s existing encrypted database.

| Table | Fields and defaults |
|---|---|
| `social_preferences` | `id INTEGER PRIMARY KEY CHECK(id=1)`, `sync_enabled INTEGER NOT NULL DEFAULT 0`, `blossom_imported INTEGER NOT NULL DEFAULT 0`, `visibility_revision TEXT NOT NULL DEFAULT ''` |
| `social_documents` | `kind INTEGER`, `identifier TEXT DEFAULT ''`, `event_id TEXT NULL`, `created_at INTEGER DEFAULT 0`, `event_json TEXT NULL`, `decoded_json TEXT NULL`, `private_state INTEGER DEFAULT 0`, `local_revision INTEGER DEFAULT 0`, `dirty INTEGER DEFAULT 0`; primary key `(kind,identifier)` |
| `social_edits` | `sequence INTEGER PRIMARY KEY`, `kind`, `identifier`, `base_event_id TEXT NULL`, `operation_json TEXT NOT NULL` |
| `social_removals` | `kind`, `identifier`, `entry_key TEXT`, `removed_at INTEGER`, `local_revision INTEGER`; composite primary key |
| `social_publications` | `id INTEGER PRIMARY KEY`, `kind`, `identifier`, `local_revision`, `event_json`, `targets_json`, `outcomes_json`, `state`, `next_attempt_at`; pending operation state |
| `social_follows` | `pubkey TEXT PRIMARY KEY`, `relay_hint TEXT NULL`, `petname TEXT NULL`, `position INTEGER` |
| `social_mutes` | `entry_key TEXT PRIMARY KEY`, `tag_name TEXT`, `value TEXT`, `position INTEGER`, `is_private INTEGER` |
| `social_servers` | `url TEXT PRIMARY KEY`, `position INTEGER` |

`social_follows` and `social_mutes` are derived indexes. Updating a document and its indexes is one transaction.
`social_servers` is also derived, from the 10063 document. `entry_key` is the tag name, a NUL byte and the normalized value. When the same entry is in both public and private parts, the private row wins.

No account public key column is needed: the database already belongs to one account.

#### Schema v12: visibility projection

Add:

- `messages.social_hidden INTEGER NOT NULL DEFAULT 0`.
- An index supporting visible-message paging by conversation and message order, and an index on `messages.sender_pubkey`.
- `social_visibility_work(epoch, message_id, hidden)` with composite primary key.

`social_hidden` is a derived local cache, never synchronized.

#### Schema v13: draft sync

Add:

| Table | Required fields |
|---|---|
| `draft_sync_state` | `room_key TEXT PRIMARY KEY`, `draft_d TEXT`, `aliases_json TEXT`, `local_revision INTEGER NOT NULL DEFAULT 0`, `dirty INTEGER DEFAULT 0`, `last_event_id TEXT NULL`, `last_created_at INTEGER DEFAULT 0`, `cleared_at INTEGER DEFAULT 0`, `expires_at INTEGER DEFAULT 0` |
| `draft_publications` | `room_key`, `d`, `local_revision`, `event_json`, `targets_json`, `outcomes_json`, `state`; primary key `(room_key,d)` |
| `draft_conflicts` | `id INTEGER PRIMARY KEY`, `room_key`, `text`, `source_event_id NULL`, `saved_at` |

`conversations.draft` remains the local draft column. It needs no migration.

#### Schema v14: reports and badge cache

| Table | Required fields |
|---|---|
| `report_publications` | `id INTEGER PRIMARY KEY`, `unsigned_json`, `signed_json NULL`, `targets_json`, `outcomes_json`, `state`, `created_at`, `next_attempt_at DEFAULT 0` |
| `social_event_cache` | `event_id TEXT PRIMARY KEY`, `kind`, `author`, `identifier DEFAULT ''`, `event_json`, `stored_at` |

Migration numbers assume this stage order; each is independent and takes the next number if stages land in a different order.

Use `social_event_cache` for badge awards and definitions. Do not broaden `GhPublicNote`’s public-authoring allowlist to include badges or reports.

#### Failure semantics and limits

- Local mutation succeeds only after its SQL transaction commits.
- Storage failure leaves the previous published projection active and reports failure to the initiating UI.
- No list content, draft text, identifiers, ciphertext, or signer payload enters diagnostics.
- Bound documents plus pending edits to 32 MiB per account. Reject additional local edits with a visible storage-limit error; do not evict dirty documents.
- Bound verified badge/reference cache to 1,024 events of at most 64 KiB, evicting least recently used unreferenced entries.
- Memory/ephemeral account stores support local features for the session. Synchronization requires a writable open account store.

#### Destinations and AUTH purposes

| Data | Read from | Publish to | AUTH purpose |
|---|---|---|---|
| Kind 3 (own) | Discovery relays plus NIP-65 write relays, at most 16 | Never | `OWN_LIST_DISCOVERY` |
| 10002, 10050 | Existing | Existing | Existing |
| 10009 | Existing discovery and write union | Same | Existing |
| 10000, 10006, 10030, 10992, 3xxxx sets, 31992 | NIP-65 write relays | Same | `OWN_LIST_DISCOVERY` to read, `OWN_LIST_PUBLISH` to write |
| 10013 | NIP-65 write relays | NIP-65 write relays (NIP-37 requires this) | Same |
| 10008, 10063 (own) | NIP-65 write relays | Same, after explicit confirmation | `OWN_LIST_DISCOVERY` to read, `PUBLIC_POST` to write |
| 3xxxx published publicly | NIP-65 write relays | Same, after explicit confirmation | `PUBLIC_POST` |
| 31234 drafts | Kind-10013 relays only | Same | New `OWN_PRIVATE_SYNC_READ` and `OWN_PRIVATE_SYNC_PUBLISH` |
| 1984 reports | Not read | NIP-65 write relays | `PUBLIC_POST` |
| Others' badges: 10008, 8, 30009 | Discovery relays | Never | `CONTACT_DIRECTORY` |

Lists stay on write relays to limit exposure. A new device still finds them, because it reads kind 10002 from discovery relays first. With no write relays configured, sync reports **Needs a Relay List** and publishes nothing; there is no fallback.

`gh_auth_policy_decide()` takes only a purpose. The private-sync purposes follow the `SELF_WRAP` precedent: the caller guarantees the URL is a current kind-10013 relay (`src/app/gh-auth-policy.h:46-48`), and `ACCOUNT_PURPOSE_FILES` restricts which files may use them.

---

### 3.5 Hide policy across admission, history, reactions, and notifications

#### Interfaces

Add a main-context-owned `GhMessagePolicy` callback to `GhConversationStore`.

Conceptual shape:

```text
classify(message) → INCLUDE | HIDE_RETAIN | DROP, or error
```

Modify the persistence delegate signature atomically:

```text
Before:
admit(data, message, wrap_id, commit, error)

After:
admit(data, message, wrap_id, disposition, commit, error)
```

`disposition` has exactly `INCLUDE`, `HIDE_RETAIN` and `DROP`.

Update all delegates:

- NIP-17 durable delegate.
- NIP-29 durable delegate.
- MLS durable delegate.
- DM inbox memory delegate.
- Test delegates.

Existing `gh_conversation_store_admit()` and `add_message()` public signatures remain unchanged.

Policy errors produce `GH_CONVERSATION_ADD_FAILED`, not rejection or seen-state mutation.

Install the policy in the same call that sets the delegate (`gh_conversation_store_set_account()`, `src/app/gh-conversation-store.c:181`) and clear it with the delegate. If an account-bound policy has not loaded, admission returns `GH_CONVERSATION_ADD_FAILED`, so catch-up after unlock cannot admit a hidden account as `INCLUDE`.

#### Matching rules

Apply only to incoming application content. The account’s own messages remain visible.

| Entry | Matching |
|---|---|
| `p` | Exact normalized sender public key |
| `word` | Nonempty NFC-normalized, Unicode-casefolded substring in textual application content |
| `t` | Casefolded hashtag matching explicit valid `t` tags or textual hashtags; strip one leading `#` on local input |
| `e` | Exact message ID plus descendants connected by explicit root/reply `e` relationships within that conversation |

Do not treat a quote’s `q` tag as a thread-parent edge.

Build hash indexes for public keys, IDs, and hashtags. Compile word patterns into a multi-pattern matcher. Build thread closure with an iterative visited set, bounded by stored messages; cycles cannot recurse indefinitely.

Attachments are not downloaded or OCR-scanned. File URLs and binary metadata do not participate in word matching; captions do.

30007 sets remain manager-only, as D4 requires.

A `p` match returns `DROP`, per D4. A `word`, `t` or `e` match returns `HIDE_RETAIN`, because substring matches produce false positives and silently discarding a friend's message over one would be unrecoverable. `DROP` takes precedence when both apply.

#### Incoming admission

For `DROP` (sender matches a `p` entry):

- NIP-17/NIP-04: commit carrier and message seen keys, but store no application payload.
- NIP-29: commit its event seen key, but store no application payload.
- MLS: preserve authenticated Marmot processing and ratchet state, commit the backend’s seen identity, but store no hidden application payload.
- Return `HIDDEN`.
- Emit neither `message-added` nor `message-committed` for the hidden application message.

When disposition is not `INCLUDE`, check `commit.hidden` before duplicate relay-merging/model handling, so a hidden duplicate cannot revive an already indexed item. `INCLUDE` paths keep today's ordering, because seen-only duplicates must still return `DUPLICATE` (`src/app/gh-conversation-store.c:314-322`).

A later unblock does not retry discarded payloads; their seen keys intentionally remain.

For `HIDE_RETAIN`, each backend stores the message with `messages.social_hidden=1`, commits its seen keys and returns `HIDDEN`, with these differences from normal admission: `unread = FALSE`; no title update; no activity advance; no read-state change; and `commit->hidden = TRUE` set last, after the room-block value is applied (`src/store/gh-store-conversations.c:375-378, 405, 416`). It still creates a conversation row, and Requests queries apply the same no-visible-messages omission as the room list. MLS processing is unchanged. Removing the matching entry reveals the message through the projection below.

#### Existing history and restore

On a committed policy change:

A diff containing only `p` entries, including **Also Hide This Account** from a report, needs no content scan: run one indexed `UPDATE` on `sender_pubkey` plus recount in a single transaction, with no masking phase. The steps below apply to `word`, `t` and `e` changes, and their mask is global.

1. Install the new immutable policy snapshot.
2. Withdraw affected notifications immediately.
3. Mask message content, previews, and unread badges while projection is updating. Show **Updating Hidden Messages…**, not a false unread count of zero.
4. Scan stored messages in batches of 256 on main-context idle turns.
5. Write results into `social_visibility_work` for the current epoch.
6. Evaluate new admissions directly against the new policy.
7. At completion, atomically update flags and stored unread counts, record the policy revision, and discard work rows.
8. Reset affected loaded windows from visible stored rows.
9. Unmask the UI.

An intervening policy edit invalidates the epoch and restarts projection. No old epoch may unmask the model.

All restore/load-older/load-newer/reset-latest queries exclude `social_hidden=1`, including all three backend stores. Paging continues scanning until it fills the requested visible page or exhausts history; hidden rows must not make history appear exhausted prematurely.
Message search (`gh_store_search_message_rooms()`, `src/store/gh-store.c:2316-2320`) and all four unread recounts (`src/store/gh-store.c:2705, 4192, 4262, 4437`) also add `m.social_hidden = 0`, so hidden text is not findable and hidden messages do not return to the badge after a read move or purge.

Recompute:

- Preview and activity from visible messages.
- NIP-17 message-derived subject from visible messages.
- Unread count from visible incoming messages after the existing read boundaries.

Keep group names, membership, timers, room blocks, drafts, and read markers unchanged. A NIP-17 room with no visible messages is omitted unless it is an explicitly opened empty room or has a draft. Joined groups remain listed.

#### Non-chat paths

- **Reactions:** hidden authors cannot add visible reaction chips; reactions targeting hidden messages are not shown. Existing chips are reprojected. Reactions are stored and filtered at projection, so unhiding restores them, and the same filter applies when a pending reaction resolves (`src/store/gh-store-reactions.c:263`; tables at `src/store/gh-store-schema.c:377-401`).
- **Deletions:** still validate and apply legitimate ownership-based deletion requests, even from hidden authors.
- **MLS commits/proposals/membership/branch resolution:** always process normally.
- **NIP-29 metadata/admin/member/removal events:** always process normally.
- **Polls:** do not expose hidden poll definitions or count hidden authors’ votes in the displayed tally. Preserve authenticated protocol processing.
- **MLS invitations:** a Welcome from a hidden author is recorded as processed/declined with its wrap seen, without joining or surfacing an invitation.
- **Notifications:** policy filtering applies before creation and again before send. Policy changes withdraw existing notifications without replaying alerts for retained history when unhidden.

Extend `GhNotifier` with message-ID withdrawal so a mixed group notification can drop hidden messages without discarding unrelated messages. Existing room-level mute/block withdrawal remains.

---

### 3.6 Follows, avatar eligibility, and Requests

#### Follow service

Use `nostr_nip02_parse_follow_list()` for the active account’s signed kind 3.

- Discover on the existing own-list discovery purpose, ephemeral AUTH, from the same bounded source set as kind 10009: discovery relays plus the account's 10002 write relays, at most 16 (`src/nip29/gh-nip29-group-list.c:839-930`).
- Read kind 3 only while `load-profile-pictures` is on, so a default install makes no new request. The persisted set remains stored but unused while the switch is off, and "You follow this account" is shown only with the switch on.
- Verify author and NIP-01 ordering.
- Persist the signed document and derived follow set.
- Preserve relay hints and petnames.
- Do not use follow relay hints as DM destinations.
- This work is read-only for kind 3. Groundhog adds no Follow or Unfollow action, so it can never publish a damaging partial replacement of a list other clients maintain.

#### Directory eligibility

Keep `accepted` and `followed` separate.

Profile eligibility becomes:

```text
accepted
OR own identity
OR (followed AND surfaced AND load-profile-pictures)
```

The follow exception grants kind-0 lookup only. It does not grant acceptance, media permission, or notification detail.

`surfaced` means the pubkey appears in a conversation, Request or group roster. `GhContactDirectory` gets a new input from `GhConversationStore` and the group services that feeds it those peers; today its rescan covers NIP-17 non-requests and MLS DMs only (`src/app/gh-contact-directory.c:1149-1172`). A newly surfaced followed Request uses the existing single-contact path with its 5-60 s randomized delay (`:17-18, :648`), so D3's avatar does not wait for the 2-30 minute run.

Modify all relevant directory gates—not only `dup_picture_uri()`:

- Scheduling.
- Kind-0 admission.
- Profile-change emission.
- Cached display access.
- Pruning.
- Request display.
- Follow/master-switch revocation.

Keep the existing randomized directory batching and discovery-only source selection. Following does not trigger synchronous lookups on the send path or one connection per follow. The profile lookup set is `accepted ∪ (followed ∩ peers surfaced in a conversation, Request or group roster)`; Groundhog never fetches kind 0 for the whole follow list, which would reveal the social graph to discovery relays (PD-12).

#### Picture-cache policy

Add policy inputs to `GhPictureCache` rather than writing followed accounts into persistent consent.

Effective eligibility:

```text
master_enabled
AND (followed OR explicit_contact_consent OR own_identity)
```

Rules:

- Master off: no profile-picture network request on any surface.
- Follow removal: revoke follow-derived eligibility immediately; explicit consent may still permit loading while master remains on.
- Master off: cancel requests, remove textures, and clear explicit consents, preserving the current revocation behavior.
- Re-enable: followed and own-account pictures become eligible; non-followed accounts require fresh manual consent.
- Hidden accounts, and accounts whose only direct room is blocked, are ineligible regardless of following.
- Manual picture loading for a non-followed account requires enabling the master switch and confirming persistent per-contact consent.
- Account-derived eligibility never calls `gh_picture_cache_allow()`.

Copy the consent backend into the cache rather than retaining a pointer whose lifetime is implicit.

Each request captures cache epoch, account generation, and entry serial. Completion may update only the exact current entry. This fixes same-URI stale-completion races.

#### Surfaces and Requests

All surfaces use the same cache decision:

- Conversation rows and headers.
- Message avatars.
- Conversation Info.
- Requests.
- New Message picker.
- Account switcher.
- NIP-29 and MLS member rows.
- Invitee picker and contact-detail surfaces.

For followed Requests:

- Continue showing plain selectable message text only.
- Add avatar, cached cleaned name, full identity access, and **You follow this account**.
- Keep the request unaccepted.
- Keep notifications count-only.
- Do not enable links, previews, attachments, reactions, NIP-05 verification, or other remote content.
- Unknown/non-followed Requests retain the existing no-profile-fetch behavior.

Do not globally remove request guards from web-content code. Add a picture-only exception.

---

### 3.7 Draft persistence and NIP-37 synchronization

#### Local drafts

`conversations.draft` remains the source of truth. Existing save, restore, flush and clear behaviour is unchanged.

- Sync covers NIP-17 rooms only: direct, multi-person and note to self.
- NIP-29 and MLS composers are delegated and have no drafts today (`src/app/gh-send-ui.c:49-59`). That is separate work: `nostrc-2lpm` tracks NIP-29, and MLS direct chats need a follow-up bead.
- Sync covers text only, never unsent file bytes or voice recordings. Reply targets are not stored today and are not added.

#### Privacy-preserving draft identifiers

A public hash of room participants is prohibited because peers can be guessed offline.

- Use a random 256-bit lowercase-hex `d` per NIP-17 room, stored in `draft_sync_state`.
- The room is identified only inside the ciphertext, by the unsigned inner event's author and `p` tags.
- Publish no Groundhog-specific kind, `d` prefix or tag. The charter rejects fingerprinting extensions (PD-1, §2.1), and standard events stay readable by other NIP-37 clients.

#### Draft event and ordering

- Kind 31234, random `d`, `k=14`, `expiration` 30 days after publication.
- NIP-44 content to self: an unsigned kind-14 event authored by the account, with its normal recipient `p` tags and text. A note to self carries the account's own `p` tag.
- Blank events keep `d` and `k=14`, so the import filter below returns them.
- Revisions use NIP-01 ordering (greatest `created_at`, then lowest ID), like every other replaceable document here. A local publication uses `created_at = MAX(now, last_created_at + 1)`, so a new edit always supersedes the revision it replaces.

#### Write path

1. Inside the existing `gh_store_conversations_set_draft()` transaction (`src/store/gh-store-conversations.c:969-998`), save the draft, increment `local_revision` and mark the room dirty. A post-commit signal only wakes the scheduler.
2. After the cadence below, encrypt, sign, and durably save the signed event in `draft_publications`.
3. Publish to the configured, unblocked kind-10013 relays.
4. Clear dirty only when a relay accepts that exact `local_revision`. A newer local revision supersedes it.

#### Import path

Open `{kinds:[31234], authors:[me], #k:["14"]}` on the kind-10013 relays with an explicit EOSE boundary, then stay live. No polling.
Do not create a `d` for a room until import has reached EOSE on every configured kind-10013 relay.

1. Verify signature, author and kind. Ignore expired events.
2. Blank content: if `d` is known and the event is newer than the local revision, apply it as a clear. An unknown `d` is ignored.
3. Decrypt non-blank events newest first, one at a time through the bulk NIP-44 path, at most 64 per bind. Report incomplete sync beyond that bound.
4. Reject an inner event that is signed, is not kind 14, has a different author, or whose room excludes the account. Compute the canonical room key from the inner event.
5. Ignore a room the account has blocked, and any revision with `created_at <= forgotten_before`. Otherwise apply this table:

| Local state | Remote revision | Result |
|---|---|---|
| Any | Not newer than `cleared_at` or the last known revision | Ignore |
| Clean | Newer | Replace through a programmatic composer update, which suppresses draft-change emission |
| Dirty, or composer has focus, with different text | Newer | Keep local; save the remote text in `draft_conflicts` |
| Same text | Newer | Record the revision only |

Applying a remote draft goes through a new `GhSendUi` apply-remote-draft entry point, with a focus query, which must also update `draft_saved` so a later matching edit is not dropped (`src/app/gh-send-ui.c:394`). A remote draft for a room not yet in `GhConversationStore` adds it to the model when it is imported.

6. If a room has two identifiers (concurrent first creation on two devices), keep the newest revision, write future revisions under the lexicographically lowest known `d`, and blank the other once the new head is accepted.
7. Undecryptable or malformed events count as incomplete sync. Never overwrite or blank them automatically.

#### Cadence and remote signer behavior

- Local save: existing one-second debounce.
- Network publication: after 30 seconds without edits. For NIP-46 accounts: when leaving the room, or after 5 minutes without edits, because each revision can mean a signer prompt.
- Automatic draft signing uses the NIP-46 bulk lane, through a new background hint on the signer context, so it cannot delay the user's own signatures (`src/identity/gh-nip46-session.c:595-597, 1355`).
- Maximum automatic publication rate: once per room per 60 seconds.
- Switching rooms schedules the latest saved revision; it does not force a second publication inside the rate limit.
- One draft encryption/sign pipeline runs at a time.
- A newer local revision supersedes an unsigned pending one.
- A revision changed during signing discards the result.
- Read-only/signer-unavailable: save locally and show **Saved on This Device · Waiting for Signer**.
- Signer denial stops automatic attempts for that dirty revision until explicit Retry or a later edit.
- Network failure retries the same stored signed event; it does not re-prompt for signing.

#### Atomic clear at send

Extend the actual enqueue transaction (`src/store/gh-store.c:3459-3472`), not the later local echo, for chat kinds only. Kinds 5 and 7 keep the draft, as they do today.

1. Insert outgoing message/outbox state.
2. Clear the local draft.
3. Set `cleared_at` and mark the room dirty.
4. Queue a blank-content kind-31234 event for every known identifier.

These changes commit or roll back together. With sync off, steps 3 and 4 still record the tombstone for a room that has a published identifier, so a later enable clears it remotely.
Each enqueue site (`src/app/gh-outbox.c:2028, 2131, 2193`) wakes `GhDraftSync` after its transaction commits.

Keep `cleared_at` until `expires_at`, so a late older revision cannot resurrect the draft.

#### Enabling sync

`conversations.draft` has no timestamp, so an existing draft's age is unknown. On enable, wait until import reaches EOSE on every configured kind-10013 relay. Then, for each pre-existing local draft:

- No remote revision for the room: mark it dirty and publish normally.
- A remote revision with different text: run conflict handling, keeping the remote revision and saving the local text in `draft_conflicts`. Never publish the local text as newest.

#### Forgetting a conversation

`gh_store_forget_conversation()` (`src/store/gh-store.c:4327, 4366-4370`) sets `cleared_at` and queues blank events for every known `d` in its transaction, on the same terms as the atomic clear. Whole-account forget (`gh_store_forget_account`, `:2064`) cannot queue anything, so its confirmation says synced drafts remain on private relays until they expire.

**Known limit:** a device learns a `d` only by decrypting, so a second identifier can appear after concurrent creation, a signer outage during import, or beyond the 64-event bound. A fresh device can then show one stale draft, bounded by its 30-day expiration.

#### Relay and AUTH policy

- Discover kind 10013 on the account’s NIP-65 write relays.
- Require public item tags empty and private `relay` tags readable.
- Publish 10013 to NIP-65 write relays.
- Publish/read drafts only on configured, unblocked kind-10013 private relays.
- No fallback to 10002, 10050, discovery relays, or hardcoded destinations for draft payloads.
- Add `GH_AUTH_PURPOSE_OWN_PRIVATE_SYNC_READ`: account AUTH only on those private relays.
- Add `GH_AUTH_PURPOSE_OWN_PRIVATE_SYNC_PUBLISH` for those same relays.
- Amend R1 accordingly.
- A relay that refuses AUTH leaves the draft local and reports its exact destination-level failure.

If no private relay is configured, enabling sync still permits other private-list synchronization, but draft status says **Choose a Private Relay to Sync Drafts**.

---

### 3.8 Relay denial and account relay integration

Add `GhRelayDenyPolicy`, an immutable normalized-URL snapshot owned by `GhSocialLists`.

Use one normalization helper consistent with existing relay URL handling. Preserve meaningful paths; do not block unrelated paths merely because hosts match.

Enforce at:

- `GhRelayNet::open_inner()`.
- `GhRelayGuard` initial scope/publish open.
- Reconnect.
- AUTH send.
- Scope event write.
- Publish resend.

On policy change:

1. Update the snapshot.
2. Close matching live application relay connections on their owner contexts before scheduling reconnect work.
3. Report a policy-denied outcome.
4. Prevent reopening while denied.

This covers message, group, discovery, profile, list, and report connections. NIP-46 signer-control connections remain a separate credential transport and are explicitly excluded, so blocking a bunker's relay cannot cut off the signer needed to undo the edit; document this distinction in the UI. Implement the exclusion with an explicit connection class on `GhRelayScope` and `GhRelayPublish` (a `set_signer_control()` call), set only at the three NIP-46 sites (`src/identity/gh-nip46-session.c:512-515, 782-784, 1294-1296`), checked by the deny hook, and restricted to that file by a new `check_privacy.py` rule.

Also check the deny policy before the NIP-11 key fetch in `GhNip29Service` (`src/nip29/gh-nip29-service.c:1106`), reporting the relay as policy-denied.

Policy state must be tied to account generation. Load it before restoring network-active account services. On switch, close the old generation before installing the next policy.

#### All recipient relays blocked

Do not rewrite or delete the recipient’s signed kind-10050 list.

- Resolver continues reporting the actual signed destinations.
- Target planning marks denied targets separately.
- Add an additive `BLOCKED_BY_LOCAL_POLICY` leg/target reason.
- If every destination is denied, show **Not Sent — All of This Account’s Message Relays Are Blocked**.
- Keep the message queued locally for recovery.
- Do not increment network-attempt counts for policy refusal.
- Never fall back to NIP-65 or defaults.
- Unblocking a destination triggers outbox reconciliation of the same stored wrap.
- Already accepted target outcomes remain accepted.

Existing 10002/10050 setup remains the sole editor for those lists. The generic list manager links to those editors rather than introducing competing writers.

---

### 3.9 Blossom destinations

`social_servers` becomes the runtime source for uploads.

Migration:

1. On first writable bind of a store migrated from below v11, copy the existing ordered global `blossom-servers` values into that account if it has no account destination state. Fresh stores set `blossom_imported=1` at creation.
2. Set `blossom_imported=1` transactionally.
3. Do not publish.
4. Leave the GSettings key as a deprecated migration/rollback input; new runtime consumers do not read it.
5. Each pre-existing account receives the legacy configuration once when opened.

Remote 10063 is public and empty-content:

- Preserve server order.
- Validate using existing Blossom URL/security rules.
- Receiving a newer own 10063 updates local destinations only during an enabled/explicitly requested import.
- Locally dirty destination edits are rebased; do not overwrite them silently.
- **Publish Server List…** previews the public URLs and selected NIP-65 write targets.
- Private-sync permission alone does not authorize this publication.

Existing per-upload destination and server-disclosure confirmations remain. Empty or entirely unusable destinations prevent upload and open the destination chooser; no fallback server is introduced.

---

### 3.10 NIP-56 report workflow

Add `GhReportService` and a separate `GhReportDialog`.

#### Entry points

- Incoming message menu.
- Conversation Info person row.
- NIP-29/MLS member row.
- Request sender menu.

Disable reporting oneself.

Use **Report Account…** for private contexts. Use **Report Message…** only for a signed NIP-29 event whose group metadata explicitly establishes public visibility. Unknown visibility is private. A new predicate requires received kind-39000 metadata and `!is_private`, because `is_private` defaults to false for a group with no metadata (`src/nip29/gh-nip29-group.h:124`; `src/nip29/gh-nip29-service.c:382, 1498`).

#### Payload and targets

| Origin | Payload | Default targets |
|---|---|---|
| NIP-17/NIP-04/MLS/private NIP-29 | Typed `p`; optional user explanation | Account’s unblocked NIP-65 write relays |
| Explicitly public NIP-29 message | Typed `p` and `e` using its signed public event ID | Same |
| Person/member row | Typed `p` | Same |

Do not automatically add the group relay, recipient inbox, discovery relay, or an evidence URL.

The dialog shows:

- Reporting identity.
- Report category.
- Optional explanation.
- Exact public tags.
- Exact destination list.
- A warning that the report is public and links reporter to reported account.
- **Also Hide This Account**, off by default.

Submit label: **Publish Report**. Cancel is the default response.

Apply account hiding as a separate local transaction when the user submits, before network publication. If hiding fails, do not publish; show the storage failure. If report publication fails, keep the successful hide and explain that the report was not accepted.

#### Delivery behavior

Persist the user-approved report before asking for a signature. After signing, verify author, ID, signature, kind, tags, and content against the approved unsigned event.

Use `GhRelayPublish` with the existing ephemeral `PUBLIC_POST` purpose (`src/app/gh-auth-policy.h:28`), not a blanket account-AUTH exception.

- At least one accepted/duplicate relay: **Report Accepted by a Relay**.
- None accepted: failure with per-target Retry.
- Partial acceptance: retain outcomes.
- Retry sends the same signed event.
- Cancellation stops unfinished work; already accepted reports remain public.
- After account switch, no further publication occurs until that account is reopened and the user retries.

Keep `app.report-issue` and its NIP-34 infrastructure unchanged.

---

### 3.11 Emoji picking and image display

#### Portable support

Extend `nip30` additively to:

- Recognize hyphens as well as alphanumerics/underscore in shortcodes.
- Preserve optional set address.
- Add a full-tag builder while retaining the existing builder.
- Validate image URL syntax without fetching.

#### Groundhog behavior

- Kind 10030 provides ordered favorites.
- Kind 30030 supplies named sets.
- Add a Custom section to the reaction/emoji picker with shortcode labels and textual fallback.
- Selecting an emoji in a composer inserts `:shortcode:` and attaches the matching NIP-30 `emoji` tag to the outgoing event when it is sent.
- This covers kind 7 reactions (defined by NIP-30 and allowed in NIP-17 chats) and chat messages in NIP-17, NIP-29 and MLS. Clients without support show the literal `:shortcode:`, which NIP-30 expects. The tag is a standard NIP tag, encrypted inside the wrap for private chats.
- Public kinds supported by NIP-30, including existing public post paths, use the same portable builder.
- Received custom emoji show as shortcode text until the user chooses **Load Custom Emoji** on that message, following the §2.1 per-message consent rules. Requests never offer it.
- Carriers: extend `GhSendUiDelegate.send` (`src/app/gh-send-ui.h:94`) and the reaction send path with an optional tag list. `GhReaction` caps emoji at 64 bytes (`src/app/gh-reaction.h:23, 36-41`), so the picker does not offer a custom emoji whose `:shortcode:` exceeds that.
- Consent: add `GH_WEB_EMOJI` and `GH_WEB_BADGE` to `GhWebKind` (`src/ui/gh-web-content.h:6-7`), both mapped to the existing `load-remote-images` key, so no new GSettings key is needed. `GhMessageRow` renders the loaded image inline in place of the shortcode.

PD-2 behavior:

- No image fetched when a list or picker opens.
- A placeholder exposes **Load Emoji Images…**.
- Consent is session-only for the selected set/destination collection.
- Fetch through existing `GhWebContent`/`GhNetHttp`, respecting Tor, public-host checks, TLS isolation, and existing media size/dimension limits.
- Closing the relevant UI or revoking permission cancels loads.
- Malformed, failed, or denied images remain readable shortcodes.
- Never expand a remote-image URL into markup or fetch arbitrary set references automatically.

---

### 3.12 Badges

Add `GhBadgeService`, owned per account, with explicit lookup operations and cached immutable badge records.

Validation chain:

1. Verify profile-badge list signature and owner.
2. Read ordered consecutive `a`/`e` pairs; ignore unmatched entries.
3. Fetch award by exact ID only after lookup permission.
4. Verify award signature.
5. Require award author to equal the issuer in the definition address.
6. Require the displayed account in an award `p` tag.
7. Require the award’s `a` to match the selected definition.
8. Fetch definition by exact author/kind/`d`; select by NIP-01 ordering.
9. Show a badge only after the full relationship is valid.

Relay hints are hints, not automatic permission to contact new destinations. Default lookups use configured discovery relays; additional hinted relays require explicit approval.

#### UI

- Conversation Info person details: cached badge names and **Look Up Badges…**.
- Preferences → Account → **Profile Badges**: received awards, selected ordered display list, add/remove/reorder, and **Publish Profile Badges…**.
- Requests do not trigger badge lookup.
- Badge artwork is separate remote-image consent; textual badges work without fetching images.
- Copy says **Awarded by …**, not “Verified identity.”

Acceptance means adding an award/definition pair to the account’s selected 10008 list. It is not automatic upon receiving an award.

#### Legacy migration

- If canonical 10008 exists, it wins.
- Otherwise display valid legacy 30008 `d=profile_badges` as equivalent.
- On the user’s next explicit profile-badge publication, publish canonical 10008.
- Do not automatically publish a deletion of legacy 30008.
- Preserve ordinary 30008 badge sets.
- An empty canonical 10008 intentionally clears the profile and must not fall back to legacy data.

---

### 3.13 Preferences and generic manager

Use existing GTK 4.14/libadwaita 1.5-compatible patterns: navigation pages, boxed lists, action rows, switch rows, status pages, alert dialogs, and plain-text labels.

#### Placement

| Preferences page | Additions |
|---|---|
| Privacy | Hidden Accounts and Content; existing Blocked Conversations remains separate |
| Messages | Drafts, conflict copies, synchronization status |
| Network | Blocked Relays; Private Sync Relays; existing DM relay editor; account Attachment Servers |
| Account & Storage | Sync Drafts and Lists; Lists and Sets; Profile Badges |
| Emoji picker | Favorites and sets, with explicit image-loading control |

The sync row is injected through a store-backed backend, not `g_settings_bind()`.

Suggested description:

> Sync private drafts and supported lists with your other devices. Contents are encrypted to your account. Relays can still see your account, event kinds, update times, and sizes. Public lists require a separate Publish action.

#### Generic manager

Add `GhListsPage` and `GhListEditorPage`, backed by service snapshots.

Support:

- Document selection by kind and address.
- New set with required `d`.
- Metadata editing.
- Typed entry editors for known forms.
- Raw tag-array editor for preserved extensions.
- Add/remove/reorder.
- Public/private visibility with disclosure.
- Local save independently of sync.
- Explicit import/export.
- Sync/read/publish failure details.
- Delete set by publishing an empty replacement only after confirmation; do not silently issue broad NIP-09 deletion requests.

Slash commands:

- Manage 10992 references and 31992 definitions.
- Validate and retain `arg` choices.
- No execution, shell launch, network invocation, composer interception, or requirement that ordinary text parse as a command.

#### Existing UI contracts

- Keep existing room Block and Undo behavior.
- Add Hide Account alongside it, not in place of it.
- Request Block continues block-and-forget; Hide Account is a distinct action.
- Blocking/unblocking never automatically accepts a request.
- `GhBlockedPage` remains the room-block page.
- Remove cached display-name substitution from its npub-only adapter, or explicitly update its contract and tests. This specification chooses **npub-only**, preserving PT-8.

Dialogs capture account generation and target identity. Dismiss them on account switch or backend replacement. Backend failure keeps the current row and focus; success moves focus to the next surviving row or the page heading.

---

### 3.14 Charter, privacy checks, and versions

#### Exact charter amendments

Update `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`:

| Rule | Required amendment |
|---|---|
| P7 | List, draft, badge, report, and image operations cancel before account storage closes |
| P8 | Read state, pins, notification mutes, and room blocks remain local. Drafts and supported list entries may sync only under per-account opt-in; public actions require separate approval |
| P9 | Explain public outer metadata, recipient/account relationships in reports, public badges/server lists, private-relay AUTH, and irreversible accepted publications |
| PD-2 / §2.1 | Master-gated followed avatars; independent emoji/badge artwork consent, clarifying that a standard NIP-30 tag is not a fingerprinting private tag; existing fetch/security limits unchanged |
| PD-4 | Drafts use only 10013 private relays; denied DM relays never cause fallback |
| PD-6 | Give list, private-sync, report and badge traffic separate sockets and isolation labels; report and badge publication reuse the `PUBLIC_POST` purpose |
| PD-8 | Followed Requests may receive kind-0 identity/avatar lookup under the picture master switch; acceptance and all other quarantine restrictions remain |
| PD-11 | Sync preference, follows, list state, draft routing, destinations, and hide policy reside in SQLCipher |
| PD-12 | Follow-derived kind-0 lookup stays on discovery connections and off the send path |
| R1 | Allow account AUTH for private-sync reads/publications only on active account 10013 destinations |
| §7.9 | Distinguish notification mute, author hide, room block, and public report |
| §7.11 | Add store-backed account preference and new manager pages; correct picture-consent and Blossom descriptions |
| Kind 10009 amendment | Shared service replaces the implementation; automatic private publication and explicit Restore Groups are unchanged |

#### Privacy-test identifiers

Add runtime tests with these exact paths:

- `/privacy/social/default-no-sync` (kind 10009 excepted; it keeps its existing behaviour)
- `/privacy/social/publication-needs-confirmation`
- `/privacy/social/account-teardown`
- `/privacy/avatars/master-off-all-surfaces`
- `/privacy/avatars/followed-request-quarantine`
- `/privacy/drafts/opaque-identifiers`
- `/privacy/drafts/private-relays-only`
- `/privacy/drafts/private-auth-purpose`
- `/privacy/reports/no-private-evidence`
- `/privacy/relays/no-fallback-when-blocked`
- `/privacy/blossom/account-isolation`
- `/privacy/emoji/no-default-fetch`
- `/privacy/badges/no-default-fetch`

In `tests/check_privacy.py`:

- Do not add a GSettings sync key.
- Keep existing key types/defaults; mark `blossom-servers` deprecated.
- Add private-sync purposes to `ACCOUNT_PURPOSE_RE`.
- Add only the new adapter/private-sync files to `ACCOUNT_PURPOSE_FILES`, with destination-specific justification.
- Remove stale account-AUTH allowance from `gh-nip29-group-list` after publication moves to the adapter.
- Add static rules for private report evidence construction, raw-key use in Groundhog social services, and private-sync destination selection.
- Add clean and failing mutation fixtures for every new static rule.
- Do not relax existing NIP-17 relay-source rules.

Static checks supplement—not replace—the runtime payload and destination tests.

#### Versions

- Groundhog: **0.13.0**.
- `nostr-gobject`: **3.1.0**, same SONAME 3; additive APIs and compatibility facade.
- libnostr: **2.4.0**, reflecting the expanded public kind vocabulary.
- gnostr: PATCH, because the facade removes private-entry logging; no other behaviour change is intended.
- NIP-51 and NIP-58 remain **Unversioned** in the current repository convention; do not invent standalone releases.
- New NIP-37/NIP-56 modules enter `VERSION_MANIFEST.md` as **Unversioned**.
- Record NIP-30 additions without changing its existing versioning convention.
- Update all version declarations and Groundhog packaging inputs checked by `groundhog-packaging-version` together.

## 4. File-by-file impact

A row listing `.c/.h` means both files change. **Candidate** means the file was not inspected during planning. Paths in §4.1 are repository-relative. Paths in §4.2–§4.6 are under `gnome/groundhog/`, unless they begin with `docs/` or name `VERSION_MANIFEST.md`.

### 4.1 Protocol and shared libraries

| Files | Changes and dependencies |
|---|---|
| `libnostr/include/nostr-kinds.h` | Add missing constants and explicit badge aliases; preserve old numeric meanings |
| `nips/nip51/include/nostr/nip51/nip51.h` | Add opaque lossless document/tag/schema APIs; retain legacy structs/functions |
| `nips/nip51/src/nip51.c` | Replace unsafe JSON paths; implement signer-independent codecs and fail-closed wrappers |
| `nips/nip51/src/nip51-schema.c` **new** | Schema registry and typed validation |
| `nips/nip51/tests/test_nip51.c` | Lossless, escaping, schema, bounds, failure, and legacy regression cases |
| `nips/nip51/CMakeLists.txt`, `TestInventory.cmake` | Compile registry; register actual test inputs |
| `nips/nip37/include/nostr/nip37/nip37.h`, `src/nip37.c`, `tests/test_nip37.c`, `CMakeLists.txt`, `TestInventory.cmake` **new** | Pure draft/checkpoint codec |
| `nips/nip56/include/nostr/nip56/nip56.h`, `src/nip56.c`, `tests/test_nip56.c`, `CMakeLists.txt`, `TestInventory.cmake` **new** | Pure report codec |
| `nips/nip58/include/nostr/nip58/nip58.h`, `tests/test_nip58.c` | Add canonical 10008 and badge-set APIs/tests |
| `nips/nip58/src/nip58.c` **candidate** | Implement additive APIs while preserving legacy builders |
| `nips/nip30/include/nostr/nip30/nip30.h` | Add full emoji-tag access/build API and hyphen contract |
| `nips/nip30/src/nip30.c`, `tests/test_nip30.c` **candidates** | Parser/matcher/builder tests |
| `NipOptions.cmake` | Add NIP-37/56 options; turn on NIP-02 (`ENABLE_NIP02` is OFF, `:8`) and link it into Groundhog; resolve dependencies before adding subdirectories |
| `nips/generate_cmake.sh` **candidate** | Update generator source so generated options survive regeneration |
| `nostr-gobject/include/nostr-gobject-1.0/gnostr-list-document.h`, `gnostr-list-service.h` **new** | Immutable document, operation, adapter, state, and async contracts |
| `nostr-gobject/src/gnostr-list-document.c`, `gnostr-list-service.c` **new** | Shared engine; depends on portable codecs |
| `nostr-gobject/include/nostr-gobject-1.0/gnostr-mute-list.h` | Preserve ABI; document facade and additive backing-service accessor |
| `nostr-gobject/src/gnostr-mute-list.c` | Replace duplicate parser/sync pipeline; preserve explicit compatibility strategies |
| `nostr-gobject/src/gnostr-list-gnostr-adapter.c` **new** | Existing Gnostr pool/signer bridge adapter; no Groundhog dependencies |
| `nostr-gobject/tests/test_list_service.c`, `test_mute_list_compat.c` **new** | Real service state-machine and facade tests |
| `nostr-gobject/CMakeLists.txt` | Sources, installed headers, GIR inputs, archive closure, tests, version |
| `nostr-gobject/include/nostr-gobject-1.0/nostr-gobject.h` **candidate** | Export new public headers |

Validate standalone installed-library linking and the executable-side archive contract. Do not introduce a second libnostr copy in shared builds.

### 4.2 Groundhog persistence and services

| Files under `gnome/groundhog/` | Changes and dependencies |
|---|---|
| `src/store/gh-store-schema.c`, `src/store/gh-store.h` | Append v11–v13; bump schema constant; expose draft sync state |
| `src/store/gh-store-social.c/.h` **new** | Preferences, documents, edits, indexes, publication state, destinations |
| `src/store/gh-store-drafts.c/.h` **new** | Draft sync state and conflict copies |
| `src/store/gh-store.c` | Atomic draft clear and tombstone at actual enqueue and forget; `m.social_hidden = 0` in search and the four unread recounts |
| `src/store/gh-store-conversations.c/.h` | Draft dirty and revision update inside `set_draft`; filtered admission; visible paging/recount |
| `src/store/gh-store-nip29.c/.h` | Filtered admission, seen-only hidden path, visible paging |
| `src/store/gh-store-reactions.c` | Store hidden-author reactions and filter at projection |
| `src/store/gh-store-mls.c` | Filtered admission preserving Marmot transaction; visible paging |
| `src/store/gh-store-marmot.c` | Processed/declined Welcome storage integration |
| `src/app/gh-social-lists.c/.h` **new** | Account owner, shared-service adapter wiring, derived projections |
| `src/app/gh-list-adapter.c/.h` **new** | `GhRelayScope`, `GhRelayPublish`, signer, clock, SQLCipher adapter |
| `src/app/gh-message-policy.c/.h` **new** | Matching/indexes and history projection coordinator |
| `src/app/gh-draft-sync.c/.h` **new** | NIP-37 synchronization and cadence |
| `src/app/gh-report-service.c/.h` **new** | Approved report persistence/sign/publish |
| `src/app/gh-badge-service.c/.h` **new** | Verified badge graph and explicit lookups |
| `src/app/gh-account-store.c/.h` | Bind/close hooks; social services stop before store close |
| `src/app/gh-app-services.c` | Own services; inject UI backends; replace global server consumers |
| `src/app/gh-app-outbox.c` | Delegate kind-10009 lifecycle to shared service |
| `src/nip29/gh-nip29-group-list.c/.h` | Compatibility projection over shared engine |
| `src/app/gh-auth-policy.h`, `gh-auth-policy.c` **candidate** | Private-sync purposes (caller-guaranteed URLs, following `SELF_WRAP`) |
| `src/app/gh-account-controller.c/.h` | Reuse existing guarded async API; add purpose/context propagation only if missing from current callable surface |
| `src/identity/gh-signer.c`, `src/identity/gh-nip46-session.c` | Background lane hint for automatic draft signing; signer-control connection class at the three NIP-46 sites |
| `src/app/gh-features.h` | Truthful feature gates tied to actual linked services |

No change is required to `GhPublicNote`’s kind allowlist or its cache.

### 4.3 Admission, send, and notification consumers

| Files | Changes |
|---|---|
| `src/app/gh-conversation-store.c/.h` | Policy binding; delegate disposition argument; hidden-before-duplicate behavior |
| `src/app/gh-conversation-private.h` | Projection/window-refresh hooks |
| `src/app/gh-conversation.c` **candidate**, `gh-conversation.h` | Projection-loading state and visible-history derived properties |
| `src/app/gh-message.c/.h` | Extract reply/thread and emoji metadata without network access |
| `src/app/gh-dm-inbox.c/.h` | Update memory delegate; preserve checkpoint semantics |
| `src/app/gh-nip04-inbox.c` | Existing admission remains; test hidden legacy messages |
| `src/nip29/gh-nip29-service.c` | Separate application filtering from group control; reaction/poll projection |
| `src/mls/gh-mls-service.c` | Preserve commits/ratchets; filtered app handling, Welcome decline, reaction/poll projection |
| `src/app/gh-notifier.c`, `gh-notifier.h` **candidate** | Message-level withdrawal and policy recheck |
| `src/app/gh-send-ui.c`, `gh-send-ui.h` **candidate** | Sync status; apply-remote-draft entry point and focus query; tag-carrying delegate send |
| `src/ui/gh-group-ui.c`, `src/ui/gh-mls-ui.c` **candidates** | Pass emoji tags through their send delegates |
| `src/nip29/gh-nip29-group.h`, `src/nip29/gh-nip29-service.c` | Known-public group predicate for Report Message; deny check before NIP-11 key fetch |
| `src/app/gh-outbox.c` | Policy-blocked targets; recovery without new wraps |
| `src/app/gh-outbox.c`, `src/app/gh-nip17-envelope.c` **candidates** | Carry `emoji` tags into the stored rumor |
| `src/app/gh-dm-send.h`, `gh-dm-send.c` **candidate** | Add explicit local-policy target/leg outcome |
| `src/app/gh-message-status.c/.h` | Honest blocked-relay status/reason |
| `src/app/gh-reaction-store.c`, `src/app/gh-reaction.c/.h` **candidates** | Reproject hidden authors/targets; retain valid deletion handling |

### 4.4 Relay enforcement

| Files | Changes |
|---|---|
| `src/relay/gh-relay-deny-policy.c/.h` **new** | Immutable generation-bound URL denial snapshot |
| `src/net/gh-relay-net.c`, `gh-relay-net.h` **candidate** | Initial/reconnect/write/AUTH/resend enforcement; close live denied sockets |
| `src/relay/gh-relay-guard.c`, `gh-relay-guard.h` **candidate** | Same enforcement for non-Tor builds |
| `src/relay/gh-relay-scope.c`, `src/relay/gh-relay-publish.c` **candidates** | Signer-control connection class |
| `src/app/gh-account-relays.c`, `gh-account-relays.h` **candidate** | Shared revision offers for 10002/10050; preserve sole editor behavior |
| `src/app/gh-inbox-setup.c/.h` | Publish accepted own-list revisions back to shared tracking |
| `src/app/gh-relay-list-setup.c/.h` **candidates** | Same for 10002 |

Reuse existing socket-writing code; `gh-relay-gnostr-write.c` needs no new framing implementation.

### 4.5 Avatar and UI consumers

| Files | Changes |
|---|---|
| `src/app/gh-contact-directory.c/.h` | Separate accepted/followed/profile eligibility; all scheduling/display gates |
| `src/app/gh-contact-titles.c` **candidate**, `gh-contact-titles.h` | Followed Request identity display without acceptance |
| `src/ui/gh-picture-cache.c`, `gh-picture-cache.h` **candidate** | Master/follow policy, copied backend, per-request epoch validation |
| `src/ui/gh-conversation-view.c` | Picture-only Request exception; central revocation; report dispatch |
| `src/ui/gh-message-row.c` | Hide/report actions and safe emoji fallback |
| `src/ui/gh-web-content.c/.h` | `GH_WEB_EMOJI` and `GH_WEB_BADGE` kinds |
| `src/ui/gh-conversation-row.c` **candidate** | Central avatar eligibility and projection masking |
| `src/ui/gh-conversation-list.c/.h` **candidates** | Bind each window cache to shared account policy |
| `src/app/gh-account-ui.c` | Remove automatic persistent consent grants |
| `src/ui/gh-new-message-page.c` **candidate** | Follow-derived pictures through central policy |
| `src/ui/gh-requests-view.c/.h` | Followed identity/avatar, distinct Hide/Report actions, stale confirmation dismissal |
| `src/ui/gh-conversation-info-dialog.c/.h` | Person Hide/Report/Badges; preserve room Block |
| `src/ui/gh-group-info-dialog.c` | Member Hide/Report and centrally permitted avatars |
| `src/ui/gh-mls-group-info-dialog.c` **candidate** | Same |
| `src/ui/gh-mls-invitee-picker.c` | Central avatar policy |
| `src/ui/gh-conversation-menu.c` | Distinct account-hide/report entry points where exactly one peer exists |
| `src/app/gh-blocked-conversations.c/.h` | Preserve npub-only room-block contract |
| `src/ui/gh-composer.c/.h` | Draft sync status; custom-shortcode insertion; emits selected emoji tags |
| `src/ui/gh-reaction-picker.c/.h` **candidates** | Favorites/sets and explicit artwork loading |
| `src/ui/gh-public-post-dialog.c`, `gh-public-note-post.c/.h` | Supported public NIP-30 tags only |
| `src/media/gh-blossom-client.c/.h` **candidates** | Account-provided server snapshot, no runtime global setting |

New UI pairs:

- `src/ui/gh-lists-page.c/.h`
- `src/ui/gh-list-editor-page.c/.h`
- `src/ui/gh-hidden-page.c/.h`
- `src/ui/gh-drafts-page.c/.h`
- `src/ui/gh-report-dialog.c/.h`
- `src/ui/gh-badges-page.c/.h`

Each receives copied backend callbacks and owned account context rather than borrowing an unguarded store.

### 4.6 UI resources, documentation, build, and tests

Modify these Blueprint sources and regenerate their committed `.ui` counterparts:

- `gh-preferences-dialog`
- `gh-requests-view`
- `gh-conversation-info-dialog`
- `gh-conversation-info-person`
- `gh-message-row`
- `gh-content-page`
- `gh-sidebar-page`
- `gh-group-info-dialog`
- `gh-mls-group-info-dialog`
- `gh-mls-invitee-row`

Add Blueprint/generated UI pairs for the six new UI components and their list rows. Update `gh-conversation-menu.blp` and its generated resource output.

Other files:

| File | Change |
|---|---|
| `src/ui/gh-preferences-dialog.c/.h` | Store-backed social backend and navigation rows |
| `data/org.nostr.Groundhog.gschema.xml` | Correct picture/discovery descriptions; deprecate Blossom key; no new account-state key |
| `data/groundhog.gresource.xml` | New resources |
| `CMakeLists.txt` | Protocol dependencies, service/UI targets, truthful feature gates, named tests |
| `TestInventory.cmake` | Explicit new test registrations, actual targets, inputs, display/private-bus requirements |
| `tests/check_privacy.py` | New purposes/rules/mutations; preserve existing guards |
| `po/POTFILES` **candidate** | All new translatable sources/resources |
| `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md` **candidate** | Amend rules listed above |
| `VERSION_MANIFEST.md` **candidate** | Component versions and new module entries |
| `packaging/flatpak/org.nostr.Groundhog.yml` (under `gnome/groundhog/`) | Enable required protocol targets; version consistency |
| Root/component version declarations and packaging files named by `groundhog-packaging-version` **candidates** | Atomic version update |

Unrelated selected voice, update-handoff, diagnostics, and link-preview implementation files require no feature changes.

## 5. Risks and migration

### 5.1 Stored-data migration and rollback

- v11–v13 are append-only transactional migrations.
- Existing room blocks, notification mutes, consent records, read state, and seen keys remain unchanged.
- Sync defaults off for every migrated account.
- Existing `conversations.draft` values are untouched.
- Blossom migration never publishes.
- Visibility flags are rebuildable; a mismatched revision triggers reindex before content is exposed.
- Badge migration is read-compatible and publication-driven, not automatic.

Older binaries must follow the existing newer-schema refusal behavior. Do not support rollback by silently opening v13 as v10. Document restoring a pre-upgrade encrypted backup as the rollback route.

### 5.2 Protocol consistency limits

NIP-01 replacement events are not a transactional distributed database. Concurrent offline edits cannot be guaranteed lossless against devices that never observe each other’s revisions.

Mitigations:

- Durable local operations.
- Full-revision checks before publication.
- Draft tombstones and identifier tracking.
- Conflict copies instead of silent draft overwrite.
- Partial-read publication confirmation.
- Explicit “accepted by a relay” wording.

Do not claim atomic cross-relay draft publication.

### 5.3 Validation required during implementation

Validate these unselected seams before modifying them:

1. Actual root ordering/generator for `NipOptions.cmake`; run a clean configure with NIP-44 initially disabled and NIP-02/37/51 enabled.
2. Every `GhConversationDelegate` initializer and every enqueue writer; compile all feature combinations and test that no path bypasses atomic draft clearing.
3. Existing `GhWebContent` byte/dimension limits; reuse them rather than introducing larger emoji/badge allowances.
4. Gnostr facade caller threading and account-binding sites; preserve synchronous query behavior while removing stale singleton callbacks.
5. Marmot processed-Welcome state values; use its actual declined representation.
6. Root version declarations, packaging inputs, and installed-library export conventions.

These checks confirm integration details; they do not reopen the behavioral decisions above.

## 6. Implementation order and verification

Each stage must leave a usable application. New UI controls remain hidden until their real consumers are linked.

### Stage 1: Portable codecs and kind vocabulary

Implement lossless NIP-51, NIP-37, NIP-56, additive NIP-58, and NIP-30 support.

**Tests:**

- Existing `test_nip51`: escaped strings, empty positional fields, arbitrary trailing fields, order preservation, unknown tags, malformed JSON, allocation/error propagation, failed encryption never producing an event.
- Existing `test_nip58`: legacy regressions plus canonical 10008, consecutive pairing, empty canonical precedence.
- New `test_nip37`.
- New `test_nip56`.
- Existing NIP-30 test target: hyphens and set-address round-trip.

Land build-generator and generated options changes together.

### Stage 2: Shared list engine and compatibility facade

Implement the document/service and Gnostr adapter; migrate `GNostrMuteList`.

**Tests:**

- New `test_nostr_gobject_list_service`.
- New `test_nostr_gobject_mute_list_compat`.

Cover out-of-order revisions, equal timestamps, duplicate EOSE, CLOSED/AUTH, unreadable private content, partial discovery, signing races, cancelled callbacks, mutation during publication, and shutdown with work in flight.

Run existing Gnostr `test_mute_filter` and `test_sync_bridge_mute_reload` targets referenced by the curated plan.

### Stage 3: Local account social storage and manager

Land v11, `GhSocialLists`, the storage half of `GhListAdapter`, local list editing, and store-backed Preferences. Sync remains off and publication disabled.

**Tests:**

- New `groundhog-store-social` → `test-groundhog-store-social`.
- New `groundhog-social-lists` → `test-groundhog-social-lists`.
- New `groundhog-lists-ui` → `test-groundhog-lists-ui`.

Cover migration, transaction rollback, bounds, account isolation, local editing without a signer, and no network activity by default.

Register exact targets in `TestInventory.cmake`.

### Stage 4: Private-list sync and kind-10009 migration

Add Groundhog adapters, master switch, auth rules, and shared-service-backed `GhNip29GroupList`.

**Tests:**

- Existing `groundhog-nip29-group-list`.
- Existing `groundhog-group-ui` and `groundhog-group-ui-gui`.
- Existing `groundhog-account-store`.
- Existing `groundhog-auth-policy`.
- New `groundhog-social-sync` → `test-groundhog-social-sync`.

Verify old group Restore behavior, kind 10009 publication unchanged and independent of the switch, no automatic publication of other lists on upgrade, private decryption failure, partial-read confirmation, and account-switch cancellation.
Also test the first-import confirmation for an existing remote kind 10000, restart resume of stored signed events, and majority versus all-source publication.

Charter, static privacy rules, and new account-AUTH purpose changes land atomically with their callers.

### Stage 5: Follow-gated avatars and followed Requests

Implement follow discovery, directory eligibility, centralized picture policy, and all surface wiring.

**Tests:**

- Existing `groundhog-contact-directory`.
- Existing `groundhog-account-ui`.
- Existing `groundhog-conversation-list`.
- Existing `groundhog-conversation-view`.
- Existing `groundhog-requests` / `groundhog-requests-gui`.
- Existing `groundhog-conversation-info`.
- New `groundhog-picture-cache` → `test-groundhog-picture-cache`.

Explicitly test master-off startup with persisted consent and no kind-3 request, follow removal, same-URI stale completion, multi-window revocation, followed Request quarantine, a lookup limited to surfaced followed peers, and no lookup on the send path.

### Stage 6: Hide admission and retained-history projection

Land v12 and the delegate signature change **atomically across every delegate and test initializer**.

**Tests:**

- New `groundhog-message-policy` → `test-groundhog-message-policy`.
- Existing `groundhog-dm-inbox`.
- Existing `groundhog-nip29-service`.
- Existing `groundhog-mls-service`.
- Existing `groundhog-notifier`.
- Existing `groundhog-conversation-info` and `groundhog-requests`.

Verify:

- `p` matches: seen-only admission in all backends, no payload stored.
- `word`, `t`, `e` matches: stored hidden in all backends, and revealed when the entry is removed.
- NIP-04 coverage.
- MLS commits/ratchets continue.
- Valid deletions still apply.
- History unhide without resurrection of discarded arrivals.
- Projection cancellation/restart.
- Hidden-only paging.
- Mixed notification withdrawal.
- `HIDE_RETAIN` leaves unread, title, activity and read state unchanged.
- Search and all four recounts ignore hidden rows.
- Reactions from hidden authors reappear after unhide, including pending ones.
- `p`-only changes use the indexed path with no mask.
- A policy that has not loaded refuses admission.
- No notification replay after unhide.

### Stage 7: Relay denial and Blossom account migration

Wire 10006 into both transport dispatchers and migrate upload destinations.

**Tests:**

- Existing `groundhog-net`.
- Existing `groundhog-relay-guard`.
- Existing `groundhog-dm-send`.
- Existing `groundhog-outbox`.
- Existing `groundhog-blossom`.
- Existing `groundhog-preferences`.

Add cases for live-socket closure, denied AUTH/resend, signer-control exclusion through a real NIP-46 session, NIP-11 denial, all-recipient-relays-denied, recovery using the same wrap, signer-control exclusion, empty destinations, and per-account legacy import.

### Stage 8: NIP-37 private draft sync

Land schema v13 (all of its tables) with opaque identifiers, cadence, import rules, the atomic clear and private-relay AUTH.

**Tests:**

- New `groundhog-draft-sync` → `test-groundhog-draft-sync`.
- New `groundhog-store-drafts` → `test-groundhog-store-drafts`.
- Existing `groundhog-composer`.
- Existing `groundhog-outbox`.
- Existing `groundhog-auth-policy`.
- Existing `groundhog-remote-feature-matrix`.
- Existing `groundhog-account-remote-switch`.

Use two service instances, deterministic clocks, and event-driven fake/local relays. Cover enqueue rollback preserving draft and tombstone, switch flushing, forget tombstones and `forgotten_before`, enable-time reconciliation with an age-unknown draft, no new `d` before EOSE, stored-event resume after restart and account switch, the NIP-46 lane and cadence, chat versus kind 5/7 clears, programmatic-update suppression, concurrent first creation and identifier convergence, clear before a late older revision, expiration, the 64-event import bound, undecryptable events, conflict copies, blocked rooms, remote signer denial, offline operation, and no participant-derived outer identifier or Groundhog-specific tag.

### Stage 9: Reports

Land schema v14. Implement report dialog/service and distinct Hide offer.

**Tests:**

- New `groundhog-report` → `test-groundhog-report`.
- New `groundhog-report-ui` → `test-groundhog-report-ui`.
- Existing `test_nip56`.
- Existing `groundhog-about` to prove NIP-34 issue reporting remains separate.

Inspect serialized private-origin report payloads and destinations—not merely mocked service calls.
Verify a group with no kind-39000 metadata is never treated as public.

### Stage 10: Emoji and badges

Add picker favorites/sets, custom emoji tag emission and consent-gated display, badge graph validation, and explicit profile publication.

**Tests:**

- New `groundhog-emoji` → `test-groundhog-emoji`.
- New `groundhog-badges` → `test-groundhog-badges`.
- Existing `test_nip58`.
- Existing `groundhog-conversation-view`.
- Existing `groundhog-conversation-info`.

Verify no default image fetch, malformed artwork fallback, `emoji` tags sent on reactions and on chat messages in all three backends, forged award chains, unmatched pairs, public confirmation, and legacy-to-canonical publication.

### Stage 11: Release integration

Complete versions, packaging, translations, resource generation, and documentation.

Run:

- `scripts/test-gate.sh`.
- `groundhog-privacy-static`, including checker self-tests.
- `groundhog-blueprint`.
- `groundhog-data`.
- `groundhog-schema`.
- `groundhog-potfiles`.
- `groundhog-i18n`.
- `groundhog-packaging-version`.
- Relevant headless and GUI aliases with their declared capabilities.
- Shared/static and feature-disabled build configurations.
- Leak/sanitizer tests for cancelled account, signer, relay, and image operations.

Before merge, perform the repository-required Nostr protocol-smell review and GNOME HIG review. Tests must advance injected clocks or consume protocol events; no sleep-based synchronization or success-only transport stubs.

## 7. Open questions

Each has a working default, so none blocks starting. Questions 3 and 5 are best settled before Stage 5 and Stage 2 respectively.

1. **Does the sync switch also govern kind 10009?** Default: no. It keeps automatic private publication under its 2026-10-11 charter amendment, because gating it would break group restore for existing users. The alternative is to apply D1 literally and accept that restore stops until the user enables sync.
2. **Should chat messages carry custom-emoji tags?** Default: yes, alongside kind 7 reactions. NIP-30 names kind 7 but not kind 14, so chat-message use is a mild extension; other clients show the literal `:shortcode:`. The alternative is reactions only.
3. **Does the kind-3 read run with `load-profile-pictures` off?** Default: no, so a default install makes no new request. The cost is that "You follow this account" appears only with the switch on. The alternative reads kind 3 always, showing the label on default installs at the cost of one more own-list request.
4. **On a signer that approves each signature, is automatic draft sync wanted?** Default: yes, with the slower NIP-46 cadence in §3.7, and signer denial stops retries. The alternative is a manual **Sync Now** for those accounts.
5. **May one dead relay stop automatic list sync?** Default: no; other lists publish once a majority of sources answer, while kind 10009 keeps its all-sources rule. The alternative is all sources everywhere, which is safer against stale overwrites but can stall sync indefinitely.

## 8. Planning notes

Changes made to the generated specification after checking it against the code and owner decisions:

| Change | Reason |
|---|---|
| Removed the kind 30078 draft manifest, Lamport revisions and device IDs; drafts use standard NIP-37 with NIP-01 ordering | Simpler design with the same requirements. `d=groundhog-drafts-v1-*` would publicly fingerprint the client (charter PD-1, §2.1), and each edit would have cost a second encrypt and sign under NIP-46. What it added was tracking of a room's identifiers across devices; the resulting limit is stated in §3.7 and bounded by expiration |
| Removed unified `composer_drafts`, group drafts and draft reply targets | The request is for DM drafts; AGENTS.md says to keep work finite, and `nostrc-2lpm` already tracks NIP-29 drafts |
| Removed Follow and Unfollow publishing | Not requested. NIP-02 is used only as an avatar gate, and read-only access avoids overwriting a list other clients maintain |
| Split hide outcomes into `DROP` (`p`) and `HIDE_RETAIN` (`word`, `t`, `e`) | D4 specifies dropping for pubkeys only; substring matches could silently and permanently discard wanted messages |
| Kept kind 10009 publication automatic; removed the onboarding change | The cited "group-list opt-in" does not exist: `relay_list_switch` concerns kind 10002. Group publication is automatic today (see Open Question 1) |
| Changed custom emoji from literal text only to sending tags and rendering behind consent | D4 requires real emoji behaviour; literal text alone gives recipients nothing but the shortcode (see Open Question 2) |
| Added explicit source sets for kind 3 and limited kind 0 lookups to surfaced followed peers | The generated spec left these unstated; fetching for a whole follow list would reveal the social graph (PD-12) |
| Applied the design critique (`docs/reviews/groundhog-social-lists-plan-critique-2026-11-08.md`) | Its findings were checked against the code: draft enable and forget safety, draft table shape, `HIDE_RETAIN` admission, search and recounts, destinations and AUTH purposes, NIP-46 exclusion, `READ_ONLY` policy, restart resume and stage dependencies |
| Corrected paths: `nips/generate_cmake.sh`, the Flatpak manifest under `gnome/groundhog/`, and charter line numbers | Verified against the tree |

## References

- NIPs: https://github.com/nostr-protocol/nips (02, 17, 30, 37, 51, 56, 58, 65, B7, CD)
- BUD-03: https://github.com/hzrd149/blossom/blob/master/buds/03.md
- `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`
- `docs/plans/groundhog-alpha6-2026-10-09.md`
- `docs/TESTING.md`, `VERSION_MANIFEST.md`, `AGENTS.md`
