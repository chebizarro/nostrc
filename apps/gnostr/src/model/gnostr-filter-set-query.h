/* gnostr-filter-set-query.h — Convert a #GnostrFilterSet into a
 * #GNostrTimelineQuery suitable for driving a timeline view.
 *
 * SPDX-License-Identifier: MIT
 *
 * The two types describe overlapping but not identical concepts:
 *
 *   - #GnostrFilterSet is the **persisted, user-facing spec** (name,
 *     icon, colour, plus Nostr filter criteria). It supports multi-hashtag,
 *     excluded-authors, and an event-id (top-level `ids`) constraint for
 *     bookmark-style feeds.
 *
 *   - #GNostrTimelineQuery is the **runtime query** the event model
 *     dispatches to NDB / relays. It accepts a multi-hashtag list (OR
 *     semantics in NIP-01 `#t`), has no excluded-authors, and uses
 *     `event_ids` for the `#e` tag filter (thread view), NOT the
 *     top-level `ids` field.
 *
 * Known limitations of the conversion — documented here so callers know
 * what the resulting query will and will not honour:
 *
 *   1. **Hashtags** — every non-empty hashtag in the filter set is
 *      forwarded to the runtime query via
 *      gnostr_timeline_query_builder_add_hashtag(). NIP-01 treats
 *      multiple values inside a single `#t` filter as OR, so notes
 *      tagged with any one of them match. Single-tag filters map
 *      straight through. nostrc-yg8j.7.
 *
 *   2. **FilterSet.ids** (top-level Nostr `ids` filter for e.g. a
 *      bookmark feed) is **not** mapped. Populating
 *      TimelineQuery.event_ids would instead emit a `#e` tag filter,
 *      changing semantics. Bookmark-style feeds therefore need a
 *      dedicated dispatch path; the converter currently drops this
 *      field with a g_debug().
 *
 *   3. **excluded_authors** is client-side only and is not representable
 *      in a Nostr subscription filter. The converter drops it with a
 *      g_debug(); the event model is responsible for post-filtering if
 *      that ever becomes relevant.
 *
 *   4. **Empty filter set** (no authors, kinds, hashtags, ids, since,
 *      until, limit) falls back to the **global** timeline query so the
 *      user always sees something sensible instead of an empty view.
 *
 *   5. **Default kinds** — when a filter set does not specify kinds the
 *      converter fills in `[1, 6]` (text note + repost), matching
 *      gnostr_timeline_query_new_global() and the other tab types.
 *
 * nostrc-yg8j.4: Timeline view filter set integration.
 */

#ifndef GNOSTR_FILTER_SET_QUERY_H
#define GNOSTR_FILTER_SET_QUERY_H

#include <glib.h>

#include "model/gnostr-filter-set.h"
#include <nostr-gobject-1.0/gn-timeline-query.h>

G_BEGIN_DECLS

/**
 * gnostr_filter_set_to_timeline_query:
 * @self: (nullable): the filter set to translate
 *
 * Produce a newly-allocated #GNostrTimelineQuery that approximates the
 * criteria of @self as closely as the runtime query type allows. See the
 * file-level comment for the list of known lossy fields (ids and
 * excluded_authors; hashtags are forwarded as multi-value `#t`).
 *
 * If @self is %NULL or empty (see gnostr_filter_set_is_empty()) the
 * returned query is the global timeline query.
 *
 * Returns: (transfer full): a new query; free with
 *   gnostr_timeline_query_free().
 */
GNostrTimelineQuery *gnostr_filter_set_to_timeline_query(GnostrFilterSet *self);

G_END_DECLS

#endif /* GNOSTR_FILTER_SET_QUERY_H */
