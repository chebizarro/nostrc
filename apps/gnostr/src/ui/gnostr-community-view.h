/**
 * GnostrCommunityView - NIP-72 Moderated Community Feed View
 *
 * Displays a community's approved posts with:
 * - Community header (image, name, description, rules)
 * - Moderator list
 * - Feed of approved posts (kind 4550 -> kind 1 references)
 * - Compose button for creating new posts
 * - Moderation actions for moderators
 */

#ifndef GNOSTR_COMMUNITY_VIEW_H
#define GNOSTR_COMMUNITY_VIEW_H

#include <gtk/gtk.h>
#include "../util/nip72_communities.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_COMMUNITY_VIEW (gnostr_community_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrCommunityView, gnostr_community_view, GNOSTR, COMMUNITY_VIEW, GtkWidget)

/**
 * Signals:
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks to view a profile
 * "open-note" (gchar* event_id, gpointer user_data)
 *   - Emitted when user clicks to view a note's thread
 * "compose-post" (gchar* a_tag, gpointer user_data)
 *   - Emitted when user wants to compose a post for this community
 * "approve-post" (gchar* event_id, gchar* author_pubkey, gchar* a_tag, gpointer user_data)
 *   - Emitted when moderator approves a pending post
 * "reject-post" (gchar* event_id, gpointer user_data)
 *   - Emitted when moderator rejects a pending post
 * "join-community" (gchar* a_tag, gpointer user_data)
 *   - Emitted when user clicks to join
 * "leave-community" (gchar* a_tag, gpointer user_data)
 *   - Emitted when user clicks to leave
 * "zap-requested" (gchar* event_id, gchar* pubkey_hex, gchar* lud16, gpointer user_data)
 *   - Emitted when user wants to zap a post
 */

typedef struct _GnostrCommunityView GnostrCommunityView;

/**
 * Create a new community view widget
 */
GnostrCommunityView *gnostr_community_view_new(void);

/**
 * Set the community data
 * @self: the view widget
 * @community: community data (copied internally)
 */
void gnostr_community_view_set_community(GnostrCommunityView *self,
                                          const GnostrCommunity *community);

/**
 * Get the community "a" tag
 */
const char *gnostr_community_view_get_a_tag(GnostrCommunityView *self);

/**
 * Add an approved post to the feed
 * @self: the view widget
 * @post: the community post data
 */
void gnostr_community_view_add_post(GnostrCommunityView *self,
                                     const GnostrCommunityPost *post);

/**
 * Add a pending post (only visible to moderators)
 * @self: the view widget
 * @post: the pending post data
 */
void gnostr_community_view_add_pending_post(GnostrCommunityView *self,
                                             const GnostrCommunityPost *post);

/**
 * Remove a post from the feed
 * @self: the view widget
 * @event_id: the post's event ID
 */
void gnostr_community_view_remove_post(GnostrCommunityView *self,
                                        const char *event_id);

/**
 * Mark a post as approved (moves from pending to approved)
 * @self: the view widget
 * @event_id: the post's event ID
 * @approval_id: the approval event's ID
 */
void gnostr_community_view_mark_approved(GnostrCommunityView *self,
                                          const char *event_id,
                                          const char *approval_id);

/**
 * Clear all posts from the feed
 */
void gnostr_community_view_clear_posts(GnostrCommunityView *self);

/**
 * Set the loading state
 * @self: the view widget
 * @is_loading: TRUE to show loading indicator
 */
void gnostr_community_view_set_loading(GnostrCommunityView *self,
                                        gboolean is_loading);

/**
 * Set the empty state message
 * @self: the view widget
 * @is_empty: TRUE to show empty state
 */
void gnostr_community_view_set_empty(GnostrCommunityView *self,
                                      gboolean is_empty);

/**
 * Set the current user's pubkey
 * @self: the view widget
 * @pubkey: user's pubkey (hex), or NULL if not logged in
 */
void gnostr_community_view_set_user_pubkey(GnostrCommunityView *self,
                                            const char *pubkey);

/**
 * Set whether the current user has joined this community
 */
void gnostr_community_view_set_joined(GnostrCommunityView *self,
                                       gboolean is_joined);

/**
 * Set whether the current user is a moderator
 */
void gnostr_community_view_set_is_moderator(GnostrCommunityView *self,
                                             gboolean is_moderator);

/**
 * Set whether to show the pending posts section (moderators only)
 */
void gnostr_community_view_set_show_pending(GnostrCommunityView *self,
                                             gboolean show_pending);

/**
 * Update post author profile information
 * @self: the view widget
 * @pubkey: author's pubkey
 * @display_name: display name
 * @avatar_url: avatar URL
 */
void gnostr_community_view_update_author_profile(GnostrCommunityView *self,
                                                   const char *pubkey,
                                                   const char *display_name,
                                                   const char *avatar_url);

/**
 * Get the scrolled window for scroll position monitoring
 */
GtkWidget *gnostr_community_view_get_scrolled_window(GnostrCommunityView *self);

G_END_DECLS

#endif /* GNOSTR_COMMUNITY_VIEW_H */
