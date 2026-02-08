#include "nostr_filter.h"
#include <glib.h>

/* Core libnostr headers for JSON serialization and tags */
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "json.h"

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
    NostrTags *tags;   /* core tag filters (#e, #p, etc.) */
};

G_DEFINE_TYPE(GNostrFilter, gnostr_filter, G_TYPE_OBJECT)

static void
gnostr_filter_finalize(GObject *object)
{
    GNostrFilter *self = GNOSTR_FILTER(object);
    g_strfreev(self->ids);
    g_free(self->kinds);
    g_strfreev(self->authors);
    if (self->tags)
        nostr_tags_free(self->tags);
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

/* ---- Incremental builders ---- */

void
gnostr_filter_add_id(GNostrFilter *self, const gchar *id)
{
    g_return_if_fail(GNOSTR_IS_FILTER(self));
    g_return_if_fail(id != NULL);

    gsize n = self->ids ? g_strv_length(self->ids) : 0;
    self->ids = g_renew(gchar *, self->ids, n + 2);
    self->ids[n] = g_strdup(id);
    self->ids[n + 1] = NULL;
}

void
gnostr_filter_add_kind(GNostrFilter *self, gint kind)
{
    g_return_if_fail(GNOSTR_IS_FILTER(self));

    self->kinds = g_renew(gint, self->kinds, self->n_kinds + 1);
    self->kinds[self->n_kinds] = kind;
    self->n_kinds++;
}

void
gnostr_filter_tags_append(GNostrFilter *self, const gchar *key, const gchar *value)
{
    g_return_if_fail(GNOSTR_IS_FILTER(self));
    g_return_if_fail(key != NULL);

    /* Store tag as [key, value] — same format as core nostr_filter_tags_append.
     * nostr_tag_new is NULL-terminated variadic. */
    NostrTag *tag = nostr_tag_new(key, value ? value : "", NULL);
    if (!tag) return;

    if (!self->tags)
        self->tags = nostr_tags_new(0);
    nostr_tags_append(self->tags, tag);
}

NostrFilter *
gnostr_filter_build(GNostrFilter *self)
{
    g_return_val_if_fail(GNOSTR_IS_FILTER(self), NULL);

    NostrFilter *core = nostr_filter_new();

    if (self->ids) {
        gsize n = g_strv_length(self->ids);
        nostr_filter_set_ids(core, (const char *const *)self->ids, n);
    }
    if (self->kinds && self->n_kinds > 0)
        nostr_filter_set_kinds(core, self->kinds, self->n_kinds);
    if (self->authors) {
        gsize n = g_strv_length(self->authors);
        nostr_filter_set_authors(core, (const char *const *)self->authors, n);
    }
    if (self->since != 0)
        nostr_filter_set_since_i64(core, self->since);
    if (self->until != 0)
        nostr_filter_set_until_i64(core, self->until);
    if (self->limit >= 0)
        nostr_filter_set_limit(core, self->limit);
    if (self->tags) {
        /* Deep-copy tags into core filter (core takes ownership) */
        gsize n = nostr_tags_size(self->tags);
        NostrTags *copy = nostr_tags_new(0);
        nostr_tags_reserve(copy, n);
        for (gsize i = 0; i < n; i++) {
            NostrTag *src = nostr_tags_get(self->tags, i);
            gsize tag_len = nostr_tag_size(src);
            NostrTag *dup;
            if (tag_len >= 3)
                dup = nostr_tag_new(nostr_tag_get(src, 0), nostr_tag_get(src, 1), nostr_tag_get(src, 2), NULL);
            else if (tag_len == 2)
                dup = nostr_tag_new(nostr_tag_get(src, 0), nostr_tag_get(src, 1), NULL);
            else
                dup = nostr_tag_new(nostr_tag_get(src, 0), NULL);
            nostr_tags_append(copy, dup);
        }
        nostr_filter_set_tags(core, copy);
    }

    return core;
}

/* ---- JSON Serialization ---- */

/**
 * Helper: convert GNostrFilter fields into a core NostrFilter for serialization.
 * The caller must call nostr_filter_clear() on the result when done.
 */
static void
gnostr_filter_to_core(GNostrFilter *self, NostrFilter *core)
{
    memset(core, 0, sizeof(*core));

    if (self->ids) {
        gsize n = g_strv_length(self->ids);
        nostr_filter_set_ids(core, (const char *const *)self->ids, n);
    }
    if (self->kinds && self->n_kinds > 0)
        nostr_filter_set_kinds(core, self->kinds, self->n_kinds);
    if (self->authors) {
        gsize n = g_strv_length(self->authors);
        nostr_filter_set_authors(core, (const char *const *)self->authors, n);
    }
    if (self->since != 0)
        nostr_filter_set_since_i64(core, self->since);
    if (self->until != 0)
        nostr_filter_set_until_i64(core, self->until);
    if (self->limit >= 0)
        nostr_filter_set_limit(core, self->limit);
    if (self->tags) {
        gsize n = nostr_tags_size(self->tags);
        for (gsize i = 0; i < n; i++) {
            NostrTag *src = nostr_tags_get(self->tags, i);
            nostr_filter_tags_append(core, nostr_tag_get(src, 0),
                                     nostr_tag_size(src) > 1 ? nostr_tag_get(src, 1) : NULL,
                                     nostr_tag_size(src) > 2 ? nostr_tag_get(src, 2) : NULL);
        }
    }
}

GNostrFilter *
gnostr_filter_new_from_json(const gchar *json, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);

    NostrFilter core = {0};
    int rc = nostr_filter_deserialize(&core, json);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                            "Failed to parse JSON filter");
        nostr_filter_clear(&core);
        return NULL;
    }

    GNostrFilter *self = gnostr_filter_new();

    /* Copy ids */
    size_t n_ids = nostr_filter_ids_len(&core);
    if (n_ids > 0) {
        gchar **ids = g_new0(gchar *, n_ids + 1);
        for (size_t i = 0; i < n_ids; i++)
            ids[i] = g_strdup(nostr_filter_ids_get(&core, i));
        g_strfreev(self->ids);
        self->ids = ids;
    }

    /* Copy kinds */
    size_t n_kinds = nostr_filter_kinds_len(&core);
    if (n_kinds > 0) {
        g_free(self->kinds);
        self->kinds = g_new0(gint, n_kinds);
        self->n_kinds = n_kinds;
        for (size_t i = 0; i < n_kinds; i++)
            self->kinds[i] = nostr_filter_kinds_get(&core, i);
    }

    /* Copy authors */
    size_t n_authors = nostr_filter_authors_len(&core);
    if (n_authors > 0) {
        gchar **authors = g_new0(gchar *, n_authors + 1);
        for (size_t i = 0; i < n_authors; i++)
            authors[i] = g_strdup(nostr_filter_authors_get(&core, i));
        g_strfreev(self->authors);
        self->authors = authors;
    }

    /* Copy timestamps */
    self->since = nostr_filter_get_since_i64(&core);
    self->until = nostr_filter_get_until_i64(&core);
    self->limit = nostr_filter_get_limit(&core);

    /* Copy tags */
    NostrTags *core_tags = nostr_filter_get_tags(&core);
    if (core_tags) {
        gsize n_tags = nostr_tags_size(core_tags);
        if (n_tags > 0) {
            self->tags = nostr_tags_new(0);
            nostr_tags_reserve(self->tags, n_tags);
            for (gsize i = 0; i < n_tags; i++) {
                NostrTag *src = nostr_tags_get(core_tags, i);
                gsize tag_len = nostr_tag_size(src);
                NostrTag *dup;
                if (tag_len >= 3)
                    dup = nostr_tag_new(nostr_tag_get(src, 0), nostr_tag_get(src, 1), nostr_tag_get(src, 2), NULL);
                else if (tag_len == 2)
                    dup = nostr_tag_new(nostr_tag_get(src, 0), nostr_tag_get(src, 1), NULL);
                else
                    dup = nostr_tag_new(nostr_tag_get(src, 0), NULL);
                nostr_tags_append(self->tags, dup);
            }
        }
    }

    nostr_filter_clear(&core);
    return self;
}

gchar *
gnostr_filter_to_json(GNostrFilter *self)
{
    g_return_val_if_fail(GNOSTR_IS_FILTER(self), NULL);

    NostrFilter core = {0};
    gnostr_filter_to_core(self, &core);

    char *json = nostr_filter_serialize(&core);
    nostr_filter_clear(&core);
    return json;
}
