/* gnostr-filter-set.h — Data model for a configurable timeline filter set.
 *
 * SPDX-License-Identifier: MIT
 *
 * A FilterSet defines the subscription filter criteria that power a single
 * timeline view. Filter sets can be predefined (driven by follow lists,
 * bookmarks, mutes, etc.) or user-created (hashtag feeds, custom filters).
 *
 * nostrc-yg8j.1: FilterSet data model and serialization.
 */

#ifndef GNOSTR_FILTER_SET_H
#define GNOSTR_FILTER_SET_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * GnostrFilterSetSource:
 * @GNOSTR_FILTER_SET_SOURCE_CUSTOM: User-defined filter set.
 * @GNOSTR_FILTER_SET_SOURCE_PREDEFINED: Built-in filter set (follows,
 *   bookmarks, mutes, etc). Not directly editable by the user.
 *
 * Indicates where a filter set's criteria originate. Predefined sets are
 * typically derived from user lists (NIP-02, NIP-51) and are refreshed
 * automatically; custom sets are persisted and edited by the user.
 */
typedef enum {
  GNOSTR_FILTER_SET_SOURCE_CUSTOM = 0,
  GNOSTR_FILTER_SET_SOURCE_PREDEFINED = 1,
} GnostrFilterSetSource;

#define GNOSTR_TYPE_FILTER_SET (gnostr_filter_set_get_type())
G_DECLARE_FINAL_TYPE(GnostrFilterSet, gnostr_filter_set, GNOSTR, FILTER_SET, GObject)

/* ------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------ */

/**
 * gnostr_filter_set_new:
 *
 * Create a new empty filter set. The caller should at minimum set a name
 * via gnostr_filter_set_set_name() before use. An ID is auto-generated
 * on creation (UUID-like string) and can be overridden with
 * gnostr_filter_set_set_id().
 *
 * Returns: (transfer full): a new #GnostrFilterSet.
 */
GnostrFilterSet *gnostr_filter_set_new(void);

/**
 * gnostr_filter_set_new_with_name:
 * @name: human-readable name
 *
 * Convenience constructor that creates a new filter set and sets its name.
 *
 * Returns: (transfer full): a new #GnostrFilterSet.
 */
GnostrFilterSet *gnostr_filter_set_new_with_name(const gchar *name);

/**
 * gnostr_filter_set_clone:
 * @self: a filter set
 *
 * Create a deep copy of @self. All string and array fields are duplicated.
 *
 * Returns: (transfer full): a new #GnostrFilterSet independent of @self.
 */
GnostrFilterSet *gnostr_filter_set_clone(GnostrFilterSet *self);

/* ------------------------------------------------------------------------
 * Identity / metadata
 * ------------------------------------------------------------------------ */

const gchar *gnostr_filter_set_get_id(GnostrFilterSet *self);
void         gnostr_filter_set_set_id(GnostrFilterSet *self, const gchar *id);

const gchar *gnostr_filter_set_get_name(GnostrFilterSet *self);
void         gnostr_filter_set_set_name(GnostrFilterSet *self, const gchar *name);

const gchar *gnostr_filter_set_get_description(GnostrFilterSet *self);
void         gnostr_filter_set_set_description(GnostrFilterSet *self, const gchar *description);

const gchar *gnostr_filter_set_get_icon(GnostrFilterSet *self);
void         gnostr_filter_set_set_icon(GnostrFilterSet *self, const gchar *icon);

const gchar *gnostr_filter_set_get_color(GnostrFilterSet *self);
void         gnostr_filter_set_set_color(GnostrFilterSet *self, const gchar *color);

GnostrFilterSetSource gnostr_filter_set_get_source(GnostrFilterSet *self);
void                  gnostr_filter_set_set_source(GnostrFilterSet *self,
                                                   GnostrFilterSetSource source);

/* ------------------------------------------------------------------------
 * Filter criteria
 *
 * All collections use GStrv / GArray semantics:
 *   - setters take ownership of nothing; they copy the input.
 *   - getters return internal data; do NOT free or modify.
 *   - a NULL / 0-length value means "no constraint on this field".
 * ------------------------------------------------------------------------ */

/**
 * gnostr_filter_set_get_authors:
 * @self: a filter set
 *
 * Returns: (transfer none) (nullable): NULL-terminated array of hex pubkey
 *   strings, or NULL if no author constraint is set.
 */
const gchar * const *gnostr_filter_set_get_authors(GnostrFilterSet *self);
/**
 * gnostr_filter_set_set_authors:
 * @self: a filter set
 * @authors: (nullable) (array zero-terminated=1): hex pubkeys, or NULL
 */
void gnostr_filter_set_set_authors(GnostrFilterSet *self, const gchar * const *authors);

/**
 * gnostr_filter_set_get_kinds:
 * @self: a filter set
 * @n_kinds: (out) (optional): number of kinds returned
 *
 * Returns: (transfer none) (nullable) (array length=n_kinds): kinds array,
 *   or NULL if no kind constraint is set.
 */
const gint *gnostr_filter_set_get_kinds(GnostrFilterSet *self, gsize *n_kinds);
/**
 * gnostr_filter_set_set_kinds:
 * @self: a filter set
 * @kinds: (nullable) (array length=n_kinds): kinds, or NULL
 * @n_kinds: number of elements in @kinds (ignored if @kinds is NULL)
 */
void gnostr_filter_set_set_kinds(GnostrFilterSet *self, const gint *kinds, gsize n_kinds);

const gchar * const *gnostr_filter_set_get_hashtags(GnostrFilterSet *self);
void gnostr_filter_set_set_hashtags(GnostrFilterSet *self, const gchar * const *hashtags);

/**
 * gnostr_filter_set_get_ids:
 * @self: a filter set
 *
 * Event-id constraint (standard Nostr filter `ids`). Typically used by
 * bookmark/thread timelines where the view is pinned to a finite set of
 * events.
 *
 * Returns: (transfer none) (nullable): NULL-terminated array of hex event
 *   ids, or NULL if no id constraint is set.
 */
const gchar * const *gnostr_filter_set_get_ids(GnostrFilterSet *self);
void gnostr_filter_set_set_ids(GnostrFilterSet *self, const gchar * const *ids);

/**
 * gnostr_filter_set_get_excluded_authors:
 * @self: a filter set
 *
 * Client-side exclusion of hex pubkeys. This is *not* a Nostr filter
 * field — relays still return notes from these authors. Consumers must
 * apply the exclusion when rendering or after fetch. Primarily used to
 * implement "muted (inverse)" predefined feeds.
 *
 * Returns: (transfer none) (nullable): NULL-terminated array of hex
 *   pubkeys to exclude, or NULL.
 */
const gchar * const *gnostr_filter_set_get_excluded_authors(GnostrFilterSet *self);
void gnostr_filter_set_set_excluded_authors(GnostrFilterSet *self,
                                             const gchar * const *authors);

gint64 gnostr_filter_set_get_since(GnostrFilterSet *self);
void   gnostr_filter_set_set_since(GnostrFilterSet *self, gint64 since);

gint64 gnostr_filter_set_get_until(GnostrFilterSet *self);
void   gnostr_filter_set_set_until(GnostrFilterSet *self, gint64 until);

gint gnostr_filter_set_get_limit(GnostrFilterSet *self);
void gnostr_filter_set_set_limit(GnostrFilterSet *self, gint limit);

/**
 * gnostr_filter_set_is_empty:
 * @self: a filter set
 *
 * Returns: TRUE if the filter set has no active criteria (no authors, kinds,
 *   hashtags, since/until or limit). Metadata fields (name, description)
 *   are ignored.
 */
gboolean gnostr_filter_set_is_empty(GnostrFilterSet *self);

/**
 * gnostr_filter_set_equal:
 * @a: (nullable): first filter set
 * @b: (nullable): second filter set
 *
 * Deep structural comparison of the id + metadata + filter criteria.
 * NULL equals NULL; NULL does not equal a non-NULL set.
 *
 * Returns: TRUE if both sets have identical state.
 */
gboolean gnostr_filter_set_equal(GnostrFilterSet *a, GnostrFilterSet *b);

/* ------------------------------------------------------------------------
 * Serialization
 *
 * JSON schema:
 *   {
 *     "id":          "<string>",
 *     "name":        "<string>",
 *     "description": "<string>",
 *     "icon":        "<string>",
 *     "color":       "#rrggbb",
 *     "source":      "custom" | "predefined",
 *     "filter": {
 *       "authors":   ["<hex>", ...],
 *       "kinds":     [<int>, ...],
 *       "hashtags":  ["<tag>", ...],
 *       "since":     <int>,
 *       "until":     <int>,
 *       "limit":     <int>
 *     }
 *   }
 *
 * Unknown keys are preserved on load and emitted again on save, so newer
 * schema revisions round-trip cleanly through an older client.
 * ------------------------------------------------------------------------ */

/**
 * gnostr_filter_set_to_json:
 * @self: a filter set
 *
 * Serialize @self to a newline-free JSON string.
 *
 * Returns: (transfer full): JSON string, free with g_free().
 */
gchar *gnostr_filter_set_to_json(GnostrFilterSet *self);

/**
 * gnostr_filter_set_new_from_json:
 * @json: JSON string produced by gnostr_filter_set_to_json() or
 *   compatible schema
 * @error: (out) (nullable): error location
 *
 * Parse a filter set from its JSON representation. All fields are optional;
 * unset fields fall back to defaults (empty strings, no criteria).
 *
 * Returns: (transfer full) (nullable): a new #GnostrFilterSet, or NULL on
 *   parse error (see @error).
 */
GnostrFilterSet *gnostr_filter_set_new_from_json(const gchar *json, GError **error);

/**
 * gnostr_filter_set_to_variant:
 * @self: a filter set
 *
 * Serialize @self to a floating #GVariant of type "a{sv}" suitable for
 * storing in GSettings keys with a corresponding schema type.
 *
 * Returns: (transfer none): a floating variant (caller should sink).
 */
GVariant *gnostr_filter_set_to_variant(GnostrFilterSet *self);

/**
 * gnostr_filter_set_new_from_variant:
 * @variant: a variant of type "a{sv}" produced by
 *   gnostr_filter_set_to_variant() or compatible schema
 *
 * Parse a filter set from a variant dictionary.
 *
 * Returns: (transfer full) (nullable): a new #GnostrFilterSet, or NULL if
 *   @variant is of the wrong type.
 */
GnostrFilterSet *gnostr_filter_set_new_from_variant(GVariant *variant);

G_END_DECLS

#endif /* GNOSTR_FILTER_SET_H */
