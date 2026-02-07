#ifndef NOSTR_TAG_LIST_H
#define NOSTR_TAG_LIST_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * NostrTag:
 * @key: The tag key (e.g., "e", "p", "t")
 * @values: NULL-terminated array of tag values
 *
 * Represents a single Nostr tag as defined in NIP-01.
 * Tags are arrays where the first element is the key and
 * subsequent elements are values.
 *
 * Since: 0.1
 */
typedef struct {
    gchar *key;       /* e.g., "e", "p", "t" */
    gchar **values;   /* NULL-terminated array */
} NostrTag;

#define NOSTR_TYPE_TAG (nostr_tag_get_type())

GType nostr_tag_get_type(void) G_GNUC_CONST;

/**
 * nostr_tag_new:
 * @key: The tag key
 * @values: (nullable) (array zero-terminated=1): NULL-terminated array of values
 *
 * Creates a new NostrTag with the given key and values.
 *
 * Returns: (transfer full): A new #NostrTag
 *
 * Since: 0.1
 */
NostrTag *nostr_tag_new(const gchar *key, const gchar * const *values);

/**
 * nostr_tag_copy:
 * @tag: (nullable): A #NostrTag to copy
 *
 * Creates a deep copy of a NostrTag.
 *
 * Returns: (transfer full) (nullable): A new #NostrTag, or %NULL if @tag is %NULL
 *
 * Since: 0.1
 */
NostrTag *nostr_tag_copy(const NostrTag *tag);

/**
 * nostr_tag_free:
 * @tag: (nullable): A #NostrTag to free
 *
 * Frees a NostrTag and all its contents.
 *
 * Since: 0.1
 */
void nostr_tag_free(NostrTag *tag);

/**
 * nostr_tag_get_key:
 * @tag: A #NostrTag
 *
 * Gets the tag key.
 *
 * Returns: (transfer none) (nullable): The tag key
 *
 * Since: 0.1
 */
const gchar *nostr_tag_get_key(const NostrTag *tag);

/**
 * nostr_tag_get_values:
 * @tag: A #NostrTag
 *
 * Gets the tag values array.
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): The values array
 *
 * Since: 0.1
 */
const gchar * const *nostr_tag_get_values(const NostrTag *tag);

/**
 * nostr_tag_get_n_values:
 * @tag: A #NostrTag
 *
 * Gets the number of values in the tag.
 *
 * Returns: The number of values
 *
 * Since: 0.1
 */
guint nostr_tag_get_n_values(const NostrTag *tag);

/**
 * nostr_tag_get_value:
 * @tag: A #NostrTag
 * @index: The value index
 *
 * Gets a specific value by index.
 *
 * Returns: (transfer none) (nullable): The value at @index, or %NULL if out of bounds
 *
 * Since: 0.1
 */
const gchar *nostr_tag_get_value(const NostrTag *tag, guint index);

/**
 * NostrTagList:
 *
 * A list of Nostr tags implementing #GListModel.
 *
 * NostrTagList provides a GObject-based container for tags that integrates
 * with GTK's list widgets through the GListModel interface.
 *
 * Since: 0.1
 */
#define NOSTR_TYPE_TAG_LIST (nostr_tag_list_get_type())
G_DECLARE_FINAL_TYPE(NostrTagList, nostr_tag_list, NOSTR, TAG_LIST, GObject)

/**
 * nostr_tag_list_new:
 *
 * Creates a new empty tag list.
 *
 * Returns: (transfer full): A new #NostrTagList
 *
 * Since: 0.1
 */
NostrTagList *nostr_tag_list_new(void);

/**
 * nostr_tag_list_append:
 * @list: A #NostrTagList
 * @tag: (transfer none): A #NostrTag to append (copied)
 *
 * Appends a copy of the tag to the list.
 * Emits #GListModel::items-changed.
 *
 * Since: 0.1
 */
void nostr_tag_list_append(NostrTagList *list, NostrTag *tag);

/**
 * nostr_tag_list_get:
 * @list: A #NostrTagList
 * @index: The index of the tag to get
 *
 * Gets the tag at the specified index.
 *
 * Note: This returns a borrowed reference to the internal tag.
 * Use nostr_tag_copy() if you need to keep the tag beyond the
 * lifetime of the list.
 *
 * Returns: (transfer none) (nullable): The tag at @index, or %NULL if out of bounds
 *
 * Since: 0.1
 */
NostrTag *nostr_tag_list_get(NostrTagList *list, guint index);

/**
 * nostr_tag_list_remove:
 * @list: A #NostrTagList
 * @index: The index of the tag to remove
 *
 * Removes the tag at the specified index.
 * Emits #GListModel::items-changed if the index is valid.
 *
 * Since: 0.1
 */
void nostr_tag_list_remove(NostrTagList *list, guint index);

/**
 * nostr_tag_list_find_by_key:
 * @list: A #NostrTagList
 * @key: The tag key to search for
 *
 * Finds all tags with the specified key.
 *
 * Returns: (transfer container) (element-type NostrTag): A #GPtrArray of
 *          borrowed #NostrTag pointers. Free the array with g_ptr_array_unref()
 *          but do not free the contained tags.
 *
 * Since: 0.1
 */
GPtrArray *nostr_tag_list_find_by_key(NostrTagList *list, const gchar *key);

/**
 * nostr_tag_list_get_length:
 * @list: A #NostrTagList
 *
 * Gets the number of tags in the list.
 *
 * Returns: The number of tags
 *
 * Since: 0.1
 */
guint nostr_tag_list_get_length(NostrTagList *list);

G_END_DECLS

#endif /* NOSTR_TAG_LIST_H */
