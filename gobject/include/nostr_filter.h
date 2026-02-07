#ifndef GNOSTR_FILTER_H
#define GNOSTR_FILTER_H

#include <glib-object.h>

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

G_END_DECLS

#endif /* GNOSTR_FILTER_H */
