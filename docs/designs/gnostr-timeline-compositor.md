# GNostr Timeline Compositor Architecture

## Design intent

The GNostr timeline is a stable spatial reading environment, not a live list of
widgets. Data arrival time and view publication time must be decoupled. Relay,
database, profile, metadata, and media updates are allowed to be chaotic inside
the data layer, but the visible feed must behave like a calm, versioned reading
surface.

Primary invariant:

> The user's viewport must not move unless the user intentionally navigates or
> explicitly requests a new visible edition of the timeline.

Freshness is subordinate to spatial stability.

## UX semantics

The user reads a `TimelineSnapshot`, not live database state. A snapshot is an
ordered, hydrated, geometry-stable edition of the feed. While the user reads
snapshot `vN`, the application may prepare snapshot `vN+1` in the background,
but it must not automatically reveal content above the user's reading position.

Unexpected motion is treated as a bug. Acceptable motion is limited to:

- user scrolling;
- keyboard navigation;
- explicit refresh/paging;
- clicking a pending-new-items banner;
- deliberate expansion interactions.

New head content while the user is scrolled down is queued and represented by a
non-intrusive affordance such as “12 new notes”. It is not prepended into the
visible coordinate space until the user requests it.

## Layered architecture

```text
relay/network/database/profile/media chaos
        ↓
Data Layer: GnostrTimelineSource
        ↓ batches, generation-tagged deltas
Timeline Model Layer: GnostrTimelineFeedController / Compositor
        ↓ immutable measured snapshots
View Layer: GnostrTimelineSnapshotModel → GtkListView → row factory
```

### 1. Data layer

The data layer owns chaotic realtime work:

- relay and nostrdb subscriptions;
- initial database queries;
- pagination queries;
- deduplication inputs;
- worker-thread validation;
- profile/count/media metadata fetching;
- moderation/filter inputs.

It must never directly mutate the visible GTK model.

Proposed type:

- `GnostrTimelineSource`

Responsibilities:

- own `GNostrTimelineQuery` and query generation;
- own nostrdb subscription ids;
- run worker-thread batch validation;
- emit generation-tagged `GnostrTimelineBatch` objects;
- expose refresh/newer/older query APIs;
- emit “need profile” style requests without deciding presentation timing.

### 2. Timeline model / compositor layer

The compositor turns chaotic deltas into stable visual editions.

Proposed types:

- `GnostrTimelineFeedController`
- `GnostrTimelineSnapshot`
- `GnostrTimelineSnapshotRow`
- `GnostrTimelineSnapshotModel`

Responsibilities:

- maintain a hidden authoritative working set;
- sort by `created_at` descending plus deterministic event-id tie breaker;
- deduplicate;
- queue pending head updates while the user is reading;
- assign and cache geometry;
- capture and restore viewport anchors;
- publish immutable snapshots intentionally;
- coalesce source bursts into batch/snapshot updates.

The compositor is the only component allowed to decide that the visible feed
should advance to a new edition.

### 3. View layer

The view layer renders snapshots. It does not own freshness policy.

Responsibilities:

- bind immutable `GnostrTimelineSnapshotRow` items;
- apply reserved geometry before reveal;
- report measured geometry back to the controller;
- replace placeholder content inside already-reserved boxes;
- avoid mutating row shape after the row becomes visible.

The GTK main thread performs final model replacement and widget updates only.

## Snapshot model

### `GnostrTimelineSnapshotRow`

A snapshot row is immutable after construction. If profile, counts, metadata, or
moderation state changes, the compositor creates a replacement row in a future
snapshot instead of mutating the visible row object.

It should contain:

- event identity: event id, note key, pubkey;
- ordering: `created_at`, deterministic tie breaker;
- author display state: display name, handle, avatar URL/fallback, NIP-05;
- content state: note text, parsed links/mentions/hashtags, render result;
- thread/repost/reply context;
- media URLs and reserved aspect-ratio boxes;
- known/fallback media dimensions;
- link-preview reserved geometry;
- interaction counts and current-user state if available;
- relay/source display info;
- moderation/filter state;
- geometry state.

### Geometry fields

Geometry is first-class state, not an accidental rendering side effect.

Each row should track:

- estimated height;
- measured height;
- effective reserved height;
- width bucket;
- content/layout signature;
- media placeholder sizes;
- whether geometry has been measured for the current snapshot generation.

Geometry cache key:

```text
event-id + width-bucket + layout/content-signature
```

Do not key only by event id; width and row shape affect height.

### `GnostrTimelineSnapshot`

A snapshot contains:

- generation id;
- query generation;
- ordered rows;
- event-id → index lookup;
- prefix top offsets / cumulative heights;
- total estimated height;
- pending head count.

Snapshots are immutable once published.

### `GnostrTimelineSnapshotModel`

A small `GListModel` wrapper exposes the current snapshot to GTK. Snapshot
replacement should be atomic from GTK's perspective, preferably one
`items_changed(0, old_len, new_len)` emission unless later optimization demands
range diffs.

## Compositor state machine

### Source batch types

Batches should be generation-tagged and explicit:

- `REFRESH`
- `LIVE_HEAD`
- `PAGE_OLDER`
- `PAGE_NEWER`
- `DELETE`
- `PROFILE_PATCH`
- `METADATA_PATCH`

Stale generations are dropped before composition.

### Admission policy

#### Live head arrivals, user not at top

- merge into hidden working set;
- keep ids in `pending_head`;
- emit/update pending banner count;
- do not publish rows above the viewport.

#### Live head arrivals, user at top

- admit into next snapshot;
- publish in a batch;
- top-of-feed movement is intentional because the user is already at the live
  edge.

#### Banner click / jump to latest

- mark pending head ids as admitted;
- compose a new snapshot;
- navigate according to explicit UX policy, usually top-of-feed.

#### Older pagination

- append older page to hidden working set;
- publish one coherent snapshot;
- preserve the current anchor identity and pixel offset.

#### Metadata/profile patches

- update hidden working set;
- compose later, preferably after scroll quiets;
- only publish if the row footprint remains stable or if the row is outside the
  visible viewport.

## Viewport anchoring

Scrollbar value alone is not an anchor. The stable anchor is:

```c
typedef struct {
  char   *event_id;
  guint   index_hint;
  double  offset_px_in_row;
  guint64 snapshot_generation;
} GnostrTimelineAnchor;
```

Before publishing a new snapshot, capture the row containing the top of the
viewport using snapshot prefix geometry. After publishing, find the same event
id in the new snapshot and restore:

```text
new_scroll_value = new_row_top + old_offset_px_in_row
```

Fallbacks if the anchor row vanished:

1. nearest surviving predecessor from old snapshot;
2. nearest surviving successor;
3. first visible row;
4. top only for explicit navigation/reset actions.

Anchor preservation is skipped for deliberate navigation events such as filter
changes, explicit jump-to-latest, or full query replacement.

## Hydration strategy

Hydration is staged:

1. ingest raw event;
2. store in nostrdb;
3. query stable id window;
4. build source batch;
5. hydrate author/content/thread/media/count state where available;
6. determine geometry/reservations;
7. compose snapshot row;
8. publish snapshot intentionally.

Partial hydration is allowed only if it fills reserved spaces without changing
outer geometry. Profile text, avatar image, media image, and preview content may
appear later, but the card footprint must not expand or push adjacent content.

## Row rendering strategy

Rows must be bound from immutable snapshot rows. Binding should:

1. set geometry token and reserved height first;
2. set stable fallback avatar immediately;
3. set text/rendered content using precomputed render state;
4. reserve media/link/embed areas before reveal;
5. start async image/media loads only into reserved containers;
6. report final measured height back to the compositor;
7. ignore stale async callbacks via generation/binding tokens.

Avatars:

- fixed box size from first frame;
- fallback initials/avatar visible immediately;
- image replacement must not change allocation;
- no opacity/overlay wash while metadata is pending.

Media:

- never reveal unknown-size images as unconstrained widgets;
- use known dimensions when cached;
- otherwise use deterministic aspect-ratio placeholder;
- replace pixels inside the same box.

Link previews:

- hydrate before reveal, reserve a fixed preview area, or require explicit user
  expansion.

## Migration plan

### Phase 1: name the semantics in existing model

Keep `GnNostrEventModel` as the active `GListModel`, but make its live-head
behavior match compositor semantics:

- source arrivals hydrate into `insertion_buffer`;
- if user is not at top, keep them pending;
- emit banner count only;
- flush only on explicit intent/top return;
- remove visible row-height expansion paths.

This phase is already partially implemented.

### Phase 2: extract `GnostrTimelineSource`

Move subscription/query/worker acquisition out of `GnNostrEventModel` into a
source object. Keep `GnNostrEventModel` as a compatibility adapter consuming
source batches so visible behavior does not change yet.

Low-risk first files:

- `apps/gnostr/src/model/gnostr-timeline-source.{h,c}`
- `apps/gnostr/src/model/gnostr-timeline-batch.{h,c}`
- `apps/gnostr/src/model/gn-nostr-event-model.c`

### Phase 3: add immutable snapshot primitives

Add snapshot row/snapshot/snapshot-model types and unit tests without switching
UI yet.

Files:

- `apps/gnostr/src/model/gnostr-timeline-snapshot.{h,c}`
- `apps/gnostr/src/model/gnostr-timeline-snapshot-model.{h,c}`

Tests should verify:

- deterministic ordering;
- immutable row replacement;
- event-id lookup;
- prefix geometry;
- anchor math.

### Phase 4: add geometry measurement API to note rows

Extend `NostrGtkNoteCardRow` with:

- reserved-height setter;
- geometry token setter;
- measured-geometry notification/signal.

Files:

- `nostr-gtk/include/nostr-gtk-1.0/nostr-note-card-row.h`
- `nostr-gtk/src/nostr-note-card-row.c`

### Phase 5: implement `GnostrTimelineFeedController`

The controller owns:

- source;
- hidden working set;
- pending head queue;
- geometry cache;
- current snapshot model;
- viewport anchor;
- compose idle/debounce state.

It publishes snapshots to `GnostrTimelineSnapshotModel` and emits pending-count
changes for the banner.

### Phase 6: teach the timeline row factory snapshot binding

Add a dual path in the main timeline factory:

- existing `GnNostrEventItem` path for legacy model;
- new `GnostrTimelineSnapshotRow` path for compositor model.

Snapshot binding must not connect visible rows to mutable profile/count updates.
Those become compositor patches.

### Phase 7: switch main timeline page to the controller

Retarget `gnostr-main-window-timeline.c`:

- tab/filter changes call controller query APIs;
- scroll changes update controller viewport state;
- banner click calls controller flush;
- older pagination is requested through controller;
- direct `GnNostrEventModel` pending/scroll APIs leave the main feed path.

### Phase 8: convert metadata/profile updates to patches

`gnostr-timeline-metadata-controller.c` should produce patch batches for the
hidden working set instead of mutating visible item objects. Legacy mutation can
remain for old consumers during transition.

## Acceptance criteria

- Initial load publishes one calm snapshot or fixed-height skeleton edition.
- Relay bursts do not insert one row at a time into the visible feed.
- New head items while scrolled down only update the pending banner.
- Clicking the banner is the only normal way to reveal pending head items.
- Older pagination preserves the anchor event and pixel offset.
- Avatar/profile/media/link-preview hydration does not change visible row
  footprints.
- Snapshot changes are generation-guarded and stale async results are ignored.
- GTK main thread performs final snapshot publication and widget binding only.
- Full timeline state can be reasoned about as stable editions, not a live
  mutation stream.

## Implementation rule of thumb

If a change answers “what data exists now?”, it belongs in the source/working
set. If it answers “what should the user see now?”, it belongs in the compositor.
If it answers “how do pixels fill an already-reserved footprint?”, it belongs in
the view layer.

Do not let the database push the user's reading surface around.
