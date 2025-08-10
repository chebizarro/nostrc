#ifndef NOSTR_FILTER_H
#define NOSTR_FILTER_H

#include <glib-object.h>
#include "nostr-filter.h"

/* Define NostrFilter GObject */
#define NOSTR_TYPE_FILTER (nostr_filter_get_type())
G_DECLARE_FINAL_TYPE(NostrFilter, nostr_filter, NOSTR, FILTER, GObject)

struct _NostrFilter {
    GObject parent_instance;
    Filter filter;
};

NostrFilter *nostr_filter_new();
void nostr_filter_set_ids(NostrFilter *self, const gchar **ids, gsize n_ids);
const gchar **nostr_filter_get_ids(NostrFilter *self, gsize *n_ids);
void nostr_filter_set_kinds(NostrFilter *self, const gint *kinds, gsize n_kinds);
const gint *nostr_filter_get_kinds(NostrFilter *self, gsize *n_kinds);
void nostr_filter_set_authors(NostrFilter *self, const gchar **authors, gsize n_authors);
const gchar **nostr_filter_get_authors(NostrFilter *self, gsize *n_authors);
void nostr_filter_set_since(NostrFilter *self, gint64 since);
gint64 nostr_filter_get_since(NostrFilter *self);
void nostr_filter_set_until(NostrFilter *self, gint64 until);
gint64 nostr_filter_get_until(NostrFilter *self);
void nostr_filter_set_limit(NostrFilter *self, gint limit);
gint nostr_filter_get_limit(NostrFilter *self);

#endif // NOSTR_FILTER_H