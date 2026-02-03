#ifndef GN_FOLLOW_LIST_MODEL_H
#define GN_FOLLOW_LIST_MODEL_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define GN_TYPE_FOLLOW_LIST_MODEL (gn_follow_list_model_get_type())

G_DECLARE_FINAL_TYPE(GnFollowListModel, gn_follow_list_model, GN, FOLLOW_LIST_MODEL, GObject)

/**
 * GnFollowListItem:
 *
 * Represents a single entry in a follow list with profile metadata.
 */
#define GN_TYPE_FOLLOW_LIST_ITEM (gn_follow_list_item_get_type())

G_DECLARE_FINAL_TYPE(GnFollowListItem, gn_follow_list_item, GN, FOLLOW_LIST_ITEM, GObject)

/* GnFollowListItem accessors */
const gchar *gn_follow_list_item_get_pubkey(GnFollowListItem *self);
const gchar *gn_follow_list_item_get_relay_hint(GnFollowListItem *self);
const gchar *gn_follow_list_item_get_petname(GnFollowListItem *self);
const gchar *gn_follow_list_item_get_display_name(GnFollowListItem *self);
const gchar *gn_follow_list_item_get_nip05(GnFollowListItem *self);
const gchar *gn_follow_list_item_get_picture_url(GnFollowListItem *self);
gboolean gn_follow_list_item_get_profile_loaded(GnFollowListItem *self);

/**
 * gn_follow_list_model_new:
 *
 * Creates a new follow list model.
 *
 * Returns: (transfer full): A new #GnFollowListModel implementing #GListModel
 */
GnFollowListModel *gn_follow_list_model_new(void);

/**
 * gn_follow_list_model_load_for_pubkey:
 * @self: the follow list model
 * @pubkey_hex: 64-char hex pubkey of the user whose follows to load
 *
 * Load follows for a specific user. First tries nostrdb cache, then fetches
 * from relays asynchronously. Emits items-changed when loaded.
 */
void gn_follow_list_model_load_for_pubkey(GnFollowListModel *self,
                                           const gchar *pubkey_hex);

/**
 * gn_follow_list_model_load_for_pubkey_async:
 * @self: the follow list model
 * @pubkey_hex: 64-char hex pubkey of the user whose follows to load
 * @cancellable: (nullable): optional cancellable
 * @callback: (nullable): callback when load completes
 * @user_data: user data for callback
 *
 * Async version that fetches from relays if not cached.
 */
void gn_follow_list_model_load_for_pubkey_async(GnFollowListModel *self,
                                                 const gchar *pubkey_hex,
                                                 GCancellable *cancellable,
                                                 GAsyncReadyCallback callback,
                                                 gpointer user_data);

/**
 * gn_follow_list_model_load_for_pubkey_finish:
 * @self: the follow list model
 * @result: the async result
 * @error: (out) (nullable): error location
 *
 * Finish async load operation.
 *
 * Returns: TRUE on success
 */
gboolean gn_follow_list_model_load_for_pubkey_finish(GnFollowListModel *self,
                                                      GAsyncResult *result,
                                                      GError **error);

/**
 * gn_follow_list_model_clear:
 * @self: the follow list model
 *
 * Clear all items from the model.
 */
void gn_follow_list_model_clear(GnFollowListModel *self);

/**
 * gn_follow_list_model_filter:
 * @self: the follow list model
 * @search_text: (nullable): text to filter by (name, pubkey), or NULL to clear
 *
 * Filter visible items by search text.
 */
void gn_follow_list_model_filter(GnFollowListModel *self, const gchar *search_text);

/**
 * gn_follow_list_model_is_loading:
 * @self: the follow list model
 *
 * Returns: TRUE if the model is currently loading data.
 */
gboolean gn_follow_list_model_is_loading(GnFollowListModel *self);

/**
 * gn_follow_list_model_get_pubkey:
 * @self: the follow list model
 *
 * Returns: (transfer none) (nullable): The pubkey whose follows are loaded.
 */
const gchar *gn_follow_list_model_get_pubkey(GnFollowListModel *self);

/**
 * gn_follow_list_model_get_total_count:
 * @self: the follow list model
 *
 * Returns: Total number of follows (before filtering).
 */
guint gn_follow_list_model_get_total_count(GnFollowListModel *self);

/**
 * gn_follow_list_model_set_visible_range:
 * @self: the follow list model
 * @start: first visible item index
 * @end: last visible item index (exclusive)
 *
 * Sets the visible range for viewport-aware profile loading.
 * Only profiles for items in this range (plus a prefetch buffer)
 * will be loaded. Call this when scroll position changes.
 */
void gn_follow_list_model_set_visible_range(GnFollowListModel *self,
                                             guint start,
                                             guint end);

/**
 * gn_follow_list_model_get_visible_range:
 * @self: the follow list model
 * @start: (out) (nullable): first visible item index
 * @end: (out) (nullable): last visible item index (exclusive)
 *
 * Gets the current visible range.
 *
 * Returns: TRUE if there are visible items.
 */
gboolean gn_follow_list_model_get_visible_range(GnFollowListModel *self,
                                                  guint *start,
                                                  guint *end);

G_END_DECLS

#endif /* GN_FOLLOW_LIST_MODEL_H */
