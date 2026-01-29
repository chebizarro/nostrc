# Timeline Architecture Refactor

**Issue**: nostrc-e03f  
**Status**: Phase 2 Complete, Phase 3 In Progress

## Overview

The new timeline architecture provides a flexible, efficient way to display multiple filtered timeline views. It consists of three main components:

1. **GnTimelineQuery** - Immutable filter specification
2. **GnTimelineModel** - Lazy view on NostrDB (new) or enhanced GnNostrEventModel (existing)
3. **GnTimelineView** - GTK widget for display

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                    GnTimelineQuery                          │
│  - Immutable filter specification                           │
│  - Kinds, authors, since/until, limit, search, hashtags     │
│  - Builder pattern for complex queries                      │
│  - JSON serialization for NostrDB                           │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│              GnNostrEventModel (Enhanced)                   │
│  - Implements GListModel                                    │
│  - Subscription-driven updates from NostrDB                 │
│  - Position-based LRU cache                                 │
│  - "Replace all" flush strategy (no widget recycling storm) │
│  - New API: set_timeline_query() / get_timeline_query()     │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    GnTimelineView                           │
│  - GTK widget wrapping GtkListView                          │
│  - Scroll position tracking                                 │
│  - "New notes" indicator with animation                     │
│  - Empty state and loading spinner                          │
│  - Uses existing GnostrNoteCardRow                          │
└─────────────────────────────────────────────────────────────┘
```

## Usage Examples

### 1. Global Timeline (Default)

```c
#include "model/gn-timeline-query.h"
#include "model/gn-nostr-event-model.h"

// Create global timeline query
GnTimelineQuery *query = gn_timeline_query_new_global();

// Create model with query
GnNostrEventModel *model = gn_nostr_event_model_new_with_query(query);

// Or set query on existing model
GnNostrEventModel *model = gn_nostr_event_model_new();
gn_nostr_event_model_set_timeline_query(model, query);

// Don't forget to free the query (model keeps a copy)
gn_timeline_query_free(query);
```

### 2. Single Author Timeline

```c
// Create query for a specific author
GnTimelineQuery *query = gn_timeline_query_new_for_author(
    "82341f882b6eabcd2ba7f1ef90aad961cf074af15b9ef44a09f9d2a8fbfbe6a2"
);

GnNostrEventModel *model = gn_nostr_event_model_new_with_query(query);
gn_timeline_query_free(query);
```

### 3. User List (Multiple Authors)

```c
const char *authors[] = {
    "82341f882b6eabcd2ba7f1ef90aad961cf074af15b9ef44a09f9d2a8fbfbe6a2",
    "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d",
    "npub1...",  // Will need hex conversion
    NULL
};

GnTimelineQuery *query = gn_timeline_query_new_for_authors(authors, 3);
GnNostrEventModel *model = gn_nostr_event_model_new_with_query(query);
gn_timeline_query_free(query);
```

### 4. Hashtag Timeline

```c
GnTimelineQuery *query = gn_timeline_query_new_for_hashtag("nostr");
GnNostrEventModel *model = gn_nostr_event_model_new_with_query(query);
gn_timeline_query_free(query);
```

### 5. Custom Query with Builder

```c
GnTimelineQueryBuilder *builder = gn_timeline_query_builder_new();

// Add kinds
gn_timeline_query_builder_add_kind(builder, 1);  // Text notes
gn_timeline_query_builder_add_kind(builder, 6);  // Reposts
gn_timeline_query_builder_add_kind(builder, 30023);  // Long-form

// Add authors
gn_timeline_query_builder_add_author(builder, "pubkey1...");
gn_timeline_query_builder_add_author(builder, "pubkey2...");

// Set time range (last 24 hours)
gint64 now = g_get_real_time() / G_USEC_PER_SEC;
gn_timeline_query_builder_set_since(builder, now - 86400);

// Set limit
gn_timeline_query_builder_set_limit(builder, 100);

// Build (consumes builder)
GnTimelineQuery *query = gn_timeline_query_builder_build(builder);

GnNostrEventModel *model = gn_nostr_event_model_new_with_query(query);
gn_timeline_query_free(query);
```

### 6. Using GnTimelineView Widget

```c
#include "ui/gn-timeline-view.h"

// Create view
GnTimelineView *view = gn_timeline_view_new();

// Create and set model
GnTimelineQuery *query = gn_timeline_query_new_global();
GnTimelineModel *model = gn_timeline_model_new(query);
gn_timeline_view_set_model(view, model);

// Connect signals
g_signal_connect(view, "need-profile", G_CALLBACK(on_need_profile), self);
g_signal_connect(view, "activate", G_CALLBACK(on_note_activated), self);

// Add to container
gtk_box_append(GTK_BOX(container), GTK_WIDGET(view));
```

## Multiple Timeline Views

The architecture supports multiple simultaneous timeline views:

```c
// Main timeline
GnTimelineQuery *main_query = gn_timeline_query_new_global();
GnNostrEventModel *main_model = gn_nostr_event_model_new_with_query(main_query);

// Mentions timeline
GnTimelineQueryBuilder *mentions_builder = gn_timeline_query_builder_new();
gn_timeline_query_builder_add_kind(mentions_builder, 1);
gn_timeline_query_builder_add_author(mentions_builder, my_pubkey);
GnTimelineQuery *mentions_query = gn_timeline_query_builder_build(mentions_builder);
GnNostrEventModel *mentions_model = gn_nostr_event_model_new_with_query(mentions_query);

// Each model can be displayed in its own GnTimelineView
GnTimelineView *main_view = gn_timeline_view_new_with_model(main_model);
GnTimelineView *mentions_view = gn_timeline_view_new_with_model(mentions_model);
```

## Key Design Decisions

### 1. Immutable Queries
`GnTimelineQuery` is immutable after creation. To change the filter, create a new query and call `set_timeline_query()`.

### 2. Replace-All Flush Strategy
When flushing pending notes, the model uses a "replace all" strategy with a single `items_changed` signal. This prevents GTK widget recycling storms that caused crashes.

### 3. Profile Gating
Notes are only displayed if their author has a profile (kind 0) in the database. The `need-profile` signal is emitted for missing profiles.

### 4. Backward Compatibility
The existing `GnNostrEventModel` API (`set_query()` with `GnNostrQueryParams`) continues to work. The new `GnTimelineQuery` API is preferred for new code.

## Files

### New Files
- `apps/gnostr/src/model/gn-timeline-query.h` - Query specification header
- `apps/gnostr/src/model/gn-timeline-query.c` - Query implementation
- `apps/gnostr/src/model/gn-timeline-model.h` - New model header (alternative)
- `apps/gnostr/src/model/gn-timeline-model.c` - New model implementation
- `apps/gnostr/src/ui/gn-timeline-view.h` - View widget header
- `apps/gnostr/src/ui/gn-timeline-view.c` - View widget implementation

### Modified Files
- `apps/gnostr/src/model/gn-nostr-event-model.h` - Added GnTimelineQuery API
- `apps/gnostr/src/model/gn-nostr-event-model.c` - Implemented GnTimelineQuery integration

## Crash Resistance Features

### Update Debouncing
When events arrive rapidly from nostrdb, updates are debounced (default 50ms) to prevent widget recycling storms:
- Multiple batches are coalesced into a single UI update
- Cache is NOT cleared on sort (cache is keyed by note_key, not position)
- Single `items_changed` signal covers all accumulated changes

### Batch Mode
For initial load, use batch mode to suppress signals entirely:
```c
gn_timeline_model_begin_batch(model);
// ... initial loading happens via subscription ...
gn_timeline_model_end_batch(model);  // Single items_changed emitted
```

### Pagination
`gn_timeline_model_load_older()` is now fully implemented:
- Queries nostrdb with `until=oldest_timestamp-1`
- Parses JSON results to extract note keys
- Appends older items to the model
- Emits proper `items_changed` signal

## Future Work

- [ ] Add search query support to GnTimelineQuery
- [ ] Implement tab/sidebar UI for switching between timelines
- [ ] Add user list management UI
- [ ] Performance optimization for very large author lists
- [ ] Add prefetching for smooth scrolling
