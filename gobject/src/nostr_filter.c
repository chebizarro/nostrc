#include "nostr_filter.h"
#include <glib.h>

/* NostrFilter GObject implementation */
G_DEFINE_TYPE(NostrFilter, nostr_filter, G_TYPE_OBJECT)

static void nostr_filter_finalize(GObject *object) {
    NostrFilter *self = NOSTR_FILTER(object);
    g_free(self->filter.IDs);
    g_free(self->filter.Kinds);
    g_free(self->filter.Authors);
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
    g_free(self->filter.IDs);
    self->filter.IDs = g_new0(gchar *, n_ids + 1);
    for (gsize i = 0; i < n_ids; i++) {
        self->filter.IDs[i] = g_strdup(ids[i]);
    }
}

const gchar **nostr_filter_get_ids(NostrFilter *self, gsize *n_ids) {
    *n_ids = g_strv_length(self->filter.IDs);
    return (const gchar **)self->filter.IDs;
}

void nostr_filter_set_kinds(NostrFilter *self, const gint *kinds, gsize n_kinds) {
    g_free(self->filter.Kinds);
    self->filter.Kinds = g_new0(gint, n_kinds);
    for (gsize i = 0; i < n_kinds; i++) {
        self->filter.Kinds[i] = kinds[i];
    }
}

const gint *nostr_filter_get_kinds(NostrFilter *self, gsize *n_kinds) {
    *n_kinds = g_array_get_length(self->filter.Kinds);
    return (const gint *)self->filter.Kinds;
}

void nostr_filter_set_authors(NostrFilter *self, const gchar **authors, gsize n_authors) {
    g_free(self->filter.Authors);
    self->filter.Authors = g_new0(gchar *, n_authors + 1);
    for (gsize i = 0; i < n_authors; i++) {
        self->filter.Authors[i] = g_strdup(authors[i]);
    }
}

const gchar **nostr_filter_get_authors(NostrFilter *self, gsize *n_authors) {
    *n_authors = g_strv_length(self->filter.Authors);
    return (const gchar **)self->filter.Authors;
}

void nostr_filter_set_since(NostrFilter *self, gint64 since) {
    self->filter.Since = since;
}

gint64 nostr_filter_get_since(NostrFilter *self) {
    return self->filter.Since;
}

void nostr_filter_set_until(NostrFilter *self, gint64 until) {
    self->filter.Until = until;
}

gint64 nostr_filter_get_until(NostrFilter *self) {
    return self->filter.Until;
}

void nostr_filter_set_limit(NostrFilter *self, gint limit) {
    self->filter.Limit = limit;
}

gint nostr_filter_get_limit(NostrFilter *self) {
    return self->filter.Limit;
}