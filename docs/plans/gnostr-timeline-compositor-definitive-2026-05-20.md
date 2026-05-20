# GNostr Timeline Compositor: Definitive Implementation Plan

## Goal

Deliver a timeline that behaves as a stable spatial reading surface. The user's viewport must never move, rows must never expand, and no async/background operation may mutate visible geometry — unless the user explicitly causes it. The current implementation has the architectural pieces (feed controller, snapshot model, hydrator, geometry resolver, batch/patch pipeline) but fails to enforce the core invariants at every layer boundary, producing visible jitter, row expansion, scroll fighting, and degraded rich content.

This plan specifies exactly what must change, file by file, to close every violation. It is a targeted refactor of existing code, not a new subsystem.

## Background

### Architecture (keep as-is)

The existing compositor object model is sound and should be preserved:

- **`GnostrTimelineFeedController`** — compositor orchestrator. Owns working set, pending head, deferred patches, geometry cache. Ingests 7 batch kinds, composes snapshots, publishes through snapshot model. Emits `pending-count-changed`, `restore-scroll`, `snapshot-published`, `need-profile`. (`apps/gnostr/src/ui/gnostr-timeline-feed-controller.c`)
- **`GnostrTimelineSnapshotModel`** — `GListModel` wrapper over immutable snapshot. (`apps/gnostr/src/model/gnostr-timeline-snapshot-model.c`)
- **`GnostrTimelineSnapshot` / `GnostrTimelineSnapshotRow`** — immutable row/snapshot structures carrying geometry fields and prefix-top index. (`apps/gnostr/src/model/gnostr-timeline-snapshot.c`)
- **`GnostrTimelineItemViewModel`** — broad immutable VM (~90 fields). Copy helpers: `copy_with_profile()`, `copy_with_interactions()`. (`apps/gnostr/src/model/gnostr-timeline-item-view-model.c`)
- **`GnostrTimelineHydrator`** — transforms batch entries → VMs with rendered markup, token arrays, reservation counts. (`apps/gnostr/src/model/gnostr-timeline-hydrator.c`)
- **`GnostrTimelineGeometryResolver`** — layout signatures, width-bucket cache, measurement cache. Effective height only grows from measured height when `explicit_expanded` is true. (`apps/gnostr/src/model/gnostr-timeline-geometry.c`)
- **Row factory** — `GtkSignalListItemFactory` with snapshot and legacy bind paths. (`apps/gnostr/src/ui/gnostr-timeline-view-app-factory.c`)
- **`NostrGtkNoteCardRow`** — row widget with `measure()` override that locks height to `reserved_height`. Has both expanding `set_content_rendered()` path and inert `set_media_urls_reserved()`/`set_link_preview_urls_reserved()` paths. (`nostr-gtk/src/nostr-note-card-row.c`)

### Critical failures (7 violations to close)

1. **Full-nuke snapshot replacement**: `replace_snapshot()` emits `items_changed(0, old_len, new_len)` on EVERY compose. Even a single metadata patch forces GTK to teardown+rebind all visible rows. (`gnostr-timeline-snapshot-model.c:89-108`)

2. **No row recycling**: `factory_setup_cb` is empty (`gnostr-timeline-view-app-factory.c:254-258`); `factory_bind_cb` creates a new `NostrGtkNoteCardRow` per bind (`gnostr-timeline-view-app-factory.c:805-806`). Setup should create the widget; bind should only update data.

3. **Scroll restore feedback loop**: Controller `restore-scroll` → `gtk_adjustment_set_value()` (`gnostr-main-window-timeline.c:215`) → `value-changed` → `set_viewport()` + `set_user_at_top()` → may trigger deferred patch reevaluation → another compose → another `restore-scroll`. No re-entrancy guard.

4. **Coarse geometry estimates**: Media `count × 240px`, link `count × 120px` (`gnostr-timeline-hydrator.c:459-460`) produce wrong reserved heights, creating spacing artifacts.

5. **VM copy helpers don't recalculate geometry**: `copy_with_profile()` and `copy_with_interactions()` produce replacement VMs with stale `geometry_signature` and `initial_reserved_height` from original hydration. (`gnostr-timeline-item-view-model.c:369-405`)

6. **Width-bucket changes trigger full recomposition**: Window resize → `schedule_compose(TRUE, FALSE)` (`gnostr-timeline-feed-controller.c:1279-1280`) → full snapshot nuke → all rows torn down and rebuilt.

7. **Every compose with `preserve_anchor=true` emits `restore-scroll`**: Profile patches, metadata patches, deferred admissions, pagination all schedule `preserve_anchor=true`, meaning every background update emits scroll restoration that feeds back. (`gnostr-timeline-feed-controller.c:541-551`)

### Hard constraints

- `GListModel` contract: `items_changed` emissions must match actual incremental model changes.
- Snapshot rows remain immutable after publication.
- Geometry measurements stay GTK main-thread driven.
- No visible row expansion from passive async work.
- Main feed uses snapshot rows; legacy `GnNostrEventItem` path remains for non-migrated consumers (calendar events view, etc.).
- No persistence or serialized schema changes.
- Geometry cache is in-memory only.

### Existing test coverage

`apps/gnostr/tests/test_timeline_feed_controller.c`: 20 tests covering snapshot publication, pending head, anchor preservation, patch admission, geometry measurement, deferred replacement, delete targets, VM reservation fields. Good model-level coverage but no GTK rendering/binding tests, no `items_changed` span assertions, no `restore-scroll` policy assertions.

### Prior art

- `docs/designs/gnostr-timeline-compositor.md` — target architecture contract.
- `docs/plans/gnostr-timeline-compositor-full-implementation.md` — previous incomplete plan; acknowledged migration was not done.

## Approach

Tighten the contracts at three unstable boundaries:

**A. Snapshot publication** — Make `GnostrTimelineSnapshotModel` diff old vs new snapshots by event identity and emit targeted `items_changed` spans instead of full-range nukes. The controller must reuse old `GnostrTimelineSnapshotRow` objects when published state is unchanged, so the diff recognizes pointer-identical rows as no-ops. (Alternative considered: keep full-range replace but make bind idempotent. Rejected because GTK's `GListModel` contract means full-range `items_changed` always triggers teardown/rebind of every visible row, regardless of bind idempotency — the model emission granularity is the root cause.)

**B. Compose scroll policy** — Replace the boolean `preserve_anchor`/`scroll_to_top` pair with a three-valued scroll policy enum (`KEEP_VIEWPORT`, `RESTORE_ANCHOR`, `SCROLL_TO_TOP`). Map each trigger to the correct policy. Background patches and width-bucket changes use `KEEP_VIEWPORT` (no `restore-scroll` emission). Add a re-entrancy guard on the main-window adjustment write so `restore-scroll` → `value-changed` → `set_viewport` feedback is blocked.

**C. Geometry derivation** — Centralize reservation/signature computation into one shared helper callable by the hydrator and both VM copy helpers, so replacement VMs carry correct geometry. Extend the geometry resolver to compute width-aware media/link placeholder sizes and publish them on snapshot rows, so the factory reads current-width reserved heights from the snapshot row rather than stale VM-baked constants. (Alternative considered: use one fixed row height for all cards. Rejected because it produces obvious wasted space on short notes and clipped content on long notes, creating a worse UX regression than the spacing artifacts it would solve.)

**D. GTK row recycling** — Move row creation to `factory_setup_cb()`, split snapshot vs legacy bind helpers, and ensure `NostrGtkNoteCardRow` fully resets reusable state in `prepare_for_unbind()`.

## Work Items

### Item 1 — Extend geometry footprint and snapshot row surface

**Goal:** Add width-aware inner placeholder geometry fields to the data structures so downstream changes can consume them.

**Done when:**
- `GnostrTimelineRowFootprint` has `media_reserved_height` and `link_preview_reserved_height` fields.
- `GnostrTimelineSnapshotRow` has immutable published `media_reserved_height` and `link_preview_reserved_height` with getters.
- `gnostr_timeline_snapshot_row_new_from_view_model()` accepts the two new heights from footprint.
- All existing call sites updated; existing tests pass unchanged.

**Key files:**
- `apps/gnostr/src/model/gnostr-timeline-geometry.h` — extend `GnostrTimelineRowFootprint`
- `apps/gnostr/src/model/gnostr-timeline-snapshot.h` / `.c` — add row fields, constructors, getters
- `apps/gnostr/src/ui/gnostr-timeline-feed-controller.c` — update `compose_snapshot()` call site

**Dependencies:** None (foundation layer).

**Size:** Small — struct additions, constructor parameter threading, getter additions.

---

### Item 2 — Centralize VM derived geometry fields

**Goal:** Eliminate stale geometry signatures in replacement VMs by unifying reservation/signature computation.

**Done when:**
- A shared `gnostr_timeline_item_view_model_spec_recompute_derived_fields()` helper exists.
- It computes: `avatar_state`, all reservation counts/heights, `initial_reserved_height`, `geometry_signature`.
- `gnostr_timeline_hydrator_hydrate_entry()` calls it instead of inline math.
- `copy_with_profile()` and `copy_with_interactions()` call it after applying overrides.
- Hydrator constants for media/link reserved heights are removed; the helper owns them.
- Existing tests pass; new test asserts that `copy_with_profile()` on a row with media produces a correct geometry signature containing the media marker.
- New test asserts that `copy_with_interactions()` produces a VM with recalculated `initial_reserved_height`.

**Key files:**
- `apps/gnostr/src/model/gnostr-timeline-item-view-model.h` / `.c` — add helper, call from copy methods
- `apps/gnostr/src/model/gnostr-timeline-hydrator.c` — remove duplicated math, call helper

**Dependencies:** None (can land independently).

**Size:** Medium — logic extraction, call-site migration, test additions.

---

### Item 3 — Width-aware geometry resolver and layout signature v2

**Goal:** Make the geometry resolver produce width-dependent placeholder heights and stop treating VM signature as the final cache signature.

**Done when:**
- Layout version bumped to `timeline-geometry-v2` (invalidates old measurement cache entries automatically).
- `gnostr_timeline_geometry_dup_layout_signature()` always builds the final signature from: version + VM semantic signature + `has_profile` + moderation/content-warning flags + media/link reservation flags + footer flag. Does NOT short-circuit to `input->geometry_signature`.
- Content-area width inset (used to derive `content_width` from `width_bucket`) must be derived from the actual row template layout, not hardcoded. Check `NostrGtkNoteCardRow` template margins at implementation time.
- `gnostr_timeline_geometry_resolver_resolve()` computes width-aware `media_reserved_height` and `link_preview_reserved_height` in the output footprint. The exact formulas (aspect ratios, clamp ranges, multi-image grid rules) are implementation choices — the contract is: the resolver produces deterministic, width-dependent placeholder heights that the factory reads from the snapshot row. Quantization reuses the existing `quantize_height()` (8px quantum, `gnostr-timeline-geometry.c:59-69`).
- Controller `compose_snapshot()` passes resolver-output reserved heights into snapshot row constructors.
- Existing tests pass; new test asserts that width-bucket change produces different published media reserved heights.
- New test asserts that layout signature v2 does not short-circuit to VM signature.

**Key files:**
- `apps/gnostr/src/model/gnostr-timeline-geometry.h` / `.c` — signature v2, width-aware sizing
- `apps/gnostr/src/ui/gnostr-timeline-feed-controller.c` — pass footprint heights to row constructors

**Dependencies:** Item 1 (footprint struct must be extended first).

**Size:** Medium — formula implementation, signature refactor, controller integration.

---

### Item 4 — Controller compose scroll policy and row reuse

**Goal:** Stop emitting `restore-scroll` for background patches and width changes. Enable snapshot-model diffing by reusing row objects.

**Done when:**
- Internal `GnostrComposeScrollPolicy` enum replaces `scheduled_preserve_anchor` + `scheduled_scroll_to_top`. Merge rule: `SCROLL_TO_TOP` > `RESTORE_ANCHOR` > `KEEP_VIEWPORT`.
- Trigger mapping enforced:
  - Refresh / pending-head admit to top / live-head at top → `SCROLL_TO_TOP`
  - Older/newer pagination / visible delete → `RESTORE_ANCHOR`
  - Metadata/profile patch / deferred admission / width-bucket change → `KEEP_VIEWPORT`
- `compose_and_publish()` with `KEEP_VIEWPORT` never emits `restore-scroll`.
- `compose_snapshot()` reuses old `GnostrTimelineSnapshotRow` objects when: same VM pointer, same estimated/measured/effective heights, same width bucket, same layout signature, same published media/link reserved heights. (Note: `copy_with_profile()` and `copy_with_interactions()` always produce new VM objects, so patched rows always fail the pointer check and get rebuilt — this is correct and intentional; do not weaken the check.)
- Existing tests pass; new tests assert: (a) metadata patch while scrolled down does not emit `restore-scroll`, (b) width-bucket change does not emit `restore-scroll`, (c) row pointer identity is preserved across no-op composes, (d) pending-head admit emits one `SCROLL_TO_TOP` restore without duplicate follow-up.

**Key files:**
- `apps/gnostr/src/ui/gnostr-timeline-feed-controller.c` — scroll policy enum, trigger mapping, row reuse helper

**Dependencies:** Items 1 + 3 (snapshot rows must carry published heights for reuse comparison).

**Size:** Medium-Large — significant controller refactor, but no public API changes.

---

### Item 5 — Diff-based snapshot model publication

**Goal:** Eliminate the full-nuke `items_changed` emission that forces GTK to teardown+rebind all visible rows on every compose.

**Done when:**
- `GnostrTimelineSnapshotModel` maintains a `published_rows` array; `get_n_items()` / `get_item()` serve from it.
- `replace_snapshot()` computes an LCS-based diff over event ids, emitting targeted `items_changed(position, removed, added)` spans.
- Fast path: if all row pointers are identical, no `items_changed` emitted (snapshot ref swapped only).
- Safety fallback: if diff generation fails (duplicate/null event ids), falls back to full-range replace.
- New tests assert: (a) metadata-only patch emits `items_changed` replace span not full-range nuke (capture `items-changed` signal on snapshot model), (b) pending-head admit emits insert span at position 0, (c) pagination emits append span, (d) identical recompose (same row pointers) emits no `items_changed`.

**Key files:**
- `apps/gnostr/src/model/gnostr-timeline-snapshot-model.c` — add `published_rows`, implement diff, change GListModel vtable

**Dependencies:** Item 4 (controller must produce reusable row objects for diff to be effective).

**Size:** Large — LCS implementation, edit script generation, fallback path, targeted tests.

---

### Item 6 — GTK row recycling and bind discipline

**Goal:** Stop destroying and recreating row widgets on every bind cycle.

**Done when:**
- `factory_setup_cb()` creates one `NostrGtkNoteCardRow`, connects permanent action/geometry signals, installs it as list-item child.
- `factory_bind_cb()` never creates a row or calls `gtk_list_item_set_child()`. Contains two internal helpers: `bind_snapshot_row()` and `bind_legacy_event_item()`.
- Snapshot bind path invariant: must not trigger async expansion. It must set reserved geometry before content, and must not connect `request-embed`, tier-2 map handler, `set_content_rendered()`, or `set_content_with_imeta()`. Exact call ordering is an implementation choice within this invariant.
- `factory_unbind_cb()` disconnects only bind-scoped handlers and calls `prepare_for_unbind()`.
- `factory_teardown_cb()` only runs final disposal.
- Row state reset follows existing convention: geometry fields (`reserved_height`, `geometry_token`, `snapshot_generation`) are reset in `prepare_for_bind()` (as currently implemented, `nostr-note-card-row.c`). `prepare_for_unbind()` / `quiesce()` handles async cancellation and disposed-state marking. Validate that this split is sufficient for clean recycling; do not move resets to unbind unless bind-side reset proves inadequate.
- Manually validated: scrolling through 100+ items shows no stale content bleeding between recycled rows.

**Key files:**
- `apps/gnostr/src/ui/gnostr-timeline-view-app-factory.c` — setup/bind/unbind/teardown refactor
- `nostr-gtk/src/nostr-note-card-row.c` — audit and extend `prepare_for_unbind()`
- `nostr-gtk/include/nostr-gtk-1.0/nostr-note-card-row.h` — update contract comments if needed

**Dependencies:** Items 1 + 3 (factory reads reserved heights from snapshot row). Note: this item does NOT depend on Item 5 (diff-based publication). `GtkSignalListItemFactory` recycles widgets based on the setup/bind contract regardless of `items_changed` granularity — moving row creation to `setup` reduces per-bind cost independently.

**Size:** Large — factory restructure, row lifecycle audit, manual validation.

---

### Item 7 — Main-window scroll feedback suppression

**Goal:** Eliminate the scroll feedback loop where controller-driven scroll restores re-enter controller viewport logic.

**Done when:**
- `GnostrMainWindow` (or its private struct) has a `timeline_scroll_restore_depth` counter.
- `on_timeline_restore_scroll_internal()` increments before `gtk_adjustment_set_value()`, decrements after.
- `on_timeline_scroll_value_changed_internal()` early-returns when `timeline_scroll_restore_depth > 0` (no `set_viewport`, no `set_user_at_top`, no pagination triggers).
- Redundant `scroll_to_top_idle()` from banner-click path removed for compositor-driven feeds; controller's `SCROLL_TO_TOP` policy handles it.
- Manually validated: profile patch while scrolled down produces no scroll movement; banner click scrolls to top once; window resize does not cause scroll fighting; older pagination preserves anchor without bounce.

**Key files:**
- `apps/gnostr/src/ui/gnostr-main-window-timeline.c` — guard + idle removal
- `apps/gnostr/src/ui/gnostr-main-window-private.h` — add counter field (validate actual struct location first)

**Dependencies:** None — the re-entrancy guard on `gtk_adjustment_set_value` is a pure main-window concern that works regardless of whether the controller has switched to the policy enum.

**Size:** Small — re-entrancy guard, one idle removal, one struct field.

## Risks

- **Diff/model desync**: If the LCS edit script is wrong, GTK's view of the model diverges. Mitigation: safety fallback to full-range replace on invalid identities/script failure, plus direct model emission tests.
- **Row reuse stale state**: If `NostrGtkNoteCardRow` does not fully reset on unbind, old reserved heights/media children/tokens bleed into next bind. Mitigation: explicit audit of `prepare_for_unbind()` and manual scroll validation.
- **Scroll suppression overreach**: If the guard is too broad, real user scrolls stop updating viewport state. Mitigation: scope guard only around synchronous `gtk_adjustment_set_value()` calls originating from `restore-scroll`.

## Implementation Order

```
Item 1 ─────────────────────┐
                             ├─→ Item 3 ─┐
Item 2 (independent) ───────┘            │
                                          ├─→ Item 4 ─→ Item 5
Item 7 (independent) ──────────────────┘
Item 6 (after 1+3) ────────────────────┘
```

Items 1, 2, and 7 can all proceed in parallel. Item 3 depends on Item 1. Item 4 depends on Items 1+3. Item 5 depends on Item 4. Item 6 depends on Items 1+3 (NOT Item 5 — row recycling works independently of diff granularity).

Critical path: 1 → 3 → 4 → 5.

Parallel paths: 2 (anytime), 6 (after 1+3), 7 (anytime).

Each item includes its own test coverage — there is no separate test-only work item. Tests land with the code that closes each invariant.

## Migration / Compatibility

- No persistence or serialized schema changes.
- Internal constructor signatures for `GnostrTimelineSnapshotRow` change; update all call sites atomically within Item 1.
- Bump geometry layout version so old in-memory measurement cache entries are ignored automatically.
- Legacy `GnNostrEventItem` path preserved for non-migrated consumers; dual bind paths in factory.

## Open Questions

1. **Main window private struct location**: The plan assumes `GnostrMainWindow` state lives in `gnostr-main-window-private.h`. Validate actual struct file before implementing Item 7.
2. **LCS complexity**: With page sizes of 30-50 rows, `O(n*m)` LCS is fine. If visible_limit ever grows much larger, consider switching to a simpler prefix/suffix/hash-diff hybrid.

## References

- `docs/designs/gnostr-timeline-compositor.md` — target architecture contract
- `docs/plans/gnostr-timeline-compositor-full-implementation.md` — previous incomplete plan
- `apps/gnostr/tests/test_timeline_feed_controller.c` — existing compositor test suite (20 tests)
- GTK4 `GtkSignalListItemFactory` — setup creates widgets once, bind updates data, unbind clears data, teardown destroys widgets
