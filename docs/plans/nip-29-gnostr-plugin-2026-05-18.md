# NIP-29 Gnostr Plugin Plan

**Status:** partial — implementation landed as commit `b5362717` (`apps/gnostr/plugins/nip29-groups/`, 4389 LOC) but `ENABLE_NIP29=OFF` by default (`NipOptions.cmake:123`); missing the plan's test harness (5 scenarios), PENDING_JOIN/PENDING_LEAVE UI states, reply composing, kind-10009 discovery, and moderation kinds 9000/9001/9002/9005/9009. Upstream conformance gap tracked in `nostrc-rxxx`.

## Goal

Add optional NIP-29 relay-based group support to `apps/gnostr` as a bundled-but-disabled-by-default GTK4/libadwaita plugin. The implementation should reuse the existing gnostr plugin lifecycle, host signing, relay publishing, GTK extension points, and `nips/nip29` library instead of creating a parallel app architecture.

## Background

- Gnostr plugins are loaded through libpeas, discovered from user/system/dev paths, and enabled via GSettings or `GNOSTR_ENABLE_PLUGINS`; see `apps/gnostr/src/util/gnostr-plugin-manager.c:160` and `apps/gnostr/src/util/gnostr-plugin-manager.c:232`.
- Plugins implement `GnostrPlugin`, optionally `GnostrEventHandler` and `GnostrUIExtension`; sidebar panels and settings pages are already supported by `apps/gnostr/src/gnostr-plugin-api.h:55`, `apps/gnostr/src/gnostr-plugin-api.h:197`, and `apps/gnostr/src/gnostr-plugin-api.h:389`.
- Plugin contexts already expose targeted publishing, relay fetch, local-store query/subscribe, plugin data storage, current pubkey/login state, navigation helpers, and async signing; see `apps/gnostr/src/gnostr-plugin-api.h:781` and `apps/gnostr/src/gnostr-plugin-api.h:918`.
- `apps/gnostr/plugins/mls-groups/` is the richest group-chat plugin precedent, but it is Marmot/MLS-based and handles kinds 443/444/445/1059/10051, not NIP-29 relay groups; see `apps/gnostr/plugins/mls-groups/mls-groups-plugin.c:80` and `apps/gnostr/plugins/mls-groups/mls-groups-plugin.c:220`.
- Existing `nips/nip29/` is stale and narrow: it emits old `public`/`open` tags, misses `restricted`/`hidden`, references typo constants such as `NOSTR_NOSTR_KIND_SIMPLE_GROUP_ADMINS`, and only smoke-tests constructors/permissions; see `nips/nip29/src/nip29.c:42` and `nips/nip29/tests/test_nip29.c:1`.
- Current NIP-29 is relay-authoritative and draft/optional/relay: user events use `h`, relay snapshots use `d`, metadata flags are `private`, `restricted`, `hidden`, `closed`, and member lists can be absent or partial.

## Approach

Build a distinct `apps/gnostr/plugins/nip29-groups/` plugin. Do not refactor `mls-groups` into a shared group-provider layer first: MLS groups and NIP-29 groups share plugin/UI mechanics, but their authority, state, relay, and membership models differ enough that a common abstraction would slow delivery and hide important protocol differences.

Treat `nips/nip29` as the spec-level normalization layer and modernize it before the plugin depends on it. It should parse/merge current NIP-29 snapshot events and own memory safely; GTK state, relay subscriptions, persistence, and interaction flows stay in the plugin.

Add the smallest host API extension needed for correctness: raw relay query/subscription through `GnostrPluginContext`. The current plugin fetch API ingests events into local storage and drops relay provenance, but NIP-29 identity is effectively `(authoritative relay, group id)`. The lower-level pool already has URL-scoped queries (`nostr-gobject/include/nostr-gobject-1.0/nostr_pool.h:149`) and live callbacks with `relay_url` provenance (`nostr-gobject/include/nostr-gobject-1.0/nostr_pool.h:230`, used in `apps/gnostr/src/ui/gnostr-main-window-pool.c:470`), so this should be an additive wrapper rather than a new transport layer.

The first user-facing version should provide one auth-gated sidebar panel, manually tracked groups persisted in plugin-local `saved-groups.json`, group metadata/admin/member/role display, chronological messages for kinds 9/10/11/12, and create/join/leave/send flows. It may read kind `10009` later as a discovery hint, but should not write the user's group list in v1.

## Work Items

1. **Modernize `nips/nip29` against the current spec.**
   - Update `nips/nip29/include/nip29.h` and `nips/nip29/src/nip29.c` to model group address, metadata flags, admins, optional/partial members, and roles separately.
   - Replace legacy `public`/`open` behavior with `private`, `restricted`, `hidden`, and `closed`.
   - Use the existing kind constants in `libnostr/include/nostr-kinds.h:82` and `libnostr/include/nostr-kinds.h:186`; remove typo constants.
   - Add/declare helpers needed by callers, including permission string conversions and a roles snapshot merge function.
   - Ensure merge functions copy strings/arrays from input events instead of borrowing event-owned memory.
   - Expand `nips/nip29/tests/test_nip29.c:1` to cover address parsing, metadata flags, timestamp-based snapshot replacement, roles, partial/absent members, and no legacy `public`/`open` output.

2. **Add raw relay access to the plugin API.**
   - In `apps/gnostr/src/gnostr-plugin-api.h` and `.c`, bump the plugin API minor version additively and introduce a small `GnostrPluginRelayQuery` shape plus async query and live subscription helpers.
   - Wrap `gnostr_pool_query_urls_async()` for one-shot relay-scoped fetches and `gnostr_pool_subscribe_multi()` for live subscriptions, preserving the callback `relay_url` so same-id groups on different relays stay distinct.
   - Do not auto-ingest results into local storage; return raw event JSON to the plugin.
   - Marshal callbacks to the main thread, cancel in-flight work on unsubscribe/context free, and keep existing `request_relay_events_async()` behavior unchanged for current plugins.

3. **Fix optional plugin UI lifecycle seams.**
   - In `apps/gnostr/src/util/gnostr-plugin-manager.c`, track which sidebar panel IDs each plugin contributes and remove them on plugin unload/disable.
   - In `apps/gnostr/src/ui/gnostr-session-view.c:1738`, store/remove the whole plugin separator row and switch back to Timeline if the active plugin panel disappears.
   - Respect `GnostrSidebarItem.position` within the existing “More” plugin section only if needed for deterministic ordering; do not interleave plugin rows with built-in rows.

4. **Scaffold the bundled optional plugin.**
   - Add `apps/gnostr/plugins/nip29-groups/` with CMake, `.plugin` metadata, `nip29-groups-plugin.[ch]`, and a placeholder panel.
   - Implement `GnostrPlugin` + `GnostrUIExtension` only for v1. Avoid `GnostrEventHandler`; the plugin should own relay-provenance subscriptions through the new raw relay API.
   - Contribute one auth-gated sidebar item: `id = "nip29-groups"`, label `Groups`, and a group/chat symbolic icon.
   - Wire the plugin into `apps/gnostr/CMakeLists.txt` as bundled metadata/module output but keep it disabled unless enabled by settings/env.

5. **Build `GnNip29GroupService` as the plugin control plane.**
   - Own tracked groups, per-group messages, raw relay subscription IDs, refresh generations, seen message IDs, current pubkey, and shutdown cancellables.
   - Persist only manually tracked groups through plugin data as `saved-groups.json` with `{version, accounts:{<pubkey>:[{relay_url, group_id, alias, last_opened}]}}`; use an anonymous/local bucket only before login.
   - On identity changes, cancel live subscriptions and clear membership/admin-derived state while keeping saved groups.
   - For each tracked group, query the authoritative relay for snapshots `39000`-`39003` with `#d=<group_id>`, query recent messages `9`-`12` with `#h=<group_id>`, then start matching live subscriptions.
   - Deduplicate messages by event id, sort by `(created_at, id)`, and ignore stale snapshot generations.

6. **Implement the GTK group panel and chat view.**
   - Add GObject models for group items and message items with explicit state for relay URL, group id, metadata flags, snapshot presence, membership/admin state, load state, and last error.
   - Build one list/detail `GnNip29GroupsPanel` and one `GnNip29GroupChatView` with refresh/add/create actions, saved-group restore, metadata badges, chronological messages, composer, inline errors, and profile/thread navigation via plugin context helpers.
   - Treat absent `39002` as “membership unknown,” not “0 members.” Hide admin-only affordances unless `39001`/roles prove them.

7. **Implement user-authored NIP-29 flows.**
   - Create group: kind `9007`, `h` tag, metadata tags/flags, sign through `gnostr_plugin_context_request_sign_event()`, publish only to the chosen relay via targeted publish, keep the dialog state if the relay rejects the event, and save the group only after publish success.
   - Join: kind `9021` with `h` and optional invite `code`; show `PENDING_JOIN` until relay state refreshes.
   - Leave: kind `9022` with `h`; show `PENDING_LEAVE` until relay state refreshes.
   - Send message: v1 sends root chat messages as kind `9` with `h`; render incoming `10/11/12` but defer reply-compose UI.
   - Add timeline `previous` tag support before claiming full NIP-29 compliance; include recent relay-local event references when available and tolerate relays that enforce them.

8. **Verify and document the delivery.**
   - Unit-test `nips/nip29` thoroughly.
   - Add focused tests or harness checks for relay-scoped query separation, unsubscribe cancellation, plugin sidebar add/remove, and same group id on two relays.
   - Cover signer failure, relay rejection, publish failure, missing member/admin snapshots, saved group restore, plugin enable/disable, and live message arrival.
   - Update user-facing docs once implementation exists; this plan remains the execution reference.

## Execution Beads and Buckets

### Bucket 1 — foundation, concurrent

- [x] `nostrc-lwh` — Modernize `nips/nip29` against current NIP-29 semantics. Completed; scoped NIP-29 build/test passed.
- [x] `nostrc-8rw` — Add raw relay access and plugin lifecycle seams in gnostr host/plugin APIs. Completed; default clean build and focused raw relay API build passed.

### Bucket 2 — plugin core, after Bucket 1

- [x] `nostrc-d29` — Scaffold `apps/gnostr/plugins/nip29-groups/` and implement `GnNip29GroupService`. Completed; plugin CMake gating fixed; default clean build and `ENABLE_NIP29=ON` plugin target build passed.

### Bucket 3 — plugin UI, after Bucket 2

- [x] `nostrc-1fp` — Implement GTK group/message models, groups panel, and chat view. Completed; service stale-snapshot reset fixed before closing.

### Bucket 4 — flows and verification, after Bucket 3

- [x] `nostrc-oxo` — Implement create/join/leave/send flows, `previous` tag support, failure handling, and final acceptance verification. Completed; focused syntax/diff checks passed, default clean build passed, and `ENABLE_NIP29=ON` plugin/NIP-29/API targets build.

## Acceptance Checks

- `ENABLE_NIP29=ON` builds and tests the modernized `nips/nip29` library without legacy `public`/`open` semantics.
- `GNOSTR_ENABLE_PLUGINS=nip29-groups` loads the plugin and adds exactly one auth-gated “Groups” sidebar row.
- Disabling/unloading the plugin removes its row and panel; if the panel was active, the session returns to Timeline cleanly.
- Adding `relay_a'general` and `relay_b'general` keeps snapshots, messages, and membership state separate.
- Opening a group uses only its authoritative relay for snapshots/messages and does not rely on generic local storage as source of truth.
- Missing `39002` or `39001` snapshots produce unknown/limited UI states, not false empty/member/admin conclusions.
- Create, join, leave, and send flows sign through the existing signer and publish only to the selected group relay.

## Open Questions

None blocking for the NIP-29 plugin implementation. Repository-wide `ctest` still has unrelated/non-NIP-29 failures tracked in follow-up bead `nostrc-480`; those block the full pre-push quality gate until resolved.

## References

- Current NIP-29 spec: https://github.com/nostr-protocol/nips/blob/master/29.md
- `nostr-tools` NIP-29 helpers: https://github.com/nbd-wtf/nostr-tools/blob/master/nip29.ts
- Go NIP-29 implementation docs: https://pkg.go.dev/fiatjaf.com/nostr/nip29
- `apps/gnostr/src/gnostr-plugin-api.h`
- `apps/gnostr/src/gnostr-plugin-api.c`
- `apps/gnostr/src/util/gnostr-plugin-manager.c`
- `apps/gnostr/src/ui/gnostr-session-view.c`
- `apps/gnostr/src/ui/gnostr-main-window-pool.c`
- `nostr-gobject/include/nostr-gobject-1.0/nostr_pool.h`
- `apps/gnostr/plugins/mls-groups/`
- `apps/gnostr/plugins/nip17-dms/`
- `nips/nip29/`
- `libnostr/include/nostr-kinds.h`
