/**
 * GnostrCommunityListView - NIP-72 Moderated Community Browser
 *
 * Displays a scrollable list of moderated communities with search/filter
 * capabilities. Uses GnostrCommunityCard for individual community display.
 */

#ifndef GNOSTR_COMMUNITY_LIST_VIEW_H
#define GNOSTR_COMMUNITY_LIST_VIEW_H

#include <gtk/gtk.h>
#include "../util/nip72_communities.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_COMMUNITY_LIST_VIEW (gnostr_community_list_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrCommunityListView, gnostr_community_list_view, GNOSTR, COMMUNITY_LIST_VIEW, GtkWidget)

/**
 * Signals:
 * "community-selected" (gchar* a_tag, gpointer user_data)
 *   - Emitted when user selects a community to view
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user wants to view a community creator's profile
 * "join-community" (gchar* a_tag, gpointer user_data)
 *   - Emitted when user clicks join on a community
 * "leave-community" (gchar* a_tag, gpointer user_data)
 *   - Emitted when user clicks leave on a community
 */

typedef struct _GnostrCommunityListView GnostrCommunityListView;

/**
 * Create a new community list view
 */
GnostrCommunityListView *gnostr_community_list_view_new(void);

/**
 * Add or update a community in the list
 * @self: the community list view
 * @community: community data (copied internally)
 */
void gnostr_community_list_view_upsert_community(GnostrCommunityListView *self,
                                                   const GnostrCommunity *community);

/**
 * Remove a community from the list
 * @self: the community list view
 * @a_tag: "a" tag of community to remove (34550:pubkey:d-tag)
 */
void gnostr_community_list_view_remove_community(GnostrCommunityListView *self,
                                                   const char *a_tag);

/**
 * Clear all communities from the list
 */
void gnostr_community_list_view_clear(GnostrCommunityListView *self);

/**
 * Set the loading state
 * @self: the community list view
 * @is_loading: TRUE to show loading spinner
 */
void gnostr_community_list_view_set_loading(GnostrCommunityListView *self,
                                             gboolean is_loading);

/**
 * Set the empty state
 * @self: the community list view
 * @is_empty: TRUE to show empty state
 */
void gnostr_community_list_view_set_empty(GnostrCommunityListView *self,
                                           gboolean is_empty);

/**
 * Get the currently selected community "a" tag
 * @return: "a" tag or NULL if none selected
 */
const char *gnostr_community_list_view_get_selected_a_tag(GnostrCommunityListView *self);

/**
 * Set the current user's pubkey (for showing joined communities)
 */
void gnostr_community_list_view_set_user_pubkey(GnostrCommunityListView *self,
                                                  const char *pubkey_hex);

/**
 * Mark a community as joined
 * @self: the community list view
 * @a_tag: "a" tag of the community
 * @is_joined: TRUE if user has joined
 */
void gnostr_community_list_view_set_joined(GnostrCommunityListView *self,
                                            const char *a_tag,
                                            gboolean is_joined);

G_END_DECLS

#endif /* GNOSTR_COMMUNITY_LIST_VIEW_H */
