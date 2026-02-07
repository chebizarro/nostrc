#ifndef GNOSTR_TAG_LIST_H
#define GNOSTR_TAG_LIST_H

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

GType gnostr_tag_get_type(void) G_GNUC_CONST;

/**
 * gnostr_tag_new:
 * @key: The tag key
 * @values: (nullable) (array zero-terminated=1): NULL-terminated array of values
 *
 * Creates a new GNostrTag with the given key and values.
 *
 * Returns: (transfer full): A new #GNostrTag
 */
GNostrTag *gnostr_tag_new(const gchar *key, const gchar * const *values);

/**
 * gnostr_tag_copy:
 * @tag: (nullable): A #GNostrTag to copy
 *
 * Creates a deep copy of a GNostrTag.
 *
 * Returns: (transfer full) (nullable): A new #GNostrTag, or %NULL if @tag is %NULL
 */
GNostrTag *gnostr_tag_copy(const GNostrTag *tag);

/**
 * gnostr_tag_free:
 * @tag: (nullable): A #GNostrTag to free
 *
 * Frees a GNostrTag and all its contents.
 */
void gnostr_tag_free(GNostrTag *tag);

/**
 * gnostr_tag_get_key:
 * @tag: A #GNostrTag
 *
 * Returns: (transfer none) (nullable): The tag key
 */
const gchar *gnostr_tag_get_key(const GNostrTag *tag);

/**
 * gnostr_tag_get_values:
 * @tag: A #GNostrTag
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): The values array
 */
const gchar * const *gnostr_tag_get_values(const GNostrTag *tag);

/**
 * gnostr_tag_get_n_values:
 * @tag: A #GNostrTag
 *
 * Returns: The number of values
 */
guint gnostr_tag_get_n_values(const GNostrTag *tag);

/**
 * gnostr_tag_get_value:
 * @tag: A #GNostrTag
 * @index: The value index
 *
 * Returns: (transfer none) (nullable): The value at @index, or %NULL if out of bounds
 */
const gchar *gnostr_tag_get_value(const GNostrTag *tag, guint index);

/**
 * GNostrTagList:
 *
 * A list of Nostr tags implementing #GListModel.
 * G-prefixed to avoid collision with core libnostr NostrTagList.
 */
#define GNOSTR_TYPE_TAG_LIST (gnostr_tag_list_get_type())
G_DECLARE_FINAL_TYPE(GNostrTagList, gnostr_tag_list, GNOSTR, TAG_LIST, GObject)

GNostrTagList *gnostr_tag_list_new(void);
void           gnostr_tag_list_append(GNostrTagList *list, GNostrTag *tag);
GNostrTag     *gnostr_tag_list_get(GNostrTagList *list, guint index);
void           gnostr_tag_list_remove(GNostrTagList *list, guint index);
GPtrArray     *gnostr_tag_list_find_by_key(GNostrTagList *list, const gchar *key);
guint          gnostr_tag_list_get_length(GNostrTagList *list);

G_END_DECLS

#endif /* GNOSTR_TAG_LIST_H */
