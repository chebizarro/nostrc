#ifndef GNOSTR_TAG_LIST_H
#define GNOSTR_TAG_LIST_H

#include "nostr-gobject-export.h"
#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * GNostrTag:
 * @key: The tag key (e.g., "e", "p", "t")
 * @values: NULL-terminated array of tag values
 *
 * Represents a single Nostr tag as defined in NIP-01.
 * Tags are arrays where the first element is the key and
 * subsequent elements are values.
 *
 * G-prefixed to avoid collision with core libnostr NostrTag.
 *
 * Since: 0.1
 */
typedef struct {
    gchar *key;       /* e.g., "e", "p", "t" */
    gchar **values;   /* NULL-terminated array */
} GNostrTag;

#define GNOSTR_TYPE_TAG (gnostr_tag_get_type())

NOSTR_GOBJECT_API GType gnostr_tag_get_type(void) G_GNUC_CONST;

/**
 * gnostr_tag_new:
 * @key: The tag key
 * @values: (nullable) (array zero-terminated=1): NULL-terminated array of values
 *
 * Creates a new GNostrTag with the given key and values.
 *
 * Returns: (transfer full): A new #GNostrTag
 */
NOSTR_GOBJECT_API GNostrTag *gnostr_tag_new(const gchar *key, const gchar * const *values);

/**
 * gnostr_tag_copy:
 * @tag: (nullable): A #GNostrTag to copy
 *
 * Creates a deep copy of a GNostrTag.
 *
 * Returns: (transfer full) (nullable): A new #GNostrTag, or %NULL if @tag is %NULL
 */
NOSTR_GOBJECT_API GNostrTag *gnostr_tag_copy(const GNostrTag *tag);

/**
 * gnostr_tag_free:
 * @tag: (nullable): A #GNostrTag to free
 *
 * Frees a GNostrTag and all its contents.
 */
NOSTR_GOBJECT_API void gnostr_tag_free(GNostrTag *tag);

/**
 * gnostr_tag_get_key:
 * @tag: A #GNostrTag
 *
 * Returns: (transfer none) (nullable): The tag key
 */
NOSTR_GOBJECT_API const gchar *gnostr_tag_get_key(const GNostrTag *tag);

/**
 * gnostr_tag_get_values:
 * @tag: A #GNostrTag
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): The values array
 */
NOSTR_GOBJECT_API const gchar * const *gnostr_tag_get_values(const GNostrTag *tag);

/**
 * gnostr_tag_get_n_values:
 * @tag: A #GNostrTag
 *
 * Returns: The number of values
 */
NOSTR_GOBJECT_API guint gnostr_tag_get_n_values(const GNostrTag *tag);

/**
 * gnostr_tag_get_value:
 * @tag: A #GNostrTag
 * @index: The value index
 *
 * Returns: (transfer none) (nullable): The value at @index, or %NULL if out of bounds
 */
NOSTR_GOBJECT_API const gchar *gnostr_tag_get_value(const GNostrTag *tag, guint index);

/**
 * GNostrTagList:
 *
 * A list of Nostr tags implementing #GListModel.
 * G-prefixed to avoid collision with core libnostr NostrTagList.
 */
#define GNOSTR_TYPE_TAG_LIST (gnostr_tag_list_get_type())
NOSTR_GOBJECT_API G_DECLARE_FINAL_TYPE(GNostrTagList, gnostr_tag_list, GNOSTR, TAG_LIST, GObject)

NOSTR_GOBJECT_API GNostrTagList *gnostr_tag_list_new(void);
NOSTR_GOBJECT_API void           gnostr_tag_list_append(GNostrTagList *list, GNostrTag *tag);
NOSTR_GOBJECT_API GNostrTag     *gnostr_tag_list_get(GNostrTagList *list, guint index);
NOSTR_GOBJECT_API void           gnostr_tag_list_remove(GNostrTagList *list, guint index);
NOSTR_GOBJECT_API GPtrArray     *gnostr_tag_list_find_by_key(GNostrTagList *list, const gchar *key);
NOSTR_GOBJECT_API guint          gnostr_tag_list_get_length(GNostrTagList *list);

G_END_DECLS

#endif /* GNOSTR_TAG_LIST_H */
