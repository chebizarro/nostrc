#ifndef GNOSTR_FILTER_H
#define GNOSTR_FILTER_H

#include <glib-object.h>
#include "nostr-error.h"

/* Forward-declare core NostrFilter for build() return type */
typedef struct NostrFilter NostrFilter;

G_BEGIN_DECLS

/**
 * GNostrFilter:
 *
 * A GObject wrapper for Nostr subscription filters (NIP-01).
 * G-prefixed to avoid collision with core libnostr NostrFilter.
 *
 * Provides a GObject-based filter builder for relay subscriptions
 * with property notifications.
 */
#define GNOSTR_TYPE_FILTER (gnostr_filter_get_type())
G_DECLARE_FINAL_TYPE(GNostrFilter, gnostr_filter, GNOSTR, FILTER, GObject)

GNostrFilter *gnostr_filter_new(void);

void           gnostr_filter_set_ids(GNostrFilter *self, const gchar **ids, gsize n_ids);
const gchar  **gnostr_filter_get_ids(GNostrFilter *self, gsize *n_ids);

void           gnostr_filter_set_kinds(GNostrFilter *self, const gint *kinds, gsize n_kinds);
const gint    *gnostr_filter_get_kinds(GNostrFilter *self, gsize *n_kinds);

void           gnostr_filter_set_authors(GNostrFilter *self, const gchar **authors, gsize n_authors);
const gchar  **gnostr_filter_get_authors(GNostrFilter *self, gsize *n_authors);

void           gnostr_filter_set_since(GNostrFilter *self, gint64 since);
gint64         gnostr_filter_get_since(GNostrFilter *self);

void           gnostr_filter_set_until(GNostrFilter *self, gint64 until);
gint64         gnostr_filter_get_until(GNostrFilter *self);

void           gnostr_filter_set_limit(GNostrFilter *self, gint limit);
gint           gnostr_filter_get_limit(GNostrFilter *self);

/* nostrc-57j: relay filtering */
void           gnostr_filter_set_relays(GNostrFilter *self, const gchar **relays, gsize n_relays);
const gchar  **gnostr_filter_get_relays(GNostrFilter *self, gsize *n_relays);
void           gnostr_filter_add_relay(GNostrFilter *self, const gchar *relay);

/* Incremental builders (append single values) */
void           gnostr_filter_add_id(GNostrFilter *self, const gchar *id);
void           gnostr_filter_add_kind(GNostrFilter *self, gint kind);

/**
 * gnostr_filter_tags_append:
 * @self: a #GNostrFilter
 * @key: tag key (e.g. "e", "p", "E")
 * @value: tag value (event id, pubkey, etc.)
 *
 * Appends a tag filter requirement. Maps to core
 * nostr_filter_tags_append(filter, key, value, NULL).
 */
void           gnostr_filter_tags_append(GNostrFilter *self, const gchar *key, const gchar *value);

/**
 * gnostr_filter_build:
 * @self: a #GNostrFilter
 *
 * Builds a heap-allocated core NostrFilter from this GObject filter.
 * Caller must free with nostr_filter_free().
 *
 * Returns: (transfer full): a new NostrFilter, or NULL on error
 */
NostrFilter   *gnostr_filter_build(GNostrFilter *self);

/**
 * gnostr_filter_new_from_json:
 * @json: a JSON string representing a Nostr filter
 * @error: (nullable): return location for a #GError
 *
 * Creates a new GNostrFilter by deserializing a JSON string.
 *
 * Returns: (transfer full) (nullable): a new #GNostrFilter, or %NULL on error
 */
GNostrFilter *gnostr_filter_new_from_json(const gchar *json, GError **error);

/**
 * gnostr_filter_to_json:
 * @self: a #GNostrFilter
 *
 * Serializes the filter to a JSON string.
 *
 * Returns: (transfer full) (nullable): a newly allocated JSON string
 */
gchar *gnostr_filter_to_json(GNostrFilter *self);

G_END_DECLS

#endif /* GNOSTR_FILTER_H */
