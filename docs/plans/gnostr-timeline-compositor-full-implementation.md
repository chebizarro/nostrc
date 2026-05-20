# GNostr Timeline Compositor — Full Implementation Plan

## Status

The current compositor migration is not complete. The previous work introduced several nouns (`GnostrTimelineSource`, `GnostrTimelineBatch`, `GnostrTimelineSnapshot`, `GnostrTimelineFeedController`) and switched parts of the main timeline to them, but it did **not** fully enforce the architecture described in `docs/designs/gnostr-timeline-compositor.md`.

Observed failures:

- visible cards still lazy-load rich content and expand on the timeline;
- viewport still moves in response to passive loading and row measurement changes;
- snapshot rows do not contain a complete hydrated view model;
- rich card features were regressed during stabilization attempts;
- legacy live-mutation paths still influence the main feed;
- geometry is not yet treated as a complete first-class snapshot contract.

This plan replaces the incremental/compatibility migration with a complete compositor implementation for the main GNostr timeline.

## Non-negotiable invariants

1. The main feed renders only compositor-published snapshots.
2. Relay/database/profile/media arrival never directly mutates visible rows.
3. Snapshot rows are complete view models, not handles back into lazy live data.
4. Rows receive fixed geometry before reveal.
5. Late async work may fill reserved boxes, but may not change the outer row footprint.
6. New head items while the user is scrolled down remain pending until explicit reveal.
7. Older pagination preserves an identity+pixel anchor, not just scrollbar value.
8. GTK main thread performs final model publication and widget binding only.
9. Rich content parity is required: replies/thread context, repost/quote context, media placeholders, link previews/embeds where supported, hashtags/links, footer buttons, and interaction state.
10. No stubs, mock rendering, or placeholder-only replacements are acceptable as the final state.

## Target architecture

```text
Relays / nostrdb / profile fetch / metadata fetch / media probe
  -> GnostrTimelineSource
  -> GnostrTimelineHydrator
  -> GnostrTimelineItemViewModel
  -> GnostrTimelineGeometryResolver
  -> GnostrTimelineFeedController hidden working set
  -> immutable GnostrTimelineSnapshot
  -> GnostrTimelineSnapshotModel / GtkListView
  -> Timeline row binding into fixed geometry
```

### Data layer

`GnostrTimelineSource` owns query/subscription/database acquisition and emits generation-tagged source batches. It does not decide what becomes visible.

Required changes:

- all initial refresh, live head, older page, newer page, delete, profile, metadata, and interaction updates become explicit batch types;
- source batches contain enough stable identity to hydrate without visible row lookups;
- relay arrival order is ignored for presentation; sorting happens in compositor/hydration output.

### Hydration layer

Add `GnostrTimelineHydrator` and `GnostrTimelineItemViewModel`.

`GnostrTimelineItemViewModel` must contain all data needed to render a card without later row expansion:

- event id / note key / pubkey / created_at / deterministic tie-breaker;
- author display name, handle, avatar URL or fallback avatar state, NIP-05 if available;
- raw note text plus parsed/rendered content result;
- mentions, hashtags, links;
- reply/root context, repost context, quote context where available;
- media URLs and reserved display geometry;
- link preview/embed state and reserved geometry;
- reaction/repost/reply/zap counts and current-user state if available;
- relay/source info if displayed;
- moderation/filter state;
- geometry signature and initial reserved height.

Hydration policy:

- perform database/profile/metadata/media-probe work off the GTK main thread where safe;
- if a resource is unavailable, produce a deterministic fallback that includes reserved geometry;
- do not reveal rows whose outer geometry is unknown;
- interaction state and rich context may be absent only if the snapshot row explicitly reserves the same footprint and future patches replace data without changing geometry.

### Geometry layer

Add `GnostrTimelineGeometryResolver`.

Geometry is state, not a side effect of GTK measurement.

Responsibilities:

- produce width-bucketed geometry signatures;
- reserve avatar, text, footer, reply/repost/quote, media, and preview areas before reveal;
- cache measured row heights by `event-id + width-bucket + layout-signature`;
- clamp/quantize heights so late GTK measurement does not drive visible scrollbar changes;
- distinguish explicit user expansion from passive hydration.

### Compositor/controller layer

`GnostrTimelineFeedController` becomes the sole owner of visible feed publication.

Responsibilities:

- maintain hidden working set of hydrated view models;
- sort/deduplicate by `created_at DESC, event-id`;
- queue live head items while user is not at top;
- publish immutable snapshots in page-sized batches;
- capture/restore identity+pixel anchors around passive model changes;
- debounce background patch composition until scroll quiet when possible;
- never compose passive updates that require visible row-height changes.

### View layer

The GTK timeline row factory binds `GnostrTimelineItemViewModel`/snapshot rows only.

Responsibilities:

- set reserved height/geometry token first;
- render rich content from precomputed VM data;
- fixed avatar allocation with fallback immediately visible;
- load avatar pixels into fixed avatar allocation only;
- render media/link/embed placeholders with fixed dimensions;
- replace pixels/content inside reserved containers only;
- footer buttons are fully wired to existing actions;
- no `request-embed`, profile notify, content parsing, or visible model mutation from row bind.

The main feed must not use the legacy live `GnNostrEventModel` as its visible model. Legacy models may remain for other views during transition, but not as the main feed truth source.

## Work buckets

### Bucket 1 — Audit and remove main-feed legacy live mutation paths

Goal: identify every path where relay/database/profile/media/row binding can mutate visible main-feed rows or force scroll changes, then remove those paths from the main feed.

Key files:

- `apps/gnostr/src/ui/gnostr-main-window-timeline.c`
- `apps/gnostr/src/ui/gnostr-timeline-view-app-factory.c`
- `apps/gnostr/src/ui/gnostr-timeline-metadata-controller.c`
- `apps/gnostr/src/model/gn-nostr-event-model.{h,c}`
- `nostr-gtk/src/nostr-note-card-row.c`
- `nostr-gtk/src/gnostr-timeline-view.c`

Done when:

- main feed visible model is compositor snapshot model only;
- visible snapshot rows are not connected to mutable profile/count notifications;
- row bind does not emit async embed/profile requests that mutate the row footprint;
- scroll adjustment is not driven by legacy event-model insertion/drain code.

### Bucket 2 — Implement complete TimelineItemViewModel and hydrator

Goal: build the complete pre-display card view model required by the design.

Key files:

- new `apps/gnostr/src/model/gnostr-timeline-item-view-model.{h,c}`
- new `apps/gnostr/src/model/gnostr-timeline-hydrator.{h,c}`
- `apps/gnostr/src/model/gnostr-timeline-batch.{h,c}`
- `apps/gnostr/src/model/gnostr-timeline-source.{h,c}`
- existing content renderer/profile/metadata helpers

Done when:

- refresh/live/older batches hydrate into complete immutable VMs before publication;
- author, avatar fallback/URL, text render state, reply/repost/quote context, media/link reservations, interaction counts, and moderation state are represented;
- hydration is generation-guarded;
- worker-thread work does not touch GTK widgets;
- unit tests cover sorting/dedup, missing metadata fallbacks, and stale generation drops.

### Bucket 3 — Implement geometry resolver and fixed-footprint row contract

Goal: make row geometry deterministic before snapshot publication.

Key files:

- new `apps/gnostr/src/model/gnostr-timeline-geometry.{h,c}`
- `apps/gnostr/src/model/gnostr-timeline-snapshot.{h,c}`
- `apps/gnostr/src/ui/gnostr-timeline-feed-controller.c`
- `nostr-gtk/include/nostr-gtk-1.0/nostr-note-card-row.h`
- `nostr-gtk/src/nostr-note-card-row.c`

Done when:

- geometry resolver creates a row footprint from VM + width bucket;
- media/link/quote/repost/thread/footer areas have reserved dimensions;
- row measurement cannot increase the effective visible height after reveal;
- explicit user expansion is the only allowed footprint-changing path;
- tests cover cache keys, width bucket changes, measured-height reuse, and no passive height expansion.

### Bucket 4 — Rebuild compositor snapshot publication semantics

Goal: make the feed controller publish stable snapshots from hydrated VMs with correct pending-new-items and anchoring behavior.

Key files:

- `apps/gnostr/src/ui/gnostr-timeline-feed-controller.{h,c}`
- `apps/gnostr/src/model/gnostr-timeline-snapshot.{h,c}`
- `apps/gnostr/src/model/gnostr-timeline-snapshot-model.{h,c}`
- tests under `apps/gnostr/tests`

Done when:

- initial load publishes one page-sized snapshot after hydration/geometry resolution;
- live head while scrolled down only changes pending count;
- banner click intentionally admits pending head;
- older pagination appends a stable page and restores anchor identity+offset;
- metadata/profile/interaction patches replace hidden VMs and publish only if geometry remains stable or row is outside viewport;
- tests cover all admission policies and anchor fallbacks.

### Bucket 5 — Restore rich row rendering from immutable VM data

Goal: render full-featured note cards from VM/snapshot data without lazy footprint changes.

Key files:

- `apps/gnostr/src/ui/gnostr-timeline-view-app-factory.c`
- `nostr-gtk/src/nostr-note-card-row.c`
- `nostr-gtk/include/nostr-gtk-1.0/nostr-note-card-row.h`
- `apps/gnostr/src/ui/gnostr-timeline-embed.c`
- `apps/gnostr/src/ui/og-preview-widget.c`
- card/action relay files

Done when:

- text, hashtags/links, reply context, repost/quote context, media placeholders, link/embed preview areas, avatars, and footer buttons render from immutable VM data;
- footer actions work against event ids/pubkeys from the VM;
- avatar/media/link preview async loads replace content inside reserved boxes only;
- missing profile does not create extra phantom spacing;
- tests or manual checklist verify feature parity with the pre-compositor card behavior.

### Bucket 6 — End-to-end verification, regressions, and cleanup

Goal: prove the main feed meets the design and remove obsolete partial-migration code from the main path.

Key files:

- tests under `apps/gnostr/tests`
- `docs/designs/gnostr-timeline-compositor.md`
- this plan file
- any obsolete main-feed compatibility APIs

Done when:

- clean build passes;
- focused compositor/hydrator/geometry tests pass;
- full `ctest` is run and unrelated failures are documented;
- manual GNostr verification checklist is documented with exact steps;
- beads are closed only for completed behavior, not partial scaffolding;
- remote push succeeds.

## Dependency graph

- Bucket 1 can start immediately and informs all later work.
- Buckets 2 and 3 can run concurrently after Bucket 1’s audit identifies mutation hazards.
- Bucket 4 depends on Buckets 2 and 3.
- Bucket 5 depends on Buckets 2 and 3 and integrates with Bucket 4.
- Bucket 6 depends on all prior buckets.

## Verification checklist

Automated/headless where possible:

- initial load emits one snapshot, not one row per event;
- live head while scrolled down increments pending count only;
- banner admit prepends intentionally;
- older pagination preserves anchor event and pixel offset;
- profile patch does not alter effective row height;
- avatar load does not alter effective row height;
- media/link-preview load does not alter effective row height;
- stale hydrator result is dropped;
- delete removes authorized target from visible and pending sets;
- footer actions receive correct event identity from VM.

Manual UI verification:

1. Launch GNostr with cached timeline; feed appears as a stable initial snapshot.
2. Scroll into the middle and leave relays active; new notes banner changes, viewport does not.
3. Let avatars load; rows do not move and avatars are not washed out.
4. Let media/link previews load; pixels fill reserved spaces, rows do not expand.
5. Open threaded/replied-to events; reply context is visible without later expansion.
6. Check repost/quote cards; context renders from snapshot data.
7. Use like/repost/reply/zap/footer buttons; actions work and count updates do not reflow visible rows.
8. Scroll near bottom; older page loads without moving the current reading anchor.
9. Click new-notes banner; feed advances intentionally.
10. Reopen app; cached snapshot appears quickly and smoothly.

## Implementation rule

Do not close a bead for scaffolding. Close only when the user-visible behavior in its acceptance criteria is implemented and verified.
