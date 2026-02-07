#include "nostr_tag_list.h"
#include <glib.h>

/* NostrTag boxed type implementation */

NostrTag *
nostr_tag_new(const gchar *key, const gchar * const *values)
{
    NostrTag *tag = g_new0(NostrTag, 1);
    tag->key = g_strdup(key);
    tag->values = g_strdupv((gchar **)values);
    return tag;
}

NostrTag *
nostr_tag_copy(const NostrTag *tag)
{
    if (tag == NULL)
        return NULL;
    return nostr_tag_new(tag->key, (const gchar * const *)tag->values);
}

void
nostr_tag_free(NostrTag *tag)
{
    if (tag == NULL)
        return;
    g_free(tag->key);
    g_strfreev(tag->values);
    g_free(tag);
}

G_DEFINE_BOXED_TYPE(NostrTag, nostr_tag, nostr_tag_copy, nostr_tag_free)

const gchar *
nostr_tag_get_key(const NostrTag *tag)
{
    g_return_val_if_fail(tag != NULL, NULL);
    return tag->key;
}

const gchar * const *
nostr_tag_get_values(const NostrTag *tag)
{
    g_return_val_if_fail(tag != NULL, NULL);
    return (const gchar * const *)tag->values;
}

guint
nostr_tag_get_n_values(const NostrTag *tag)
{
    g_return_val_if_fail(tag != NULL, 0);
    if (tag->values == NULL)
        return 0;
    return g_strv_length(tag->values);
}

const gchar *
nostr_tag_get_value(const NostrTag *tag, guint index)
{
    g_return_val_if_fail(tag != NULL, NULL);
    if (tag->values == NULL)
        return NULL;
    guint len = g_strv_length(tag->values);
    if (index >= len)
        return NULL;
    return tag->values[index];
}

/* NostrTagList GObject implementation with GListModel interface */

struct _NostrTagList {
    GObject parent_instance;
    GPtrArray *tags;
};

static void nostr_tag_list_list_model_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(NostrTagList, nostr_tag_list, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                              nostr_tag_list_list_model_init))

static void
nostr_tag_list_finalize(GObject *object)
{
    NostrTagList *self = NOSTR_TAG_LIST(object);
    g_ptr_array_unref(self->tags);
    G_OBJECT_CLASS(nostr_tag_list_parent_class)->finalize(object);
}

static void
nostr_tag_list_class_init(NostrTagListClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = nostr_tag_list_finalize;
}

static void
nostr_tag_list_init(NostrTagList *self)
{
    self->tags = g_ptr_array_new_with_free_func((GDestroyNotify)nostr_tag_free);
}

/* GListModel interface implementation */

static GType
nostr_tag_list_get_item_type(GListModel *list G_GNUC_UNUSED)
{
    return NOSTR_TYPE_TAG;
}

static guint
nostr_tag_list_get_n_items(GListModel *list)
{
    NostrTagList *self = NOSTR_TAG_LIST(list);
    return self->tags->len;
}

static gpointer
nostr_tag_list_get_item(GListModel *list, guint position)
{
    NostrTagList *self = NOSTR_TAG_LIST(list);
    if (position >= self->tags->len)
        return NULL;
    NostrTag *tag = g_ptr_array_index(self->tags, position);
    return nostr_tag_copy(tag);
}

static void
nostr_tag_list_list_model_init(GListModelInterface *iface)
{
    iface->get_item_type = nostr_tag_list_get_item_type;
    iface->get_n_items = nostr_tag_list_get_n_items;
    iface->get_item = nostr_tag_list_get_item;
}

/* Public API */

NostrTagList *
nostr_tag_list_new(void)
{
    return g_object_new(NOSTR_TYPE_TAG_LIST, NULL);
}

void
nostr_tag_list_append(NostrTagList *list, NostrTag *tag)
{
    g_return_if_fail(NOSTR_IS_TAG_LIST(list));
    g_return_if_fail(tag != NULL);

    NostrTag *copy = nostr_tag_copy(tag);
    g_ptr_array_add(list->tags, copy);
    g_list_model_items_changed(G_LIST_MODEL(list), list->tags->len - 1, 0, 1);
}

NostrTag *
nostr_tag_list_get(NostrTagList *list, guint index)
{
    g_return_val_if_fail(NOSTR_IS_TAG_LIST(list), NULL);
    if (index >= list->tags->len)
        return NULL;
    return g_ptr_array_index(list->tags, index);
}

void
nostr_tag_list_remove(NostrTagList *list, guint index)
{
    g_return_if_fail(NOSTR_IS_TAG_LIST(list));
    if (index >= list->tags->len)
        return;

    g_ptr_array_remove_index(list->tags, index);
    g_list_model_items_changed(G_LIST_MODEL(list), index, 1, 0);
}

GPtrArray *
nostr_tag_list_find_by_key(NostrTagList *list, const gchar *key)
{
    g_return_val_if_fail(NOSTR_IS_TAG_LIST(list), NULL);
    g_return_val_if_fail(key != NULL, NULL);

    GPtrArray *result = g_ptr_array_new();

    for (guint i = 0; i < list->tags->len; i++) {
        NostrTag *tag = g_ptr_array_index(list->tags, i);
        if (g_strcmp0(tag->key, key) == 0) {
            g_ptr_array_add(result, tag);
        }
    }

    return result;
}

guint
nostr_tag_list_get_length(NostrTagList *list)
{
    g_return_val_if_fail(NOSTR_IS_TAG_LIST(list), 0);
    return list->tags->len;
}
