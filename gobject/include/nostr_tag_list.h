#ifndef NOSTR_TAG_LIST_H
#define NOSTR_TAG_LIST_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* NostrTag boxed type - represents a single Nostr tag */
typedef struct {
    gchar *key;       /* e.g., "e", "p", "t" */
    gchar **values;   /* NULL-terminated array */
} NostrTag;

#define NOSTR_TYPE_TAG (nostr_tag_get_type())

GType nostr_tag_get_type(void) G_GNUC_CONST;

NostrTag *nostr_tag_new(const gchar *key, const gchar * const *values);
NostrTag *nostr_tag_copy(const NostrTag *tag);
void nostr_tag_free(NostrTag *tag);

const gchar *nostr_tag_get_key(const NostrTag *tag);
const gchar * const *nostr_tag_get_values(const NostrTag *tag);
guint nostr_tag_get_n_values(const NostrTag *tag);
const gchar *nostr_tag_get_value(const NostrTag *tag, guint index);

/* NostrTagList - GObject implementing GListModel */
#define NOSTR_TYPE_TAG_LIST (nostr_tag_list_get_type())
G_DECLARE_FINAL_TYPE(NostrTagList, nostr_tag_list, NOSTR, TAG_LIST, GObject)

NostrTagList *nostr_tag_list_new(void);
void nostr_tag_list_append(NostrTagList *list, NostrTag *tag);
NostrTag *nostr_tag_list_get(NostrTagList *list, guint index);
void nostr_tag_list_remove(NostrTagList *list, guint index);
GPtrArray *nostr_tag_list_find_by_key(NostrTagList *list, const gchar *key);
guint nostr_tag_list_get_length(NostrTagList *list);

G_END_DECLS

#endif /* NOSTR_TAG_LIST_H */
