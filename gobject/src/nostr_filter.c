#include "nostr_filter.h"
#include <glib.h>

/* NostrFilter GObject implementation */
G_DEFINE_TYPE(NostrFilter, nostr_filter, G_TYPE_OBJECT)

static void nostr_filter_finalize(GObject *object) {
    NostrFilter *self = NOSTR_FILTER(object);
    g_free(self->filter.ids);
    g_free(self->filter.kinds);
    g_free(self->filter.authors);
    G_OBJECT_CLASS(nostr_filter_parent_class)->finalize(object);
}

static void nostr_filter_class_init(NostrFilterClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = nostr_filter_finalize;
}

static void nostr_filter_init(NostrFilter *self) {
}

NostrFilter *nostr_filter_new() {
    return g_object_new(NOSTR_TYPE_FILTER, NULL);
}

void nostr_filter_set_ids(NostrFilter *self, const gchar **ids, gsize n_ids) {
    g_free(self->filter.ids);
    self->filter.ids = g_new0(gchar *, n_ids + 1);
    for (gsize i = 0; i < n_ids; i++) {
        self->filter.ids[i] = g_strdup(ids[i]);
    }
}

const gchar **nostr_filter_get_ids(NostrFilter *self, gsize *n_ids) {
    *n_ids = g_strv_length(self->filter.ids);
    return (const gchar **)self->filter.ids;
}

void nostr_filter_set_kinds(NostrFilter *self, const gint *kinds, gsize n_kinds) {
    g_free(self->filter.kinds);
    self->filter.kinds = g_new0(gint, n_kinds);
    for (gsize i = 0; i < n_kinds; i++) {
        self->filter.kinds[i] = kinds[i];
    }
}

const gint *nostr_filter_get_kinds(NostrFilter *self, gsize *n_kinds) {
    *n_kinds = g_array_get_length(self->filter.kinds);
    return (const gint *)self->filter.kinds;
}

void nostr_filter_set_authors(NostrFilter *self, const gchar **authors, gsize n_authors) {
    g_free(self->filter.authors);
    self->filter.authors = g_new0(gchar *, n_authors + 1);
    for (gsize i = 0; i < n_authors; i++) {
        self->filter.authors[i] = g_strdup(authors[i]);
    }
}

const gchar **nostr_filter_get_authors(NostrFilter *self, gsize *n_authors) {
    *n_authors = g_strv_length(self->filter.authors);
    return (const gchar **)self->filter.authors;
}

void nostr_filter_set_since(NostrFilter *self, gint64 since) {
    self->filter.since = since;
}

gint64 nostr_filter_get_since(NostrFilter *self) {
    return self->filter.since;
}

void nostr_filter_set_until(NostrFilter *self, gint64 until) {
    self->filter.until = until;
}

gint64 nostr_filter_get_until(NostrFilter *self) {
    return self->filter.until;
}

void nostr_filter_set_limit(NostrFilter *self, gint limit) {
    self->filter.limit = limit;
}

gint nostr_filter_get_limit(NostrFilter *self) {
    return self->filter.limit;
}