#include "nostr_filter.h"
#include <glib.h>

/* GNostrFilter - standalone GObject filter (no core embedding).
 * Holds its own GLib-native data (GStrv, GArray) rather than
 * wrapping the core NostrFilter which uses StringArray/IntArray. */

struct _GNostrFilter {
    GObject parent_instance;
    gchar **ids;       /* NULL-terminated */
    gint  *kinds;
    gsize  n_kinds;
    gchar **authors;   /* NULL-terminated */
    gint64 since;
    gint64 until;
    gint   limit;
};

G_DEFINE_TYPE(GNostrFilter, gnostr_filter, G_TYPE_OBJECT)

static void
gnostr_filter_finalize(GObject *object)
{
    GNostrFilter *self = GNOSTR_FILTER(object);
    g_strfreev(self->ids);
    g_free(self->kinds);
    g_strfreev(self->authors);
    G_OBJECT_CLASS(gnostr_filter_parent_class)->finalize(object);
}

static void
gnostr_filter_class_init(GNostrFilterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gnostr_filter_finalize;
}

static void
gnostr_filter_init(GNostrFilter *self)
{
    self->limit = -1; /* no limit by default */
}

GNostrFilter *
gnostr_filter_new(void)
{
    return g_object_new(GNOSTR_TYPE_FILTER, NULL);
}

void
gnostr_filter_set_ids(GNostrFilter *self, const gchar **ids, gsize n_ids)
{
    g_return_if_fail(GNOSTR_IS_FILTER(self));
    g_strfreev(self->ids);
    self->ids = g_new0(gchar *, n_ids + 1);
    for (gsize i = 0; i < n_ids; i++)
        self->ids[i] = g_strdup(ids[i]);
}

const gchar **
gnostr_filter_get_ids(GNostrFilter *self, gsize *n_ids)
{
    g_return_val_if_fail(GNOSTR_IS_FILTER(self), NULL);
    if (self->ids)
        *n_ids = g_strv_length(self->ids);
    else
        *n_ids = 0;
    return (const gchar **)self->ids;
}

void
gnostr_filter_set_kinds(GNostrFilter *self, const gint *kinds, gsize n_kinds)
{
    g_return_if_fail(GNOSTR_IS_FILTER(self));
    g_free(self->kinds);
    self->kinds = g_new0(gint, n_kinds);
    self->n_kinds = n_kinds;
    for (gsize i = 0; i < n_kinds; i++)
        self->kinds[i] = kinds[i];
}

const gint *
gnostr_filter_get_kinds(GNostrFilter *self, gsize *n_kinds)
{
    g_return_val_if_fail(GNOSTR_IS_FILTER(self), NULL);
    *n_kinds = self->n_kinds;
    return self->kinds;
}

void
gnostr_filter_set_authors(GNostrFilter *self, const gchar **authors, gsize n_authors)
{
    g_return_if_fail(GNOSTR_IS_FILTER(self));
    g_strfreev(self->authors);
    self->authors = g_new0(gchar *, n_authors + 1);
    for (gsize i = 0; i < n_authors; i++)
        self->authors[i] = g_strdup(authors[i]);
}

const gchar **
gnostr_filter_get_authors(GNostrFilter *self, gsize *n_authors)
{
    g_return_val_if_fail(GNOSTR_IS_FILTER(self), NULL);
    if (self->authors)
        *n_authors = g_strv_length(self->authors);
    else
        *n_authors = 0;
    return (const gchar **)self->authors;
}

void
gnostr_filter_set_since(GNostrFilter *self, gint64 since)
{
    g_return_if_fail(GNOSTR_IS_FILTER(self));
    self->since = since;
}

gint64
gnostr_filter_get_since(GNostrFilter *self)
{
    g_return_val_if_fail(GNOSTR_IS_FILTER(self), 0);
    return self->since;
}

void
gnostr_filter_set_until(GNostrFilter *self, gint64 until)
{
    g_return_if_fail(GNOSTR_IS_FILTER(self));
    self->until = until;
}

gint64
gnostr_filter_get_until(GNostrFilter *self)
{
    g_return_val_if_fail(GNOSTR_IS_FILTER(self), 0);
    return self->until;
}

void
gnostr_filter_set_limit(GNostrFilter *self, gint limit)
{
    g_return_if_fail(GNOSTR_IS_FILTER(self));
    self->limit = limit;
}

gint
gnostr_filter_get_limit(GNostrFilter *self)
{
    g_return_val_if_fail(GNOSTR_IS_FILTER(self), -1);
    return self->limit;
}
