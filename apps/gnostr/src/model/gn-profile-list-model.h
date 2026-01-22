#ifndef GN_PROFILE_LIST_MODEL_H
#define GN_PROFILE_LIST_MODEL_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define GN_TYPE_PROFILE_LIST_MODEL (gn_profile_list_model_get_type())

G_DECLARE_FINAL_TYPE(GnProfileListModel, gn_profile_list_model, GN, PROFILE_LIST_MODEL, GObject)

/**
 * GnProfileSortMode:
 * @GN_PROFILE_SORT_RECENT: Sort by most recently seen (created_at of kind:0)
 * @GN_PROFILE_SORT_ALPHABETICAL: Sort by display name alphabetically
 * @GN_PROFILE_SORT_FOLLOWING: Show followed profiles first
 *
 * Sorting options for the profile list.
 */
typedef enum {
    GN_PROFILE_SORT_RECENT,
    GN_PROFILE_SORT_ALPHABETICAL,
    GN_PROFILE_SORT_FOLLOWING
} GnProfileSortMode;

/**
 * gn_profile_list_model_new:
 *
 * Creates a new profile list model backed by nostrdb.
 *
 * Returns: (transfer full): A new #GnProfileListModel implementing #GListModel
 */
GnProfileListModel *gn_profile_list_model_new(void);

/**
 * gn_profile_list_model_load_profiles:
 * @self: the profile list model
 *
 * Load all cached kind:0 profiles from nostrdb.
 * This is an async operation that emits items-changed when complete.
 */
void gn_profile_list_model_load_profiles(GnProfileListModel *self);

/**
 * gn_profile_list_model_filter:
 * @self: the profile list model
 * @search_text: text to filter by (name, NIP-05, bio), or NULL to clear filter
 *
 * Filter the visible profiles by search text.
 */
void gn_profile_list_model_filter(GnProfileListModel *self, const char *search_text);

/**
 * gn_profile_list_model_set_sort_mode:
 * @self: the profile list model
 * @mode: the sorting mode
 *
 * Change the sort order of profiles.
 */
void gn_profile_list_model_set_sort_mode(GnProfileListModel *self, GnProfileSortMode mode);

/**
 * gn_profile_list_model_get_sort_mode:
 * @self: the profile list model
 *
 * Returns: The current sort mode.
 */
GnProfileSortMode gn_profile_list_model_get_sort_mode(GnProfileListModel *self);

/**
 * gn_profile_list_model_set_following_set:
 * @self: the profile list model
 * @pubkeys: (nullable): NULL-terminated array of pubkey hex strings that are followed
 *
 * Set the list of pubkeys the current user follows, for sorting/display purposes.
 */
void gn_profile_list_model_set_following_set(GnProfileListModel *self, const char **pubkeys);

/**
 * gn_profile_list_model_is_loading:
 * @self: the profile list model
 *
 * Returns: TRUE if profiles are currently being loaded.
 */
gboolean gn_profile_list_model_is_loading(GnProfileListModel *self);

/**
 * gn_profile_list_model_get_total_count:
 * @self: the profile list model
 *
 * Returns: Total number of profiles in the database (before filtering).
 */
guint gn_profile_list_model_get_total_count(GnProfileListModel *self);

G_END_DECLS

#endif /* GN_PROFILE_LIST_MODEL_H */
