/* gnostr-filter-set-query.c — #GnostrFilterSet → #GNostrTimelineQuery
 * conversion. See the header for the list of known lossy fields.
 *
 * SPDX-License-Identifier: MIT
 *
 * nostrc-yg8j.4: Timeline view filter set integration.
 */

#define G_LOG_DOMAIN "gnostr-filter-set-query"

#include "gnostr-filter-set-query.h"

#include <string.h>

GNostrTimelineQuery *
gnostr_filter_set_to_timeline_query(GnostrFilterSet *self)
{
  /* Null or empty → global feed. Always return *something* so callers
   * can unconditionally dispatch the query and users never end up staring
   * at a blank timeline when a misconfigured filter set is chosen. */
  if (!self || gnostr_filter_set_is_empty(self))
    return gnostr_timeline_query_new_global();

  GNostrTimelineQueryBuilder *b = gnostr_timeline_query_builder_new();

  /* ── Kinds ────────────────────────────────────────────────────────
   * Default to [1, 6] when the set doesn't specify kinds, matching the
   * other tab dispatch paths. */
  gsize n_kinds = 0;
  const gint *kinds = gnostr_filter_set_get_kinds(self, &n_kinds);
  if (kinds && n_kinds > 0) {
    for (gsize i = 0; i < n_kinds; i++)
      gnostr_timeline_query_builder_add_kind(b, kinds[i]);
  } else {
    gnostr_timeline_query_builder_add_kind(b, 1); /* text note */
    gnostr_timeline_query_builder_add_kind(b, 6); /* repost */
  }

  /* ── Authors ────────────────────────────────────────────────────── */
  const gchar * const *authors = gnostr_filter_set_get_authors(self);
  if (authors) {
    for (const gchar * const *p = authors; *p; p++) {
      if (**p)
        gnostr_timeline_query_builder_add_author(b, *p);
    }
  }

  /* ── Hashtags ─────────────────────────────────────────────────────
   * Emit every non-empty hashtag; the builder combines them into a
   * single NIP-01 `#t` array with OR semantics across values. */
  const gchar * const *hashtags = gnostr_filter_set_get_hashtags(self);
  if (hashtags) {
    for (const gchar * const *p = hashtags; *p; p++) {
      if (**p)
        gnostr_timeline_query_builder_add_hashtag(b, *p);
    }
  }

  /* ── Time range + limit ──────────────────────────────────────────── */
  gint64 since = gnostr_filter_set_get_since(self);
  gint64 until = gnostr_filter_set_get_until(self);
  gint   limit = gnostr_filter_set_get_limit(self);
  if (since > 0) gnostr_timeline_query_builder_set_since(b, since);
  if (until > 0) gnostr_timeline_query_builder_set_until(b, until);
  if (limit > 0) gnostr_timeline_query_builder_set_limit(b, (guint)limit);

  /* ── Top-level IDs (bookmark feeds) ──────────────────────────────
   * Map the FilterSet's `ids` to the query's top-level `ids` field,
   * which emits NIP-01 `"ids":[...]` in the filter JSON. This fetches
   * events by their own ID — exactly what bookmark-style feeds need.
   * nostrc-ch2v: NIP-51 bookmarks view. */
  const gchar * const *ids = gnostr_filter_set_get_ids(self);
  if (ids) {
    for (const gchar * const *p = ids; *p; p++) {
      if (**p)
        gnostr_timeline_query_builder_add_id(b, *p);
    }
  }

  const gchar * const *excluded = gnostr_filter_set_get_excluded_authors(self);
  if (excluded && excluded[0]) {
    g_debug("filter set '%s': dropping 'excluded_authors' — not a Nostr "
            "filter field, event model would need client-side post-filter.",
            gnostr_filter_set_get_name(self) ?
                gnostr_filter_set_get_name(self) : "(unnamed)");
  }

  return gnostr_timeline_query_builder_build(b);
}
