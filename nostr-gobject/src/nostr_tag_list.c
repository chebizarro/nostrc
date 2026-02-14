#include "nostr_tag_list.h"
#include <glib.h>

/* GNostrTag boxed type implementation */

GNostrTag *
gnostr_tag_new(const gchar *key, const gchar * const *values)
{
    GNostrTag *tag = g_new0(GNostrTag, 1);
    tag->key = g_strdup(key);
    tag->values = g_strdupv((gchar **)values);
    return tag;
}

GNostrTag *
gnostr_tag_copy(const GNostrTag *tag)
{
    if (tag == NULL)
        return NULL;
    return gnostr_tag_new(tag->key, (const gchar * const *)tag->values);
}

void
gnostr_tag_free(GNostrTag *tag)
{
    if (tag == NULL)
        return;
    g_free(tag->key);
    g_strfreev(tag->values);
    g_free(tag);
}

G_DEFINE_BOXED_TYPE(GNostrTag, gnostr_tag, gnostr_tag_copy, gnostr_tag_free)

const gchar *
gnostr_tag_get_key(const GNostrTag *tag)
{
    g_return_val_if_fail(tag != NULL, NULL);
    return tag->key;
}

const gchar * const *
gnostr_tag_get_values(const GNostrTag *tag)
{
    g_return_val_if_fail(tag != NULL, NULL);
    return (const gchar * const *)tag->values;
}

guint
gnostr_tag_get_n_values(const GNostrTag *tag)
{
    g_return_val_if_fail(tag != NULL, 0);
    if (tag->values == NULL)
        return 0;
    return g_strv_length(tag->values);
}

const gchar *
gnostr_tag_get_value(const GNostrTag *tag, guint index)
{
    g_return_val_if_fail(tag != NULL, NULL);
    if (tag->values == NULL)
        return NULL;
    guint len = g_strv_length(tag->values);
    if (index >= len)
        return NULL;
    return tag->values[index];
}

/* GNostrTagList GObject implementation with GListModel interface */

struct _GNostrTagList {
    GObject parent_instance;
    GPtrArray *tags;
};

static void gnostr_tag_list_list_model_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GNostrTagList, gnostr_tag_list, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                              gnostr_tag_list_list_model_init))

static void
gnostr_tag_list_finalize(GObject *object)
{
    GNostrTagList *self = GNOSTR_TAG_LIST(object);
    g_ptr_array_unref(self->tags);
    G_OBJECT_CLASS(gnostr_tag_list_parent_class)->finalize(object);
}

static void
gnostr_tag_list_class_init(GNostrTagListClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gnostr_tag_list_finalize;
}

static void
gnostr_tag_list_init(GNostrTagList *self)
{
    self->tags = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_tag_free);
}

/* GListModel interface implementation */

static GType
gnostr_tag_list_get_item_type(GListModel *list G_GNUC_UNUSED)
{
    return GNOSTR_TYPE_TAG;
}

static guint
gnostr_tag_list_get_n_items(GListModel *list)
{
    GNostrTagList *self = GNOSTR_TAG_LIST(list);
    return self->tags->len;
}

static gpointer
gnostr_tag_list_get_item(GListModel *list, guint position)
{
    GNostrTagList *self = GNOSTR_TAG_LIST(list);
    if (position >= self->tags->len)
        return NULL;
    GNostrTag *tag = g_ptr_array_index(self->tags, position);
    return gnostr_tag_copy(tag);
}

static void
gnostr_tag_list_list_model_init(GListModelInterface *iface)
{
    iface->get_item_type = gnostr_tag_list_get_item_type;
    iface->get_n_items = gnostr_tag_list_get_n_items;
    iface->get_item = gnostr_tag_list_get_item;
}

/* Public API */

GNostrTagList *
gnostr_tag_list_new(void)
{
    return g_object_new(GNOSTR_TYPE_TAG_LIST, NULL);
}

void
gnostr_tag_list_append(GNostrTagList *list, GNostrTag *tag)
{
    g_return_if_fail(GNOSTR_IS_TAG_LIST(list));
    g_return_if_fail(tag != NULL);

    GNostrTag *copy = gnostr_tag_copy(tag);
    g_ptr_array_add(list->tags, copy);
    g_list_model_items_changed(G_LIST_MODEL(list), list->tags->len - 1, 0, 1);
}

GNostrTag *
gnostr_tag_list_get(GNostrTagList *list, guint index)
{
    g_return_val_if_fail(GNOSTR_IS_TAG_LIST(list), NULL);
    if (index >= list->tags->len)
        return NULL;
    return g_ptr_array_index(list->tags, index);
}

void
gnostr_tag_list_remove(GNostrTagList *list, guint index)
{
    g_return_if_fail(GNOSTR_IS_TAG_LIST(list));
    if (index >= list->tags->len)
        return;

    g_ptr_array_remove_index(list->tags, index);
    g_list_model_items_changed(G_LIST_MODEL(list), index, 1, 0);
}

GPtrArray *
gnostr_tag_list_find_by_key(GNostrTagList *list, const gchar *key)
{
    g_return_val_if_fail(GNOSTR_IS_TAG_LIST(list), NULL);
    g_return_val_if_fail(key != NULL, NULL);

    GPtrArray *result = g_ptr_array_new();

    for (guint i = 0; i < list->tags->len; i++) {
        GNostrTag *tag = g_ptr_array_index(list->tags, i);
        if (g_strcmp0(tag->key, key) == 0) {
            g_ptr_array_add(result, tag);
        }
    }

    return result;
}

guint
gnostr_tag_list_get_length(GNostrTagList *list)
{
    g_return_val_if_fail(GNOSTR_IS_TAG_LIST(list), 0);
    return list->tags->len;
}
