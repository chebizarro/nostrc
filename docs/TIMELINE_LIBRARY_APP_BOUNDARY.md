# Timeline Library/App Boundary Proposal

**Date:** 2026-04-01 (last updated 2026-04-16)  
**Status:** Implemented (Phases 2–5 complete)  
**Related beads:** `nostrc-elln`, `nostrc-elln.1`, `nostrc-lkoa`, `nostrc-hiei`, `nostrc-hqtn`

## Summary

This note proposes the next-step API and ownership boundary for the GNostr
timeline split between:

- `nostr-gtk` as the reusable widget/rendering library
- `apps/gnostr` as the application-specific adapter layer

The immediate goal is to eliminate the remaining app dependency on
`gnostr-timeline-view-private.h` without pushing GNostr product policy into the
shared library.

## Current Problem

The current split is only partial.

`apps/gnostr/src/ui/gnostr-timeline-view-app-factory.c` still depends on
library internals in three ways:

1. It includes `nostr-gtk-1.0/gnostr-timeline-view-private.h`
2. It directly uses `TimelineItem` struct fields and legacy helper behavior
3. It stores GNostr-specific batching state on `NostrGtkTimelineView`

This makes `nostr-gtk` harder to evolve safely because the GNostr app is still
coupled to private layout details.

## Preferred Architecture

The preferred architecture is:

- **`nostr-gtk` owns generic widget state, rendering primitives, and stable
  extension points**
- **GNostr owns application policy, async metadata strategy, and main-window
  action routing**

More concretely:

- `NostrGtkTimelineView` remains a reusable widget with scroll tracking, tabs,
  and list/factory plumbing
- `NostrGtkNoteCardRow` remains a reusable row widget with generic presentation
  APIs and generic action signals
- GNostr provides an **adapter/controller layer** that binds app models to row
  widgets, performs GNostr-specific metadata lookups, and relays row actions to
  the main window

This keeps the library generic while still allowing GNostr to provide a rich
product-specific experience.

## Ownership Boundary

### `nostr-gtk` responsibilities

- `NostrGtkTimelineView` widget lifecycle
- internal `GtkListView`, scroller, selection model, and tab UI
- generic scroll/visibility helpers
- generic row widgets and presentation setters
- stable public APIs for any externally-supported timeline model helpers
- generic signals/hooks needed by consumers

### GNostr responsibilities

- choosing which model objects are bound in the timeline
- deciding when expensive metadata work is deferred or batched
- calling storage/NDB helpers for reactions, reposts, replies, zaps, etc.
- GNostr-specific action relay behavior
  - reply
  - repost
  - quote
  - mute
  - report
  - label
  - zap
- app-level content policy and product choices layered on top of shared widgets

### Explicit non-goal

`nostr-gtk` should **not** become a second application layer. It should not own
GNostr-specific storage queries, main-window dispatch, or GNostr product policy.

## Public API Proposal

## 1. Treat `TimelineItem` as internal unless proven necessary

Preferred direction:

- standardize GNostr on `GnNostrEventItem`
- remove app-side fallback paths that depend on direct `TimelineItem` struct use

If `TimelineItem` must remain supported outside the library, expose only a small
public API surface, for example:

- `TimelineItem *nostr_gtk_timeline_item_new(...)`
- `void nostr_gtk_timeline_item_set_meta(...)`
- `void nostr_gtk_timeline_item_set_repost_info(...)`
- `void nostr_gtk_timeline_item_set_quote_info(...)`
- `void nostr_gtk_timeline_item_add_child(...)`
- `GListModel *nostr_gtk_timeline_item_get_children(...)`

Do **not** expose struct layout publicly.

## 2. Keep timeline view hooks generic

The library should expose only generic hooks needed by consumers, such as:

- existing scroll helpers:
  - `nostr_gtk_timeline_view_get_visible_range()`
  - `nostr_gtk_timeline_view_is_item_visible()`
  - `nostr_gtk_timeline_view_is_fast_scrolling()`
- existing factory install point:
  - `nostr_gtk_timeline_view_set_factory()`
- existing internal widget accessors where justified:
  - `nostr_gtk_timeline_view_get_list_view()`
  - `nostr_gtk_timeline_view_get_scrolled_window()`

If additional lifecycle hooks are required, they should be generic and narrow.
Examples:

- a public bind helper for “run when row becomes visible”
- a public signal for visibility/range changes if consumers genuinely need it

They should not encode GNostr-specific batching concepts.

## 3. Move batching state into an app-owned controller

Introduce a GNostr-owned type or context, for example:

- `GnostrTimelineMetadataController`

Responsibilities:

- queue items that need metadata refresh
- schedule idle dispatch
- capture main-thread-only inputs before worker execution
- apply async results back to bound models
- clean up idle sources safely during teardown

Possible attachment points:

- qdata on `NostrGtkTimelineView`
- qdata on the app factory
- a dedicated GNostr adapter object that owns both the factory and controller

Preferred choice:

- **GNostr-owned adapter object with an embedded metadata controller**

This gives one explicit owner for factory wiring, batching, and teardown.

## 4. Keep row actions signal-based

The library should continue exposing generic row-level signals and setters.
GNostr should connect those signals in app code and route them to GNostr
application actions.

This is preferable to moving GNostr main-window knowledge into `nostr-gtk`.

## APIs explicitly rejected

The following APIs should be avoided unless absolutely necessary:

- exposing `struct _TimelineItem` in public headers
- exposing `struct _NostrGtkTimelineView` in public headers
- adding GNostr-specific fields like pending metadata queues to library widgets
- adding storage/NDB-specific methods to `nostr-gtk`
- embedding GNostr main-window callbacks directly into library widgets

## Migration Plan

### Phase 1: design lock

- adopt this ownership boundary
- decide whether `TimelineItem` remains externally supported or becomes fully
  internal

### Phase 2: remove app dependence on private model/layout — ✅ DONE (nostrc-lkoa)

- Deleted app-side `TimelineItem` fallback paths (6 dead helpers, legacy
  bind branch, repost/quote block, child-model passthrough)
- Standardized app binding on `GnNostrEventItem`
- Added `g_critical` type guard in `factory_bind_cb`

### Phase 3: extract metadata batching from widget-private state — ✅ DONE (nostrc-hiei)

- Created `GnostrTimelineMetadataController`
  (`apps/gnostr/src/ui/gnostr-timeline-metadata-controller.{h,c}`)
- Moved `pending_metadata_items` and `metadata_batch_idle_id` out of
  `NostrGtkTimelineView`
- Controller attached to view via qdata with `ensure()` idiom

### Phase 4: make the app adapter boundary explicit — ✅ DONE (nostrc-hqtn)

- Created `GnostrTimelineActionRelay`
  (`apps/gnostr/src/ui/gnostr-timeline-action-relay.{h,c}`)
  - 17 main-window-bound NoteCardRow action handlers in a single GObject
  - Created in `setup_app_factory()`, attached via qdata
  - Enables single-call disconnect via `g_signal_handlers_disconnect_by_data()`
  - Uses `gtk_widget_get_ancestor()` to find main window at handler time
- Factory reduced from ~1500 to ~1050 lines
- Dead `try_set_avatar()` removed
- Factory no longer includes `gnostr-main-window.h`, `gnostr-zap-dialog.h`,
  `nostr_profile_provider.h`, `nostr_nip19.h`, `gnostr-relays.h`, or
  `nip32_labels.h` — those moved to the relay

### Phase 5: remove private header include — ✅ DONE (nostrc-lkoa)

- Deleted `#include <nostr-gtk-1.0/gnostr-timeline-view-private.h>` from
  app code
- App compiles against public headers only

## Thread-Safety Requirements

The refactor must preserve the rule that GTK/main-thread-only values are read on
the main thread before worker execution.

Examples:

- current-user identity from GSettings
- any widget state
- any library objects not documented as thread-safe

Worker threads should receive plain copied data and perform only thread-safe DB
or compute work.

## Risks

### API overgrowth

Risk: replacing private coupling with too many public helpers.

Mitigation:

- prefer deleting obsolete external usage over stabilizing broad APIs
- if a public API is needed, keep it minimal and purpose-specific

### Hidden legacy dependencies

Risk: other code paths may still assume `TimelineItem` layout or app-owned
widget scratch state.

Mitigation:

- search all app/library references before removing the private header
- verify fallback thread/tree code paths explicitly

### Lifecycle regressions

Risk: moving batching state and factory ownership may introduce idle-source leaks
or rebind teardown bugs.

Mitigation:

- keep ownership centralized in one adapter/controller
- verify bind/unbind/teardown behavior during scrolling and disposal

## Verification Plan

- build from clean state
- run GNostr tests affected by timeline/model code
- manually smoke test timeline scrolling, deferred metadata loading, and row
  actions
- verify the app no longer includes `gnostr-timeline-view-private.h`
- verify the library no longer carries GNostr-only scratch fields

## Open Questions (Resolved)

1. **Is any external consumer besides GNostr using `TimelineItem` directly?**
   → No. Only the nostr-gtk library uses `TimelineItem` internally.
2. **Can GNostr fully standardize on `GnNostrEventItem` now?**
   → Yes. Done in nostrc-lkoa.
3. **Does `nostr-gtk` need one additional generic lifecycle hook for visibility?**
   → No. The app uses existing `is_item_visible()` / `is_fast_scrolling()` APIs.

## Implemented Adapter Types

| Type | File | Purpose |
|------|------|---------|
| `GnostrTimelineMetadataController` | `apps/gnostr/src/ui/gnostr-timeline-metadata-controller.{h,c}` | Batch metadata loading (reactions, reposts, replies, zaps) |
| `GnostrTimelineActionRelay` | `apps/gnostr/src/ui/gnostr-timeline-action-relay.{h,c}` | 17 NoteCardRow action → main window relays |

Both are attached to `NostrGtkTimelineView` via qdata in
`gnostr_timeline_view_setup_app_factory()` and automatically cleaned up
when the view disposes.

## Decision

Proceed with:

- **opaque or eliminated `TimelineItem` usage from the app**
- **GNostr-owned metadata/controller extraction**
- **signal-based app adapter boundary**

Do **not** preserve direct app access to library-private struct layouts.
