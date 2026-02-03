/**
 * GnostrThreadListModel - A GListModel for thread events
 *
 * Simple GListModel implementation for thread events. Stores GnNostrEventItem
 * objects which already have reply_depth for indentation support.
 *
 * Part of the GListModel Facade for Thread/Timeline Widget Lifecycle (nostrc-833y)
 */

#include "gnostr-thread-list-model.h"
#include <string.h>

struct _GnostrThreadListModel {
    GObject parent_instance;

    /* Items array - GnNostrEventItem* */
    GPtrArray *items;

    /* Event ID lookup - event_id_hex -> GnNostrEventItem* (borrowed refs) */
    GHashTable *id_lookup;

    /* Root event ID for this thread */
    char *root_id;
};

static void gnostr_thread_list_model_list_model_iface_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnostrThreadListModel, gnostr_thread_list_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, gnostr_thread_list_model_list_model_iface_init))

/* ============== GListModel Interface ============== */

static GType
gnostr_thread_list_model_get_item_type(GListModel *model)
{
    (void)model;
    return GN_TYPE_NOSTR_EVENT_ITEM;
}

static guint
gnostr_thread_list_model_get_n_items(GListModel *model)
{
    GnostrThreadListModel *self = GNOSTR_THREAD_LIST_MODEL(model);
    return self->items ? self->items->len : 0;
}

static gpointer
gnostr_thread_list_model_get_item(GListModel *model, guint position)
{
    GnostrThreadListModel *self = GNOSTR_THREAD_LIST_MODEL(model);

    if (!self->items || position >= self->items->len)
        return NULL;

    GnNostrEventItem *item = g_ptr_array_index(self->items, position);
    return g_object_ref(item);
}

static void
gnostr_thread_list_model_list_model_iface_init(GListModelInterface *iface)
{
    iface->get_item_type = gnostr_thread_list_model_get_item_type;
    iface->get_n_items = gnostr_thread_list_model_get_n_items;
    iface->get_item = gnostr_thread_list_model_get_item;
}

/* ============== GObject Lifecycle ============== */

static void
gnostr_thread_list_model_finalize(GObject *object)
{
    GnostrThreadListModel *self = GNOSTR_THREAD_LIST_MODEL(object);

    g_clear_pointer(&self->items, g_ptr_array_unref);
    g_clear_pointer(&self->id_lookup, g_hash_table_destroy);
    g_free(self->root_id);

    G_OBJECT_CLASS(gnostr_thread_list_model_parent_class)->finalize(object);
}

static void
gnostr_thread_list_model_class_init(GnostrThreadListModelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gnostr_thread_list_model_finalize;
}

static void
gnostr_thread_list_model_init(GnostrThreadListModel *self)
{
    self->items = g_ptr_array_new_with_free_func(g_object_unref);
    self->id_lookup = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->root_id = NULL;
}

/* ============== Public API ============== */

GnostrThreadListModel *
gnostr_thread_list_model_new(void)
{
    return g_object_new(GNOSTR_TYPE_THREAD_LIST_MODEL, NULL);
}

void
gnostr_thread_list_model_append(GnostrThreadListModel *self, GnNostrEventItem *item)
{
    g_return_if_fail(GNOSTR_IS_THREAD_LIST_MODEL(self));
    g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(item));

    const char *event_id = gn_nostr_event_item_get_event_id(item);

    /* Skip duplicates */
    if (event_id && g_hash_table_contains(self->id_lookup, event_id))
        return;

    guint position = self->items->len;
    g_ptr_array_add(self->items, g_object_ref(item));

    if (event_id) {
        g_hash_table_insert(self->id_lookup, g_strdup(event_id), item);
    }

    g_list_model_items_changed(G_LIST_MODEL(self), position, 0, 1);
}

void
gnostr_thread_list_model_clear(GnostrThreadListModel *self)
{
    g_return_if_fail(GNOSTR_IS_THREAD_LIST_MODEL(self));

    guint old_len = self->items->len;
    if (old_len == 0)
        return;

    g_ptr_array_set_size(self->items, 0);
    g_hash_table_remove_all(self->id_lookup);

    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, 0);
}

void
gnostr_thread_list_model_set_root(GnostrThreadListModel *self, const char *root_id)
{
    g_return_if_fail(GNOSTR_IS_THREAD_LIST_MODEL(self));

    /* Clear existing items when setting new root */
    gnostr_thread_list_model_clear(self);

    g_free(self->root_id);
    self->root_id = g_strdup(root_id);
}

const char *
gnostr_thread_list_model_get_root(GnostrThreadListModel *self)
{
    g_return_val_if_fail(GNOSTR_IS_THREAD_LIST_MODEL(self), NULL);
    return self->root_id;
}

/**
 * Find insertion position for sorted insert.
 * Thread events are ordered: root first, then children grouped by parent
 * with siblings sorted by created_at ascending.
 */
static guint
find_insertion_position(GnostrThreadListModel *self, GnNostrEventItem *item)
{
    const char *parent_id = gn_nostr_event_item_get_parent_id(item);
    gint64 created_at = gn_nostr_event_item_get_created_at(item);

    /* If no parent (root event), insert at position 0 */
    if (!parent_id || !*parent_id) {
        return 0;
    }

    /* Find parent position */
    guint parent_pos = G_MAXUINT;
    for (guint i = 0; i < self->items->len; i++) {
        GnNostrEventItem *existing = g_ptr_array_index(self->items, i);
        const char *existing_id = gn_nostr_event_item_get_event_id(existing);
        if (existing_id && g_strcmp0(existing_id, parent_id) == 0) {
            parent_pos = i;
            break;
        }
    }

    /* Parent not found - append at end */
    if (parent_pos == G_MAXUINT) {
        return self->items->len;
    }

    /* Find position after parent and its existing children.
     * Insert after all siblings with earlier created_at. */
    guint insert_pos = parent_pos + 1;
    guint parent_depth = gn_nostr_event_item_get_reply_depth(
        g_ptr_array_index(self->items, parent_pos));
    guint item_depth = gn_nostr_event_item_get_reply_depth(item);

    for (guint i = parent_pos + 1; i < self->items->len; i++) {
        GnNostrEventItem *existing = g_ptr_array_index(self->items, i);
        guint existing_depth = gn_nostr_event_item_get_reply_depth(existing);

        /* Stop if we've left the parent's subtree */
        if (existing_depth <= parent_depth) {
            break;
        }

        /* Only compare with direct siblings (same depth) */
        if (existing_depth == item_depth) {
            const char *existing_parent = gn_nostr_event_item_get_parent_id(existing);
            if (g_strcmp0(existing_parent, parent_id) == 0) {
                gint64 existing_time = gn_nostr_event_item_get_created_at(existing);
                if (existing_time <= created_at) {
                    insert_pos = i + 1;
                }
            }
        } else {
            /* Continue past deeper nested items */
            insert_pos = i + 1;
        }
    }

    return insert_pos;
}

void
gnostr_thread_list_model_insert_sorted(GnostrThreadListModel *self, GnNostrEventItem *item)
{
    g_return_if_fail(GNOSTR_IS_THREAD_LIST_MODEL(self));
    g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(item));

    const char *event_id = gn_nostr_event_item_get_event_id(item);

    /* Skip duplicates */
    if (event_id && g_hash_table_contains(self->id_lookup, event_id))
        return;

    guint position = find_insertion_position(self, item);
    g_ptr_array_insert(self->items, position, g_object_ref(item));

    if (event_id) {
        g_hash_table_insert(self->id_lookup, g_strdup(event_id), item);
    }

    g_list_model_items_changed(G_LIST_MODEL(self), position, 0, 1);
}

GnNostrEventItem *
gnostr_thread_list_model_get_item_by_event_id(GnostrThreadListModel *self,
                                               const char *event_id)
{
    g_return_val_if_fail(GNOSTR_IS_THREAD_LIST_MODEL(self), NULL);
    g_return_val_if_fail(event_id != NULL, NULL);

    return g_hash_table_lookup(self->id_lookup, event_id);
}

gboolean
gnostr_thread_list_model_contains(GnostrThreadListModel *self, const char *event_id)
{
    g_return_val_if_fail(GNOSTR_IS_THREAD_LIST_MODEL(self), FALSE);
    g_return_val_if_fail(event_id != NULL, FALSE);

    return g_hash_table_contains(self->id_lookup, event_id);
}
