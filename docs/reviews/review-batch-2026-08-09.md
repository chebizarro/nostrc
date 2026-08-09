# Peer Review — 21-commit batch ahead of `origin/master`

**Date:** 2026-08-09
**Reviewer:** Claude (required peer reviewer per `AGENTS.md` pre-push policy)
**Range:** `origin/master..HEAD` (21 commits, 66 files, +3519/−2986)
**Build/test status as submitted:** ctest 285/285 pass (1 skipped), build green on macOS.

---

## Scope

Reviewed the full `git diff origin/master...HEAD` with emphasis on the priority files
named in the review request. Review dimensions: correctness, GObject refcounting and
memory safety, race conditions, error-handling gaps, and pattern consistency.

Every load-bearing claim below was verified by reading the full surrounding function,
not just the diff hunk. Sub-agent probe output was spot-checked; one probe's threading
claims were **rejected on verification** (see [Rejected findings](#rejected-findings)).

---

## Verdict summary

No blocking defects. The high-risk areas — transactional signet revocation, the
`go_select` wake race, and profile-service ownership — are correct, well-reasoned, and
backed by targeted regression tests. Findings below are all low severity and suitable
as follow-up beads rather than pre-push blockers.

| # | Severity | Area | Finding |
|---|----------|------|---------|
| 1 | Low | timeline-view-app-factory | Stale idle-source ID on two early-return paths |
| 2 | Low | gnostr-login | `Retry` is not idempotent w.r.t. the NIP-46 listener |
| 3 | Low | gnostr-login | `ctx->self` dereferenced without the guard used everywhere else |
| 4 | Low | signet/revocation | Handle-mismatch precondition fails closed with no diagnostic |
| 5 | Low | gnostr-media-service | `disk_root` read outside `media_disk` lock |
| 6 | Low | settings dialog | Missing builder ID binds silently to nothing |
| 7 | Low | testkit | New `NULL` return path is unchecked by all callers |
| 8 | Info | timeline-view | New type guard has zero call sites |
| 9 | Info | timeline-hydrator | Profile refs are dropped, not bounded (verified safe) |

---

## Findings by area

### signet — transactional revocation (`revocation.c`, `key_store.c`) ✅

`signet_revoke_internal` is now a genuine single-transaction operation:
`BEGIN IMMEDIATE` → deny-list insert → lease burn → client-binding revoke →
agent-row delete → `COMMIT`, with hot-cache publication (`signet_deny_list_cache_add`,
`signet_key_store_evict_agent`) deferred until after durable success. This is the
correct ordering and cleanly fixes the previous split-brain failure mode.

Verified:

- **No nested transactions.** `signet_store_revoke_agent_leases`
  (`store_leases.c:118`), `signet_store_revoke_agent_clients` (`store.c:1866`), and
  `signet_store_delete_agent` (`store.c:1511`) are all bare prepare/step/finalize with
  no internal `BEGIN`. `signet_audit_log_append` *does* open its own transaction
  (`store_audit.c:67`) but runs after `COMMIT`, outside the critical section.
- **Lock ordering is safe.** `sqlite3_mutex_leave` happens at `done:` before
  `signet_key_store_evict_agent` takes `ks->mu`, so the db-mutex → `ks->mu` order is
  never inverted. `sqlite3_db_mutex()` returning `NULL` in NOMUTEX builds is handled
  (`sqlite3_mutex_enter(NULL)` is a documented no-op).
- **Caller-held transactions are not corrupted.** A failing `BEGIN IMMEDIATE` jumps to
  `done:` rather than `rollback:`, so an outer transaction is left intact.
- **`signet_key_store_revoke_agent` reordering is correct.** Store delete first, cache
  eviction second, with an early `return -1` under the lock on store failure — the key
  is never wiped while a live row remains. The `(!found && store_rc == 1) → 1`
  not-found semantics are preserved.

`signet/tests/test_store_lifecycle.c` deserves specific credit: the forced-failure
test installs a `TEMP TRIGGER` that aborts the `agent_clients` update mid-transaction
and then asserts the deny list, leases, *and* client bindings all rolled back. That is
exactly the right test for this change.

**Finding 4 (Low).** `signet/src/revocation.c:180-184` — the new precondition rejects a
deny list or key store that does not share the *exact* `sqlite3*` handle. All in-tree
callers satisfy it: `mgmt_protocol.c:1409` passes
`signet_key_store_get_store(h->keys)` as `store`, and `signetd_main.c:815` builds the
deny list from that same `base_store`. But on mismatch the function returns `-1`
before any state changes and before any audit record, and the mgmt layer surfaces only
a generic `"revoke_failed"`. For a security-critical operation, a silent hard-fail that
leaves the agent live is worth a `g_warning` naming the mismatched component.

### signet — management reply transport (`mgmt_protocol.c`) ✅

The legacy kind-28090 encrypted-ack path is reinstated alongside the ContextVM
gift-wrap path, selected per-invocation by `SignetMgmtReplyTransport`. Public
`signet_mgmt_handler_handle_request` keeps its ABI and defaults to
`SIGNET_MGMT_REPLY_LEGACY_ACK`; the intent path threads
`SIGNET_MGMT_REPLY_CONTEXTVM` through.

Ownership verified against the actual setter implementations:

- `nostr_event_set_content` (`libnostr/src/event.c:853`) **strdups**, so the subsequent
  `secure_wipe` + `free(encrypted_content)` is correct and not a double free.
- `nostr_event_set_tags` (`libnostr/src/event.c:841`) **takes ownership**, so the
  `NostrTags` is released by `nostr_event_free`. Correct.
- The `sk` scratch buffer is wiped on every path; `pk` is public and needs no wipe.
- Fail-closed on encryption failure (warn + drop, never publish plaintext) is the right
  call for a management channel.

### libgo — `go_select` shutdown wake race (`select.c`) ✅

Two distinct bugs, both correctly fixed:

1. **Registration-after-close.** `go_channel_register_select_waiter` now checks
   `chan->closed` under `chan->mutex` and publishes the terminal wakeup directly
   instead of linking a waiter onto a channel that will never transition again. The
   early return adds no node and takes no waiter ref, so the later
   `go_channel_unregister_select_waiter` no-op leaves refcounts balanced.
2. **Lost condition-variable wake.** `go_channel_signal_select_waiters` now holds
   `w->mutex` across both the `signaled` store and the `cv_signal`, closing the window
   where a waiter observes `signaled == 0` and then sleeps after the signal fired. The
   old "cv_signal is safe without the mutex" comment was the actual defect: it is safe
   for the *signal*, but not for the *predicate transition*.

Lock order is uniformly `chan->mutex` → `w->mutex` in both the register and signal
paths, so the previously-feared ABBA cycle does not exist. The replacement comment
documents this accurately.

`libgo/tests/go_select_closed_test.c` covers both orderings across 500 iterations with
a `sched_yield()` on alternating passes to shuffle the interleaving — a real race
regression test, not a smoke test.

### nostr-gobject — profile service ownership (`nostr_profile_service.c`) ✅

The service is now atomically refcounted, with strong references held by every
in-flight `BatchFetchCtx` and every debounce timeout (via `schedule_debounce`'s
`g_timeout_add_full` + `GDestroyNotify`). `shutdown()` detaches the singleton, fires a
terminal `NULL` callback for every pending key, and drops its own ref — final
destruction is deferred until in-flight work drains. This is the correct pattern and
directly removes the previous "free the service while a pool query is outstanding" UAF.

Verified in detail:

- `on_profiles_fetched` now calls `dispatch_next_batch(svc)` **before**
  `profile_service_unref(ctx->svc)`, so the service cannot be destroyed mid-dispatch.
- `profile_meta_copy` covers all nine fields of `GnostrProfileMeta`
  (`nostr_profile_provider.h:24-34`) and matches `gnostr_profile_meta_free`
  (`nostr_profile_provider.c:412`) exactly. No silent field loss.
- `request_gtask_finish` correctly flips to `(transfer full)`; the bridge returns an
  owned copy with `gnostr_profile_meta_free` as the destroy notify. There are **no
  out-of-tree callers** of the gtask API to break.
- `set_pool` now refs-before-unref *and* sets `owns_pool = TRUE` for injected pools.
  This is consistent with the internal-creation path (`:555-557`) and fixes a real
  leak: the old code called `g_object_ref(pool)` but set `owns_pool = FALSE`, so the
  reference was never released.
- `dispatch_next_batch` checks `svc->shutdown` first (`:454`), and
  `debounce_timeout_cb` clears `debounce_source_id` and bails on shutdown (`:613-618`),
  so no post-shutdown timeout can resurrect work.
- `complete_request`'s retry path is now gated on `!svc->shutdown`, preventing an
  infinite reschedule during teardown.

The three new tests (`gtask-owned-copy`, `gtask-shutdown-completion`,
`gtask-invalid-error`) pin the ownership contract, the terminal-completion guarantee,
and the new argument validation respectively. Good coverage.

### apps/gnostr — media service rework and avatar cache migration ✅

`gnostr_media_service_evict_account` is the notable new surface and it is written with
unusually careful attention to reentrancy: the namespace epoch is advanced *before*
cancellation or deletion so detached disk jobs cannot recreate the namespace, and
pending requests are snapshotted into a strong-ref array with the explicit comment that
"a subscriber callback may recursively evict the account." That is the right defensive
posture.

The privacy policy did **not** regress. `load_media_image_internal` dropped its local
`remote_media_loading_enabled()` gate for the injected-loader path, but `user_initiated`
is still forwarded to the loader (`nostr-note-card-row.c:2863`) and the media service
enforces the policy at both entry points — `reject_common` (`:3143`) and
`start_request_download` (`:3042`) — via `gnostr_media_fetch_intent_is_allowed`
(`utils.c:328`), which maps `GNOSTR_MEDIA_FETCH_AUTOMATIC` through
`gnostr_is_remote_media_allowed()`. Enforcement simply moved to a single chokepoint.

The `remote_media_loading_enabled` rewrite to `g_once_init_enter` additionally requires
the `load-remote-media` key to exist before constructing the `GSettings`, and returns
`FALSE` when it does not — fail-closed, correct.

**Finding 5 (Low).** `gnostr-media-service.c:3634` — `self->disk_root` is read to build
`namespace_dir` *before* `G_LOCK(media_disk)` is taken, while
`gnostr_media_service_test_set_disk_root` frees and reassigns it under that same lock.
Only the test-only setter mutates it, so this is not reachable in production, but
moving the `g_build_filename` inside the lock costs nothing.

### apps/gnostr — SoupSession ownership migration ✅

`gnostr_get_shared_soup_session` correctly flips to `(transfer full)`, and the
migration is **complete**. All 15 call sites were audited individually and every one
uses `g_autoptr(SoupSession)`:

`main_app.c:409`, `gnostr-emoji-content.c:167`, `gnostr-article-card.c:616`,
`gnostr-image-viewer.c:850`, `gnostr-picture-card.c:1034`, `gnostr-article-reader.c:276`,
`custom_emoji.c:527`, `relay_info.c:408`, `zap.c:415`, `zap.c:695`, `dm_files.c:722`,
`gnostr-media-service.c:3014`, `nostr-note-card-row.c:6511`, `nostr-note-card-row.c:6880`,
`gnostr-profile-pane.c:2584`.

The companion `gnostr-nip05.c` change (module now holds a strong ref via `g_set_object`)
is paired correctly with the new `gnostr_nip05_set_soup_session(NULL)` call in
`on_shutdown` — released *before* `gnostr_cleanup_shared_soup_session()`, in the right
order. `gnostr-note-embed.c` likewise upgrades `external_cancellable` to a strong ref
with a matching `g_clear_object` in `dispose`.

### nostr-gtk — note card reservation/activation split ✅

The `NostrGtkMediaTextureLoader` refcounted interface is a clean improvement over the
previous "borrowed pointer that must outlive the row" contract. `load_media_image_internal`
takes a local ref across the async request dispatch, which is the correct guard against
the loader being swapped mid-flight. `dispose` releases the row's ref.

Reservation state (`rich_media_reserved_height`, `rich_link_preview_reserved_height`,
`rich_embed_reserved_height`, `rich_content_activated`) is set in
`reserve_rich_content` and reset in the bind-preparation path, so no state leaks across
row recycles. `GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF` is filtered out of the
`eligible` array in **both** the reserve loop (`:4531`) and the activate loop (`:4662`),
which is what makes the `g_assert_not_reached()` in the frame-assignment loop (`:4620`)
and in `start_rich_slot_realization` (`:4415`) genuinely unreachable.

One behavioural note worth keeping in mind (not a defect): `activate_rich_content`
bails with a `g_warning` when `rich_frame_pool_is_compatible` fails. The user-visible
consequence is empty reserved slots rather than a crash — a reasonable failure mode,
but the warning is the only signal, so it is worth watching for in logs.

### apps/gnostr — timeline view app factory ⚠️

**Finding 1 (Low).** `apps/gnostr/src/ui/gnostr-timeline-view-app-factory.c:715-724` —
`activate_snapshot_rich_content_idle` clears `tv-rich-activation-idle-id` at line 724,
but two early returns execute *before* that clear:

- `if (!GTK_IS_LIST_ITEM(list_item)) return G_SOURCE_REMOVE;`
- `if (!NOSTR_GTK_IS_NOTE_CARD_ROW(row)) return G_SOURCE_REMOVE;`

If either fires, the source has been dispatched and destroyed but the row still holds
its (now dead) ID. A later `cleanup_bound_row` (`:489-493`) then calls
`g_source_remove(rich_idle_id)` on it.

*Failure scenario:* the list item's child is unset or replaced without an intervening
unbind, the idle fires and returns at the second check, then unbind runs
`cleanup_bound_row` → `g_source_remove(stale_id)` → `GLib-CRITICAL: Source ID N was not
found`. If GLib has recycled that ID for an unrelated source, that source is silently
removed instead. Narrow, but the fix is trivial: clear the data key as the first
statement of the callback, before any early return.

### apps/gnostr — login retry UAF ⚠️

The core fix is right. Transferring `NostrNip46Session` ownership into the task context
(`ctx->session`) removes the dangling read of `self->nip46_session` from the worker
thread, `g_task_return_error_if_cancelled` short-circuits the RPC, and the new
`G_IO_ERROR_CANCELLED` branch frees the context cleanly. `g_task_set_task_data(task,
task_ctx, NULL)` with a `NULL` destroy notify is deliberate and correct — `task_ctx` is
freed exactly once, in `on_get_pubkey_done`, with no double-free.

**Finding 2 (Low).** `gnostr-login.c:1131` — `on_retry_bunker_clicked` now cancels the
in-flight operation, but the cancelled completion branch it enables (`:412-416`)
deliberately skips `stop_nip46_listener()`, and `set_bunker_status(BUNKER_STATUS_IDLE)`
does not stop it either. `start_nip46_listener` (`:1777`) then hits its
`"Already listening for response"` guard and returns without subscribing.

*Failure scenario:* any path that reaches `BUNKER_STATUS_ERROR` while the listener is
still running leaves `Retry` generating a fresh `nostrconnect` URI and QR code with no
subscription behind it — the UI shows "Waiting for approval..." forever. Today the
retry button is only visible in the ERROR state (`:226`), and the primary error path
does call `stop_nip46_listener`, so this is not currently reachable via the normal
flow. Making `on_retry_bunker_clicked` call `stop_nip46_listener(self)` before
`start_nip46_listener(self)` would make retry unconditionally idempotent.

**Finding 3 (Low).** `gnostr-login.c:436` — `GnostrLogin *self = ctx->self;` is
immediately dereferenced (`self->nip46_session = ctx->session;`) without the
`ctx->self && GNOSTR_IS_LOGIN(ctx->self)` guard that every other use in this function
applies (`:420`, `:427`). `ctx->self` is always a strong ref today so this is safe in
practice, but the inconsistency is exactly the kind that rots. Separately, if
`self->nip46_session` were already non-`NULL` (overlapping connect attempts) the
previous session leaks — a `g_clear_pointer` before the assignment would close that.

### apps/gnostr — account removal UI ✅

`gnostr_main_window_on_account_remove_requested_internal` orders operations correctly:
persist the settings change, then delete the keystore entry, and **restore the previous
`known-accounts` list if the keystore delete fails**. That rollback is the right
instinct and is easy to get wrong. The media-service namespace eviction and account-list
refresh follow, and logout is triggered only when the removed account is current.

The session-view side is sound: `AccountRemoveDialogContext` holds a strong ref on the
view, the response index (`1` = Remove) matches the `{Cancel, Remove}` button array,
`set_cancel_button(0)` / `set_default_button(0)` make dismissal safe, and the compound
string literal passed to `gtk_alert_dialog_set_buttons` is copied by GTK. The new signal
is declared in the header, registered in `class_init`, connected in
`gnostr-main-window-signals.c:43`, and the handler is declared in
`gnostr-main-window-private.h:316` — the full wiring is present.

### libmarmot — commit cleanup leak (`mls_group.c`) ✅

The conversion of ~15 scattered `return` statements into `goto fail_wire_msg_{memory,
internal}` is correct, and critically the two variables the new epilogue touches are
both live at every jump site:

- `pre_gc` is declared at `:1231` and assigned at `:1236`; the earliest `goto` is at
  `:1331`. No jump skips its initialization.
- `rc` is declared at `:1240`.
- `wire_msg` is populated by `begin_commit_public_message` at `:1322`, before the first
  `goto` — so `mls_message_clear(&wire_msg)` never touches uninitialised storage.

No double frees: the success path frees `pre_gc` at `:1581`/`:1603` and returns before
reaching the epilogue, and the `:1573-1575` failure path frees `pre_gc` inline without
jumping. The change genuinely adds the previously-missing `mls_message_clear` and
`free(pre_gc)` to the later failure paths.

### Remaining areas ✅

- **NIP-90 removal** — `nip90_dvm.{c,h}` (−1525 lines) and the three `NOSTR_KIND_JOB_*`
  defines are gone with **no dangling references** anywhere in `apps/`, `libnostr/`, or
  `nostr-gtk/`. Clean removal.
- **Cache budget settings** — the new `avatar-cache-max-mb` key is defined in the
  gschema, and all four spin-button IDs (`w_inline_cache_max_mb`,
  `w_og_image_cache_max_mb`, `w_video_poster_cache_max_mb`, `w_avatar_cache_max_mb`)
  exist in **both** the `.blp` source and the generated `.ui`. `cache_prune.c` was
  updated consistently to read the new key with the new 8 MB default.
- **`og-preview-widget.c`** — the extracted `connect_external_cancellable` correctly
  fixes the same-URL early-return path that previously left a replacement parent
  cancellable unconnected. No double-connect: the caller disconnects first.
- **CI** — removing the manual `cp -a build/libjson/libnostr_json.so*` from the AppImage
  job is the right cleanup now that install rules cover it.

**Finding 6 (Low).** `gnostr-main-window-settings.c:748` —
`settings_dialog_bind_media_budget` silently no-ops when
`gtk_builder_get_object` misses. A future typo'd or renamed widget ID produces a
setting that appears in the schema, appears in the UI, and quietly does nothing. A
`g_warning` in the `else` branch would catch it at first run.

**Finding 7 (Low).** `tests/testkit/gnostr-testkit.c:199` —
`gn_test_make_event_json_with_pubkey` gained a `return NULL` path (canonical-ID
computation failure). None of its ~11 callers in
`apps/gnostr/tests/test_event_model_windowing.c` or
`apps/gnostr/tests/integ/test_model_delete_authorization.c` check for `NULL`; they
assign straight into `g_autofree char *` and pass it on. A regression in
`nostr_event_compute_id` would surface as a confusing `NULL` deref rather than a clear
assertion. Test-only, but a `g_assert_nonnull` at the call sites (or inside the helper)
would be cheap.

**Finding 8 (Info).** `nostr-gtk/src/gnostr-timeline-view.c:455` — the new item-type
`g_return_if_fail` guards `nostr_gtk_timeline_view_set_tree_roots`, which has **zero
call sites** in the repository (confirmed independently; also noted in
`docs/reviews/review-nostrc-timeline-view-followups.md:69`). The guard is therefore
untested and unexercised. Worth noting that it would reject a model whose declared item
type is `G_TYPE_OBJECT` or `GTK_TYPE_TREE_LIST_ROW` even if it contains valid
`TimelineItem`s — relevant if a `GtkTreeListModel` is ever wired in here.

**Finding 9 (Info).** `apps/gnostr/src/model/gnostr-timeline-hydrator.c:186` — the
commit is titled "bound" profile mention retention, but the implementation **drops all**
`NOSTR_PROFILE_REF` descriptors (and does not count them toward `overflow_count`).
Verified safe: profile mentions render as `nostr:` anchors from the content-renderer
markup path, not from the descriptor-based rich-frame path, and the note card filters
these descriptors out at both `:4531` and `:4662` regardless. The new test
`test_many_profile_mentions_keep_markup_without_descriptors` asserts exactly this
(`descriptors->len == 0`, `overflow_count == 0`, `anchor_count == mention_count`).
Flagged only because the commit message implies bounding rather than elimination.

---

## Rejected findings

An automated probe reported four "critical thread-safety violations" in
`gnostr-media-service.c`, three of which rested on the claim that `decode_texture_done`
runs on a worker thread. **This is incorrect.** `decode_texture_done` is the
`GAsyncReadyCallback` passed to `g_task_new` at `:1530`; `g_task_run_in_thread`
(`:1533`) runs only `decode_texture_worker` off-thread, while the callback is dispatched
in the main context that was thread-default at task creation. The alleged races on
`self->pending`, the texture/OG/negative LRUs, and the GTK calls in
`subscriber_complete_texture` therefore do not exist. Recording this so the claims are
not resurrected by a future review.

The fourth probe finding (`disk_root`) survived verification in reduced form and is
carried above as Finding 5.

---

## Recommendations

None of the findings block this push. Suggested follow-up beads, in priority order:

1. Clear `tv-rich-activation-idle-id` at the top of
   `activate_snapshot_rich_content_idle` (Finding 1).
2. Call `stop_nip46_listener` in `on_retry_bunker_clicked` to make retry idempotent
   (Finding 2).
3. Apply the `GNOSTR_IS_LOGIN` guard and `g_clear_pointer` at `gnostr-login.c:436`
   (Finding 3).
4. Add a `g_warning` when `signet_revoke_internal` rejects a mismatched store handle
   (Finding 4).
5. Minor hardening: `disk_root` under lock (5), builder-ID warning (6), testkit
   `NULL` assertions (7).

Housekeeping: `signet/src/revocation.c.bak` exists on disk and is git-ignored. It is
not part of this diff, but a stale `.bak` of a security-critical file next to the real
one is an invitation to confusion — worth deleting.

---

## Notable strengths

- The signet revocation rollback test (forced `TEMP TRIGGER` abort mid-transaction) and
  the `go_select` 500-iteration race test are both genuine regression tests for the
  exact failure modes being fixed, not smoke tests. This is the standard the rest of
  the repo should be held to.
- The `SoupSession` `transfer none` → `transfer full` migration was applied to every
  one of 15 call sites with no misses, and the paired `nip05` shutdown ordering was
  handled correctly. Complete API-contract migrations are rare; this one is clean.
- `gnostr_media_service_evict_account`'s explicit reasoning about epoch-before-delete
  and recursive-eviction reentrancy is exactly the kind of comment that prevents the
  next regression.
- The `profile_service` refcount conversion addresses the historical failure mode of
  this codebase (teardown-vs-in-flight-callback UAF) structurally rather than with
  another guard flag.

---

## Verdict

`APPROVED`
