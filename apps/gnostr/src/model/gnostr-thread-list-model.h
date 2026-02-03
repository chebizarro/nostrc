/**
 * GnostrThreadListModel - A GListModel for thread events
 *
 * Provides thread events as a GListModel for use with GtkListView.
 * Items are GnNostrEventItem objects with reply_depth for indentation.
 *
 * Part of the GListModel Facade for Thread/Timeline Widget Lifecycle (nostrc-833y)
 */

#ifndef GNOSTR_THREAD_LIST_MODEL_H
#define GNOSTR_THREAD_LIST_MODEL_H

#include <gio/gio.h>
#include "gn-nostr-event-item.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_THREAD_LIST_MODEL (gnostr_thread_list_model_get_type())

G_DECLARE_FINAL_TYPE(GnostrThreadListModel, gnostr_thread_list_model, GNOSTR, THREAD_LIST_MODEL, GObject)

/**
 * gnostr_thread_list_model_new:
 *
 * Creates a new thread list model.
 *
 * Returns: (transfer full): A new #GnostrThreadListModel implementing #GListModel
 */
GnostrThreadListModel *gnostr_thread_list_model_new(void);

/**
 * gnostr_thread_list_model_append:
 * @self: the thread list model
 * @item: (transfer none): a #GnNostrEventItem to append
 *
 * Appends an event item to the model. The model takes a reference
 * to the item. Emits items-changed signal.
 */
void gnostr_thread_list_model_append(GnostrThreadListModel *self, GnNostrEventItem *item);

/**
 * gnostr_thread_list_model_clear:
 * @self: the thread list model
 *
 * Removes all items from the model. Emits items-changed signal.
 */
void gnostr_thread_list_model_clear(GnostrThreadListModel *self);

/**
 * gnostr_thread_list_model_set_root:
 * @self: the thread list model
 * @root_id: (nullable): the root event ID for this thread (64-char hex)
 *
 * Sets the root event ID for this thread. Clears existing items.
 */
void gnostr_thread_list_model_set_root(GnostrThreadListModel *self, const char *root_id);

/**
 * gnostr_thread_list_model_get_root:
 * @self: the thread list model
 *
 * Returns: (transfer none) (nullable): The root event ID or NULL.
 */
const char *gnostr_thread_list_model_get_root(GnostrThreadListModel *self);

/**
 * gnostr_thread_list_model_insert_sorted:
 * @self: the thread list model
 * @item: (transfer none): a #GnNostrEventItem to insert
 *
 * Inserts an event item maintaining tree traversal order based on
 * parent_id relationships and created_at timestamps.
 * Emits items-changed signal.
 */
void gnostr_thread_list_model_insert_sorted(GnostrThreadListModel *self, GnNostrEventItem *item);

/**
 * gnostr_thread_list_model_get_item_by_event_id:
 * @self: the thread list model
 * @event_id: the event ID to find (64-char hex)
 *
 * Finds an item by its event ID.
 *
 * Returns: (transfer none) (nullable): The item or NULL if not found.
 */
GnNostrEventItem *gnostr_thread_list_model_get_item_by_event_id(GnostrThreadListModel *self,
                                                                  const char *event_id);

/**
 * gnostr_thread_list_model_contains:
 * @self: the thread list model
 * @event_id: the event ID to check (64-char hex)
 *
 * Checks if an event is already in the model.
 *
 * Returns: TRUE if the event is in the model.
 */
gboolean gnostr_thread_list_model_contains(GnostrThreadListModel *self, const char *event_id);

G_END_DECLS

#endif /* GNOSTR_THREAD_LIST_MODEL_H */
